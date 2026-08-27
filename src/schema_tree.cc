// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/schema_tree.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "yang/schema_path.h"
#include "yang/identity_feature.h"

namespace yang::semantic {
namespace {

std::optional<SchemaNodeKind> NodeKind(std::string_view keyword) {
  static const std::unordered_map<std::string_view, SchemaNodeKind> kinds{
      {"container", SchemaNodeKind::kContainer}, {"list", SchemaNodeKind::kList},
      {"leaf", SchemaNodeKind::kLeaf}, {"leaf-list", SchemaNodeKind::kLeafList},
      {"choice", SchemaNodeKind::kChoice}, {"case", SchemaNodeKind::kCase},
      {"anydata", SchemaNodeKind::kAnydata}, {"anyxml", SchemaNodeKind::kAnyxml},
      {"rpc", SchemaNodeKind::kRpc}, {"action", SchemaNodeKind::kAction},
      {"input", SchemaNodeKind::kInput}, {"output", SchemaNodeKind::kOutput},
      {"notification", SchemaNodeKind::kNotification}};
  const auto found = kinds.find(keyword);
  return found == kinds.end() ? std::nullopt : std::optional<SchemaNodeKind>(found->second);
}

const Statement* FindChild(const ResolvedModule& source, const Statement& parent,
                           std::string_view keyword) {
  for (const StatementId id : parent.children) {
    const Statement& child = source.syntax->Get(id);
    if (child.keyword == keyword) return &child;
  }
  return nullptr;
}

std::optional<bool> BooleanChild(const ResolvedModule& source,
                                 const Statement& parent,
                                 std::string_view keyword) {
  const Statement* child = FindChild(source, parent, keyword);
  if (!child || !child->argument) return std::nullopt;
  if (*child->argument == "true") return true;
  if (*child->argument == "false") return false;
  return std::nullopt;
}

std::optional<std::string> StringChild(const ResolvedModule& source,
                                       const Statement& parent,
                                       std::string_view keyword) {
  const Statement* child = FindChild(source, parent, keyword);
  return child && child->argument ? child->argument : std::nullopt;
}

std::vector<std::string> StringChildren(const ResolvedModule& source,
                                        const Statement& parent,
                                        std::string_view keyword) {
  std::vector<std::string> values;
  for (const StatementId child_id : parent.children) {
    const Statement& child = source.syntax->Get(child_id);
    if (child.keyword == keyword && child.argument) {
      values.push_back(*child.argument);
    }
  }
  return values;
}

std::optional<std::uint64_t> UnsignedChild(const ResolvedModule& source,
                                           const Statement& parent,
                                           std::string_view keyword) {
  const auto text = StringChild(source, parent, keyword);
  if (!text || *text == "unbounded") return std::nullopt;
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(text->data(),
                                            text->data() + text->size(), value);
  return error == std::errc{} && end == text->data() + text->size()
             ? std::optional<std::uint64_t>(value)
             : std::nullopt;
}

void AppendMustConstraints(const std::shared_ptr<const ResolvedModule>& source,
                           const Statement& statement, SchemaNode& node) {
  for (const StatementId child_id : statement.children) {
    if (source->syntax->Get(child_id).keyword == "must") {
      node.must_constraints.push_back(
          {.source_module = source.get(), .statement = child_id});
    }
  }
}

void AppendWhenConstraints(const std::shared_ptr<const ResolvedModule>& source,
                           const Statement& statement, SchemaNode& node,
                           std::optional<SchemaNodeId> context_node = std::nullopt) {
  for (const StatementId child_id : statement.children) {
    if (source->syntax->Get(child_id).keyword == "when") {
      node.when_constraints.push_back({source.get(), child_id, context_node});
    }
  }
}

bool Augmentable(SchemaNodeKind kind) {
  return kind == SchemaNodeKind::kContainer || kind == SchemaNodeKind::kList ||
         kind == SchemaNodeKind::kChoice || kind == SchemaNodeKind::kCase ||
         kind == SchemaNodeKind::kInput || kind == SchemaNodeKind::kOutput ||
         kind == SchemaNodeKind::kNotification;
}

bool StatementEnabled(const ResolvedModule& source, const Statement& statement,
                      const FeatureSet* features,
                      DiagnosticSink& diagnostics, bool& valid) {
  if (!features) return true;
  for (const StatementId child_id : statement.children) {
    const Statement& condition = source.syntax->Get(child_id);
    if (condition.keyword != "if-feature" || !condition.argument) continue;
    const auto enabled = features->Evaluate(source, *condition.argument);
    if (!enabled) {
      diagnostics.Report({DiagnosticCode::kInvalidFeatureExpression,
                          DiagnosticSeverity::kError,
                          fmt::format("invalid if-feature expression '{}'",
                                      *condition.argument),
                          condition.range});
      valid = false;
      return false;
    }
    if (!*enabled) return false;
  }
  return true;
}

using GroupingScope = std::unordered_map<std::string, Symbol>;

struct GroupingKey {
  const ResolvedModule* source;
  StatementId statement;
  bool operator==(const GroupingKey&) const = default;
};

struct GroupingKeyHash {
  std::size_t operator()(const GroupingKey& key) const noexcept {
    return std::hash<const void*>{}(key.source) ^
           (std::hash<StatementId>{}(key.statement) << 1U);
  }
};

}  // namespace

SchemaNodeId SchemaTree::Add(SchemaNode node) {
  if (nodes_.size() >= kInvalidSchemaNodeId) throw std::length_error("too many schema nodes");
  const auto id = static_cast<SchemaNodeId>(nodes_.size());
  node.id = id;
  nodes_.push_back(std::move(node));
  if (nodes_.back().parent) Get(*nodes_.back().parent).children.push_back(id);
  else roots_.push_back(id);
  return id;
}

const SchemaNode& SchemaTree::Get(SchemaNodeId id) const { return nodes_.at(id); }
SchemaNode& SchemaTree::Get(SchemaNodeId id) { return nodes_.at(id); }

void SchemaTree::DisableSubtree(SchemaNodeId id) {
  SchemaNode& node = Get(id);
  if (!node.supported) return;
  node.supported = false;
  const std::vector<SchemaNodeId> children = node.children;
  for (const SchemaNodeId child : children) DisableSubtree(child);
  auto& siblings = node.parent ? Get(*node.parent).children : roots_;
  siblings.erase(std::remove(siblings.begin(), siblings.end(), id), siblings.end());
}

std::optional<SchemaNodeId> SchemaTree::FindChild(
    std::optional<SchemaNodeId> parent, const SchemaName& name) const {
  const auto& candidates = parent ? Get(*parent).children : roots_;
  for (const SchemaNodeId id : candidates) if (Get(id).name == name) return id;
  return std::nullopt;
}

std::optional<SchemaTree> SchemaBuilder::Build(
    const SemanticContext& semantics, const TypeContext& types,
    const FeatureSet* features) {
  return BuildFor(semantics, types, semantics.root(), features);
}

std::optional<SchemaTree> SchemaBuilder::BuildFor(
    const SemanticContext& semantics, const TypeContext& types,
    std::shared_ptr<const ModuleSymbols> selected_symbols,
    const FeatureSet* features) {
  if (!selected_symbols) return std::nullopt;
  SchemaTree result;
  bool valid = true;
  std::unordered_set<GroupingKey, GroupingKeyHash> expanding;

  struct ResolvedGrouping {
    Symbol symbol;
    std::shared_ptr<const ModuleSymbols> owner;
  };

  std::function<std::optional<ResolvedGrouping>(
      const std::shared_ptr<const ModuleSymbols>&,
      const std::shared_ptr<const ResolvedModule>&, std::string_view,
      const std::vector<GroupingScope>&)> resolve_grouping;
  resolve_grouping = [&](const std::shared_ptr<const ModuleSymbols>& owner,
                         const std::shared_ptr<const ResolvedModule>& source,
                         std::string_view name,
                         const std::vector<GroupingScope>& scopes)
      -> std::optional<ResolvedGrouping> {
    const std::size_t colon = name.find(':');
    if (colon == std::string_view::npos) {
      for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
        const auto found = scope->find(std::string(name));
        if (found != scope->end()) return ResolvedGrouping{found->second, owner};
      }
      if (auto symbol = owner->Find(SymbolKind::kGrouping, name)) return ResolvedGrouping{*symbol, owner};
      return std::nullopt;
    }
    const std::string_view prefix = name.substr(0, colon);
    const std::string_view local = name.substr(colon + 1);
    if (prefix == source->prefix || prefix == owner->module()->prefix) {
      if (auto symbol = owner->Find(SymbolKind::kGrouping, local)) return ResolvedGrouping{*symbol, owner};
      return std::nullopt;
    }
    const auto imported = source->imports.find(std::string(prefix));
    if (imported == source->imports.end()) return std::nullopt;
    auto imported_owner = semantics.FindModule(*imported->second);
    if (!imported_owner) return std::nullopt;
    if (auto symbol = imported_owner->Find(SymbolKind::kGrouping, local)) {
      return ResolvedGrouping{*symbol, std::move(imported_owner)};
    }
    return std::nullopt;
  };

  const auto add_node = [&](SchemaNode node, SourceRange range) -> std::optional<SchemaNodeId> {
    if (result.FindChild(node.parent, node.name)) {
      diagnostics_.Report({DiagnosticCode::kDuplicateSchemaNode, DiagnosticSeverity::kError,
          fmt::format("duplicate schema node '{}:{}'", node.name.module, node.name.local_name), range});
      valid = false;
      return std::nullopt;
    }
    return result.Add(std::move(node));
  };

  std::function<void(const std::shared_ptr<const ModuleSymbols>&,
                     const std::shared_ptr<const ModuleSymbols>&,
                     const std::shared_ptr<const ResolvedModule>&, const Statement&,
                     std::optional<SchemaNodeId>, SchemaNodeOrigin, StatementId,
                     std::vector<GroupingScope>&, std::vector<SchemaNodeId>*)> expand_children;

  expand_children = [&](const std::shared_ptr<const ModuleSymbols>& owner,
                        const std::shared_ptr<const ModuleSymbols>& namespace_owner,
                        const std::shared_ptr<const ResolvedModule>& source,
                        const Statement& parent_statement,
                        std::optional<SchemaNodeId> parent_node,
                        SchemaNodeOrigin origin, StatementId instantiation,
                        std::vector<GroupingScope>& scopes,
                        std::vector<SchemaNodeId>* created) {
    GroupingScope local_groupings;
    for (const StatementId child_id : parent_statement.children) {
      const Statement& child = source->syntax->Get(child_id);
      if (child.keyword == "grouping" && child.argument) {
        local_groupings.emplace(*child.argument,
            Symbol{SymbolKind::kGrouping, *child.argument, source, child_id});
      }
    }
    const bool pushed = !local_groupings.empty();
    if (pushed) scopes.push_back(std::move(local_groupings));

    for (const StatementId child_id : parent_statement.children) {
      const Statement& statement = source->syntax->Get(child_id);
      if (!StatementEnabled(*source, statement, features, diagnostics_, valid)) {
        continue;
      }
      if (statement.keyword == "uses" && statement.argument) {
        const auto grouping = resolve_grouping(owner, source, *statement.argument, scopes);
        if (!grouping) continue;  // Symbol resolution already diagnosed this.
        const GroupingKey key{grouping->symbol.source_module.get(), grouping->symbol.statement};
        if (!expanding.insert(key).second) {
          diagnostics_.Report({DiagnosticCode::kGroupingCycle, DiagnosticSeverity::kError,
                               fmt::format("recursive grouping use '{}'", *statement.argument), statement.range});
          valid = false;
          continue;
        }
        const Statement& declaration = grouping->symbol.source_module->syntax->Get(grouping->symbol.statement);
        std::vector<GroupingScope> grouping_scopes;
        GroupingScope top;
        for (const auto& [name, symbol] : grouping->owner->symbols(SymbolKind::kGrouping)) {
          top.emplace(name, symbol);
        }
        grouping_scopes.push_back(std::move(top));
        std::vector<SchemaNodeId> uses_nodes;
        expand_children(grouping->owner, namespace_owner,
                        grouping->symbol.source_module, declaration, parent_node,
                        SchemaNodeOrigin::kUses, statement.id, grouping_scopes,
                        &uses_nodes);
        if (FindChild(*source, statement, "when")) {
          std::function<void(SchemaNodeId)> propagate_when;
          propagate_when = [&](SchemaNodeId node_id) {
            AppendWhenConstraints(source, statement, result.Get(node_id),
                                  parent_node);
            for (const SchemaNodeId child : result.Get(node_id).children) {
              propagate_when(child);
            }
          };
          for (const SchemaNodeId node_id : uses_nodes) propagate_when(node_id);
        }
        expanding.erase(key);

        for (const StatementId refine_id : statement.children) {
          const Statement& refine = source->syntax->Get(refine_id);
          if (refine.keyword != "refine" || !refine.argument) continue;
          std::string_view path = *refine.argument;
          std::optional<SchemaNodeId> current;
          std::vector<SchemaNodeId> candidates = uses_nodes;
          bool found = true;
          while (!path.empty()) {
            const std::size_t slash = path.find('/');
            std::string_view segment = path.substr(0, slash);
            const std::size_t colon = segment.find(':');
            if (colon != std::string_view::npos) segment.remove_prefix(colon + 1);
            std::optional<SchemaNodeId> next;
            for (const SchemaNodeId candidate : candidates) {
              if (result.Get(candidate).name.local_name == segment) { next = candidate; break; }
            }
            if (!next) { found = false; break; }
            current = next;
            candidates = result.Get(*current).children;
            if (slash == std::string_view::npos) break;
            path.remove_prefix(slash + 1);
          }
          if (!found || !current) {
            diagnostics_.Report({DiagnosticCode::kInvalidRefineTarget, DiagnosticSeverity::kError,
                                 fmt::format("refine target '{}' was not produced by this uses", *refine.argument),
                                 refine.range});
            valid = false;
            continue;
          }
          SchemaNode& target = result.Get(*current);
          if (const auto config = BooleanChild(*source, refine, "config")) target.declared_config = config;
          if (const auto mandatory = BooleanChild(*source, refine, "mandatory")) {
            target.declared_mandatory = mandatory;
            target.mandatory = *mandatory;
          }
          if (const auto default_value = StringChild(*source, refine, "default")) target.default_value = default_value;
          if (const auto defaults = StringChildren(*source, refine, "default");
              !defaults.empty()) {
            target.default_values = defaults;
          }
          AppendMustConstraints(source, refine, target);
        }
        for (const StatementId augment_id : statement.children) {
          const Statement& augment = source->syntax->Get(augment_id);
          if (augment.keyword != "augment" || !augment.argument) continue;
          std::string_view path = *augment.argument;
          std::optional<SchemaNodeId> target;
          std::vector<SchemaNodeId> candidates = uses_nodes;
          bool complete = true;
          while (!path.empty()) {
            const std::size_t slash = path.find('/');
            std::string_view segment = path.substr(0, slash);
            const std::size_t colon = segment.find(':');
            if (colon != std::string_view::npos) segment.remove_prefix(colon + 1);
            std::optional<SchemaNodeId> next;
            for (const SchemaNodeId candidate : candidates) {
              if (result.Get(candidate).name.local_name == segment) { next = candidate; break; }
            }
            if (!next) { complete = false; break; }
            target = next;
            candidates = result.Get(*target).children;
            if (slash == std::string_view::npos) break;
            path.remove_prefix(slash + 1);
          }
          if (!complete || !target || !Augmentable(result.Get(*target).kind)) {
            diagnostics_.Report({DiagnosticCode::kInvalidAugmentTarget, DiagnosticSeverity::kError,
                                 fmt::format("uses augment target '{}' is invalid", *augment.argument),
                                 augment.range});
            valid = false;
            continue;
          }
          std::vector<SchemaNodeId> augmented_nodes;
          expand_children(owner, namespace_owner, source, augment, *target,
                          SchemaNodeOrigin::kAugment, augment.id, scopes,
                          &augmented_nodes);
          if (FindChild(*source, augment, "when")) {
            for (const SchemaNodeId node_id : augmented_nodes) {
              AppendWhenConstraints(source, augment, result.Get(node_id),
                                    *target);
            }
          }
        }
        if (created) created->insert(created->end(), uses_nodes.begin(), uses_nodes.end());
        continue;
      }

      const auto kind = NodeKind(statement.keyword);
      if (!kind || (!statement.argument && statement.keyword != "input" &&
                    statement.keyword != "output")) continue;
      const std::string node_name = statement.argument.value_or(statement.keyword);
      std::optional<SchemaNodeId> actual_parent = parent_node;
      if (parent_node && result.Get(*parent_node).kind == SchemaNodeKind::kChoice &&
          *kind != SchemaNodeKind::kCase) {
        SchemaNode implicit;
        implicit.parent = parent_node;
        implicit.kind = SchemaNodeKind::kCase;
        implicit.origin = SchemaNodeOrigin::kImplicitCase;
        implicit.name = {namespace_owner->module()->name, node_name};
        implicit.source_module = source;
        implicit.declaration = statement.id;
        implicit.instantiation = instantiation;
        actual_parent = add_node(std::move(implicit), statement.range);
        if (!actual_parent) continue;
        if (created) created->push_back(*actual_parent);
      }

      SchemaNode node;
      node.parent = actual_parent;
      node.kind = *kind;
      node.origin = origin;
      node.name = {namespace_owner->module()->name, node_name};
      node.source_module = source;
      node.declaration = statement.id;
      node.instantiation = instantiation;
      node.declared_config = BooleanChild(*source, statement, "config");
      node.declared_mandatory = BooleanChild(*source, statement, "mandatory");
      node.mandatory = node.declared_mandatory.value_or(false);
      node.presence_container = StringChild(*source, statement, "presence").has_value();
      node.default_value = StringChild(*source, statement, "default");
      node.default_values = StringChildren(*source, statement, "default");
      node.units = StringChild(*source, statement, "units");
      node.min_elements = UnsignedChild(*source, statement, "min-elements");
      node.max_elements = UnsignedChild(*source, statement, "max-elements");
      node.max_elements_unbounded =
          StringChild(*source, statement, "max-elements") == "unbounded";
      node.ordered_by_user =
          StringChild(*source, statement, "ordered-by") == "user";
      node.type = types.Find(*source, statement.id);
      AppendMustConstraints(source, statement, node);
      AppendWhenConstraints(source, statement, node,
                            (*kind == SchemaNodeKind::kChoice ||
                             *kind == SchemaNodeKind::kCase)
                                ? actual_parent : std::nullopt);
      if (*kind == SchemaNodeKind::kRpc || *kind == SchemaNodeKind::kAction ||
          *kind == SchemaNodeKind::kInput || *kind == SchemaNodeKind::kOutput ||
          *kind == SchemaNodeKind::kNotification) node.declared_config = false;
      const auto id = add_node(std::move(node), statement.range);
      if (!id) continue;
      if (created && actual_parent == parent_node) created->push_back(*id);
      expand_children(owner, namespace_owner, source, statement, *id, origin,
                      instantiation, scopes, nullptr);
    }
    if (pushed) scopes.pop_back();
  };

  auto root_symbols = std::move(selected_symbols);
  std::vector<GroupingScope> scopes;
  GroupingScope top;
  for (const auto& [name, symbol] : root_symbols->symbols(SymbolKind::kGrouping)) top.emplace(name, symbol);
  scopes.push_back(std::move(top));
  const auto expand_unit = [&](const std::shared_ptr<const ResolvedModule>& unit) {
    if (!unit->syntax || unit->syntax->roots().empty()) return;
    expand_children(root_symbols, root_symbols, unit,
                    unit->syntax->Get(unit->syntax->roots().front()), std::nullopt,
                    SchemaNodeOrigin::kDeclared, kInvalidStatementId, scopes,
                    nullptr);
  };
  expand_unit(root_symbols->module());
  for (const auto& included : root_symbols->module()->includes) expand_unit(included);

  const auto apply_augments = [&](const std::shared_ptr<const ResolvedModule>& unit) {
    if (!unit->syntax || unit->syntax->roots().empty()) return;
    const Statement& syntax_root = unit->syntax->Get(unit->syntax->roots().front());
    SchemaPathParser path_parser(diagnostics_);
    SchemaPathResolver path_resolver(*unit, result, diagnostics_);
    for (const StatementId child_id : syntax_root.children) {
      const Statement& augment = unit->syntax->Get(child_id);
      if (augment.keyword != "augment" || !augment.argument) continue;
      if (!StatementEnabled(*unit, augment, features, diagnostics_, valid)) {
        continue;
      }
      const auto path = path_parser.Parse(*augment.argument, SchemaPathKind::kAbsolute,
                                          augment.range);
      if (!path) { valid = false; continue; }
      const SchemaPathSegment& first = path->segments.front();
      std::string target_module = unit->belongs_to.value_or(unit->name);
      if (first.prefix && *first.prefix != unit->prefix) {
        const auto imported = unit->imports.find(*first.prefix);
        if (imported != unit->imports.end()) target_module = imported->second->name;
      }
      if (target_module != root_symbols->module()->name) continue;
      const auto target = path_resolver.ResolveAbsolute(*path, augment.range);
      if (!target) { valid = false; continue; }
      if (!Augmentable(result.Get(*target).kind)) {
        diagnostics_.Report({DiagnosticCode::kInvalidAugmentTarget, DiagnosticSeverity::kError,
                             fmt::format("augment target '{}' has an invalid node kind", *augment.argument),
                             augment.range});
        valid = false;
        continue;
      }
      std::vector<SchemaNodeId> augmented_nodes;
      expand_children(root_symbols, root_symbols, unit, augment, *target,
                      SchemaNodeOrigin::kAugment, augment.id, scopes,
                      &augmented_nodes);
      if (FindChild(*unit, augment, "when")) {
        for (const SchemaNodeId node_id : augmented_nodes) {
          AppendWhenConstraints(unit, augment, result.Get(node_id), *target);
        }
      }
    }
  };
  apply_augments(root_symbols->module());
  for (const auto& included : root_symbols->module()->includes) apply_augments(included);

  std::function<void(SchemaNodeId, bool)> inherit_config;
  inherit_config = [&](SchemaNodeId id, bool parent_config) {
    SchemaNode& node = result.Get(id);
    node.effective_config = parent_config && node.declared_config.value_or(true);
    for (const SchemaNodeId child : node.children) inherit_config(child, node.effective_config);
  };
  for (const SchemaNodeId root_id : result.roots()) inherit_config(root_id, true);

  for (std::size_t index = 0; index < result.size(); ++index) {
    SchemaNode& node = result.Get(static_cast<SchemaNodeId>(index));
    if (node.kind != SchemaNodeKind::kList || !node.source_module ||
        node.declaration == kInvalidStatementId) continue;
    const Statement& declaration = node.source_module->syntax->Get(node.declaration);
    SchemaPathParser path_parser(diagnostics_);
    SchemaPathResolver path_resolver(*node.source_module, result, diagnostics_,
                                     node.name.module);
    if (const Statement* key_statement = FindChild(*node.source_module, declaration, "key");
        key_statement && key_statement->argument) {
      std::istringstream input(*key_statement->argument);
      std::string token;
      std::unordered_set<SchemaNodeId> seen;
      bool found_key = false;
      while (input >> token) {
        found_key = true;
        const auto path = path_parser.Parse(token, SchemaPathKind::kDescendant,
                                            key_statement->range);
        const auto target = path ? path_resolver.ResolveDescendant(node.id, *path,
                                                                    key_statement->range)
                                 : std::nullopt;
        if (!target || result.Get(*target).kind != SchemaNodeKind::kLeaf ||
            path->segments.size() != 1 || !seen.insert(*target).second) {
          diagnostics_.Report({DiagnosticCode::kInvalidKey, DiagnosticSeverity::kError,
                               fmt::format("'{}' is not a unique direct leaf key", token),
                               key_statement->range});
          valid = false;
        } else {
          node.key.push_back(*target);
        }
      }
      if (!found_key) {
        diagnostics_.Report({DiagnosticCode::kInvalidKey, DiagnosticSeverity::kError,
                             "key must name at least one leaf", key_statement->range});
        valid = false;
      }
    } else if (node.effective_config) {
      diagnostics_.Report({DiagnosticCode::kInvalidKey, DiagnosticSeverity::kError,
                           fmt::format("configuration list '{}' requires a key", node.name.local_name),
                           declaration.range});
      valid = false;
    }
    for (const StatementId child_id : declaration.children) {
      const Statement& unique_statement = node.source_module->syntax->Get(child_id);
      if (unique_statement.keyword != "unique" || !unique_statement.argument) continue;
      std::istringstream input(*unique_statement.argument);
      std::string token;
      std::vector<SchemaNodeId> targets;
      std::unordered_set<SchemaNodeId> seen;
      while (input >> token) {
        const auto path = path_parser.Parse(token, SchemaPathKind::kDescendant,
                                            unique_statement.range);
        const auto target = path ? path_resolver.ResolveDescendant(node.id, *path,
                                                                   unique_statement.range)
                                 : std::nullopt;
        if (!target || result.Get(*target).kind != SchemaNodeKind::kLeaf ||
            !seen.insert(*target).second) {
          diagnostics_.Report({DiagnosticCode::kInvalidUnique, DiagnosticSeverity::kError,
                               fmt::format("'{}' is not a unique leaf descendant", token),
                               unique_statement.range});
          valid = false;
        } else {
          targets.push_back(*target);
        }
      }
      if (targets.empty()) {
        diagnostics_.Report({DiagnosticCode::kInvalidUnique, DiagnosticSeverity::kError,
                             "unique must name at least one leaf descendant",
                             unique_statement.range});
        valid = false;
      } else {
        node.unique.push_back(std::move(targets));
      }
    }
  }
  for (std::size_t index = 0; index < result.size(); ++index) {
    const SchemaNode& node = result.Get(static_cast<SchemaNodeId>(index));
    if (node.min_elements && node.max_elements &&
        *node.min_elements > *node.max_elements) {
      diagnostics_.Report({DiagnosticCode::kInvalidElementBounds,
                           DiagnosticSeverity::kError,
                           fmt::format("node '{}:{}' has min-elements greater than max-elements",
                                       node.name.module, node.name.local_name),
                           {}});
      valid = false;
    }
    if (node.kind == SchemaNodeKind::kChoice && node.default_value) {
      std::string_view default_case = *node.default_value;
      if (const std::size_t colon = default_case.find(':');
          colon != std::string_view::npos) {
        default_case.remove_prefix(colon + 1);
      }
      const bool found = std::ranges::any_of(node.children, [&](SchemaNodeId child) {
        return result.Get(child).kind == SchemaNodeKind::kCase &&
               result.Get(child).name.local_name == default_case;
      });
      if (!found || node.mandatory) {
        diagnostics_.Report({DiagnosticCode::kInvalidChoiceDefault,
                             DiagnosticSeverity::kError,
                             found ? "a mandatory choice cannot have a default case"
                                   : fmt::format("choice default case '{}' was not found",
                                                 *node.default_value),
                             {}});
        valid = false;
      }
    }
    if (node.kind == SchemaNodeKind::kAction ||
        node.kind == SchemaNodeKind::kNotification) {
      std::optional<SchemaNodeId> ancestor = node.parent;
      while (ancestor) {
        const SchemaNodeKind kind = result.Get(*ancestor).kind;
        if (kind == SchemaNodeKind::kRpc || kind == SchemaNodeKind::kAction ||
            kind == SchemaNodeKind::kNotification ||
            kind == SchemaNodeKind::kInput || kind == SchemaNodeKind::kOutput) {
          diagnostics_.Report({DiagnosticCode::kInvalidOperationPlacement,
                               DiagnosticSeverity::kError,
                               fmt::format("operation or notification '{}:{}' has an invalid ancestor",
                                           node.name.module, node.name.local_name),
                               {}});
          valid = false;
          break;
        }
        ancestor = result.Get(*ancestor).parent;
      }
    }
  }
  if (!valid) return std::nullopt;
  return result;
}

const SchemaTree* SchemaContext::Find(const ResolvedModule& module) const {
  const auto found = trees_.find(&module);
  return found == trees_.end() ? nullptr : &found->second;
}

SchemaTree* SchemaContext::Find(const ResolvedModule& module) {
  const auto found = trees_.find(&module);
  return found == trees_.end() ? nullptr : &found->second;
}

const SchemaTree& SchemaContext::root() const {
  return trees_.at(root_module_.get());
}

std::vector<const ResolvedModule*> SchemaContext::modules() const {
  std::vector<const ResolvedModule*> result;
  result.reserve(trees_.size());
  for (const auto& [module, tree] : trees_) { (void)tree; result.push_back(module); }
  return result;
}

std::optional<SchemaContext> SchemaContextBuilder::Build(
    const SemanticContext& semantics, const TypeContext& types,
    const FeatureSet* features) {
  if (!semantics.root()) return std::nullopt;
  SchemaContext context;
  context.root_module_ = semantics.root()->module();
  bool valid = true;
  std::vector<std::shared_ptr<const ModuleSymbols>> modules;
  std::unordered_set<const ResolvedModule*> visited;
  std::function<void(std::shared_ptr<const ModuleSymbols>)> collect;
  collect = [&](std::shared_ptr<const ModuleSymbols> symbols) {
    if (!symbols || !visited.insert(symbols->module().get()).second) return;
    modules.push_back(symbols);
    for (const auto& [prefix, imported] : symbols->module()->imports) {
      (void)prefix;
      collect(semantics.FindModule(*imported));
    }
    for (const auto& included : symbols->module()->includes) {
      for (const auto& [prefix, imported] : included->imports) {
        (void)prefix;
        collect(semantics.FindModule(*imported));
      }
    }
  };
  collect(semantics.root());
  SchemaBuilder canonical_builder(diagnostics_);
  for (const auto& symbols : modules) {
    auto tree = canonical_builder.BuildFor(semantics, types, symbols, features);
    if (!tree) return std::nullopt;
    context.trees_.emplace(symbols->module().get(), std::move(*tree));
  }

  const auto append_augment = [&](const std::shared_ptr<const ModuleSymbols>& source_owner,
                                  const std::shared_ptr<const ResolvedModule>& source,
                                  const Statement& augment, SchemaTree& target_tree,
                                  SchemaNodeId target) {
    std::vector<SchemaNodeId> added_roots;
    std::unordered_set<GroupingKey, GroupingKeyHash> grouping_stack;
    std::function<void(const std::shared_ptr<const ModuleSymbols>&,
                       const std::shared_ptr<const ModuleSymbols>&,
                       const std::shared_ptr<const ResolvedModule>&,
                       const Statement&, SchemaNodeId, StatementId,
                       SchemaNodeOrigin)> append_children;
    append_children = [&](const std::shared_ptr<const ModuleSymbols>& defining_owner,
                          const std::shared_ptr<const ModuleSymbols>& namespace_owner,
                          const std::shared_ptr<const ResolvedModule>& defining_source,
                          const Statement& parent_statement, SchemaNodeId parent,
                          StatementId instantiation, SchemaNodeOrigin expansion_origin) {
      for (const StatementId child_id : parent_statement.children) {
        const Statement& statement = defining_source->syntax->Get(child_id);
        if (!StatementEnabled(*defining_source, statement, features,
                              diagnostics_, valid)) {
          continue;
        }
        if (statement.keyword == "uses" && statement.argument) {
          std::string_view grouping_name = *statement.argument;
          std::shared_ptr<const ModuleSymbols> grouping_owner = defining_owner;
          const std::size_t colon = grouping_name.find(':');
          if (colon != std::string_view::npos) {
            const std::string_view prefix = grouping_name.substr(0, colon);
            grouping_name.remove_prefix(colon + 1);
            if (prefix != defining_source->prefix && prefix != defining_owner->module()->prefix) {
              const auto imported = defining_source->imports.find(std::string(prefix));
              if (imported != defining_source->imports.end()) {
                grouping_owner = semantics.FindModule(*imported->second);
              }
            }
          }
          const auto symbol = grouping_owner
              ? grouping_owner->Find(SymbolKind::kGrouping, grouping_name)
              : std::nullopt;
          if (!symbol) continue;
          const GroupingKey key{symbol->source_module.get(), symbol->statement};
          if (!grouping_stack.insert(key).second) {
            diagnostics_.Report({DiagnosticCode::kGroupingCycle, DiagnosticSeverity::kError,
                                 fmt::format("recursive grouping use '{}'", *statement.argument),
                                 statement.range});
            valid = false;
            continue;
          }
          const Statement& grouping = symbol->source_module->syntax->Get(symbol->statement);
          append_children(grouping_owner, namespace_owner,
                          symbol->source_module, grouping, parent, statement.id,
                          SchemaNodeOrigin::kUses);
          grouping_stack.erase(key);
          continue;
        }
        const auto kind = NodeKind(statement.keyword);
        if (!kind || (!statement.argument && statement.keyword != "input" &&
                      statement.keyword != "output")) continue;
        const std::string local_name = statement.argument.value_or(statement.keyword);
        SchemaNodeId actual_parent = parent;
        if (target_tree.Get(parent).kind == SchemaNodeKind::kChoice &&
            *kind != SchemaNodeKind::kCase) {
          SchemaNode implicit;
          implicit.parent = parent;
          implicit.kind = SchemaNodeKind::kCase;
          implicit.origin = SchemaNodeOrigin::kImplicitCase;
          implicit.name = {namespace_owner->module()->name, local_name};
          implicit.source_module = defining_source;
          implicit.declaration = statement.id;
          implicit.instantiation = instantiation;
          if (target_tree.FindChild(parent, implicit.name)) {
            diagnostics_.Report({DiagnosticCode::kDuplicateSchemaNode, DiagnosticSeverity::kError,
                                 fmt::format("duplicate augmented case '{}'", local_name), statement.range});
            valid = false;
            continue;
          }
          actual_parent = target_tree.Add(std::move(implicit));
          if (parent == target) added_roots.push_back(actual_parent);
        }
        SchemaNode node;
        node.parent = actual_parent;
        node.kind = *kind;
        node.origin = expansion_origin;
        node.name = {namespace_owner->module()->name, local_name};
        node.source_module = defining_source;
        node.declaration = statement.id;
        node.instantiation = instantiation;
        node.declared_config = BooleanChild(*defining_source, statement, "config");
        node.declared_mandatory = BooleanChild(*defining_source, statement,
                                               "mandatory");
        node.mandatory = node.declared_mandatory.value_or(false);
        node.presence_container =
            StringChild(*defining_source, statement, "presence").has_value();
        node.default_value = StringChild(*defining_source, statement, "default");
        node.default_values =
            StringChildren(*defining_source, statement, "default");
        node.units = StringChild(*defining_source, statement, "units");
        node.min_elements = UnsignedChild(*defining_source, statement,
                                          "min-elements");
        node.max_elements = UnsignedChild(*defining_source, statement,
                                          "max-elements");
        node.max_elements_unbounded =
            StringChild(*defining_source, statement, "max-elements") ==
            "unbounded";
        node.ordered_by_user =
            StringChild(*defining_source, statement, "ordered-by") == "user";
        node.type = types.Find(*defining_source, statement.id);
        AppendMustConstraints(defining_source, statement, node);
        AppendWhenConstraints(defining_source, statement, node,
                              (*kind == SchemaNodeKind::kChoice ||
                               *kind == SchemaNodeKind::kCase)
                                  ? std::optional<SchemaNodeId>(actual_parent)
                                  : std::nullopt);
        if (*kind == SchemaNodeKind::kRpc || *kind == SchemaNodeKind::kAction ||
            *kind == SchemaNodeKind::kInput || *kind == SchemaNodeKind::kOutput ||
            *kind == SchemaNodeKind::kNotification) node.declared_config = false;
        if (target_tree.FindChild(actual_parent, node.name)) {
          diagnostics_.Report({DiagnosticCode::kDuplicateSchemaNode, DiagnosticSeverity::kError,
                               fmt::format("duplicate augmented node '{}:{}'", node.name.module,
                                           node.name.local_name), statement.range});
          valid = false;
          continue;
        }
        const SchemaNodeId id = target_tree.Add(std::move(node));
        if (actual_parent == parent && parent == target) {
          added_roots.push_back(id);
        }
        append_children(defining_owner, namespace_owner, defining_source,
                        statement, id, instantiation, expansion_origin);
      }
    };
    append_children(source_owner, source_owner, source, augment, target,
                    augment.id,
                    SchemaNodeOrigin::kAugment);
    if (FindChild(*source, augment, "when")) {
      std::function<void(SchemaNodeId)> propagate_when;
      propagate_when = [&](SchemaNodeId node_id) {
        AppendWhenConstraints(source, augment, target_tree.Get(node_id), target);
        for (const SchemaNodeId child : target_tree.Get(node_id).children) {
          propagate_when(child);
        }
      };
      for (const SchemaNodeId id : added_roots) propagate_when(id);
    }
    const bool parent_config = target_tree.Get(target).effective_config;
    std::function<void(SchemaNodeId, bool)> inherit;
    inherit = [&](SchemaNodeId id, bool inherited) {
      SchemaNode& node = target_tree.Get(id);
      node.effective_config = inherited && node.declared_config.value_or(true);
      for (const SchemaNodeId child : node.children) inherit(child, node.effective_config);
    };
    for (const SchemaNodeId id : added_roots) inherit(id, parent_config);
    if (!FindChild(*source, augment, "when")) {
      std::function<bool(SchemaNodeId)> contains_mandatory;
      contains_mandatory = [&](SchemaNodeId id) {
        const SchemaNode& node = target_tree.Get(id);
        if (!node.effective_config) return false;
        if (node.mandatory || node.min_elements.value_or(0) > 0) return true;
        // A presence container and a list with no positive min-elements are
        // optional instances. Mandatory descendants constrain an instance only
        // after it exists, so they do not make the augment itself mandatory.
        if ((node.kind == SchemaNodeKind::kContainer &&
             node.presence_container) ||
            node.kind == SchemaNodeKind::kList) {
          return false;
        }
        return std::ranges::any_of(node.children, contains_mandatory);
      };
      if (std::ranges::any_of(added_roots, contains_mandatory)) {
        diagnostics_.Report({DiagnosticCode::kMandatoryAugmentRequiresWhen,
                             DiagnosticSeverity::kError,
                             "cross-module augment adding mandatory configuration requires 'when'",
                             augment.range});
        valid = false;
      }
    }
  };

  for (const auto& source_symbols : modules) {
    const auto process_unit = [&](const std::shared_ptr<const ResolvedModule>& source) {
      if (!source->syntax || source->syntax->roots().empty()) return;
      const Statement& root = source->syntax->Get(source->syntax->roots().front());
      for (const StatementId child_id : root.children) {
        const Statement& augment = source->syntax->Get(child_id);
        if (augment.keyword != "augment" || !augment.argument) continue;
        if (!StatementEnabled(*source, augment, features, diagnostics_, valid)) {
          continue;
        }
        SchemaPathParser parser(diagnostics_);
        const auto path = parser.Parse(*augment.argument, SchemaPathKind::kAbsolute,
                                       augment.range);
        if (!path) { valid = false; continue; }
        const SchemaPathSegment& first = path->segments.front();
        std::shared_ptr<const ResolvedModule> target_module = source_symbols->module();
        if (first.prefix && *first.prefix != source->prefix) {
          const auto imported = source->imports.find(*first.prefix);
          if (imported == source->imports.end()) continue;
          target_module = imported->second;
        }
        if (target_module.get() == source_symbols->module().get()) continue;
        SchemaTree* target_tree = context.Find(*target_module);
        if (!target_tree) continue;
        SchemaPathResolver resolver(*source, *target_tree, diagnostics_);
        const auto target = resolver.ResolveAbsolute(*path, augment.range);
        if (!target) { valid = false; continue; }
        if (!Augmentable(target_tree->Get(*target).kind)) {
          diagnostics_.Report({DiagnosticCode::kInvalidAugmentTarget, DiagnosticSeverity::kError,
                               fmt::format("augment target '{}' has an invalid node kind", *augment.argument),
                               augment.range});
          valid = false;
          continue;
        }
        append_augment(source_symbols, source, augment, *target_tree, *target);
      }
    };
    process_unit(source_symbols->module());
    for (const auto& included : source_symbols->module()->includes) process_unit(included);
  }
  if (!valid) return std::nullopt;
  return context;
}

}  // namespace yang::semantic
