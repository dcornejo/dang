// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/leafref.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace yang::semantic {
namespace {

struct PathSegment {
  std::optional<std::string> prefix;
  std::string name;
  std::vector<std::string> predicates;
};

struct LeafrefPath {
  bool absolute = false;
  std::size_t parent_steps = 0;
  std::vector<PathSegment> segments;
};

std::string_view Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
  return value;
}

bool ValidIdentifier(std::string_view value) {
  if (value.empty()) return false;
  const auto start = [](unsigned char c) { return std::isalpha(c) != 0 || c == '_'; };
  if (!start(static_cast<unsigned char>(value.front()))) return false;
  for (const char character : value.substr(1)) {
    const unsigned char c = static_cast<unsigned char>(character);
    if (!start(c) && std::isdigit(c) == 0 && c != '-' && c != '.') return false;
  }
  return true;
}

std::optional<PathSegment> ParseSegment(std::string_view value) {
  PathSegment result;
  const std::size_t predicate = value.find('[');
  std::string_view name = Trim(value.substr(0, predicate));
  const std::size_t colon = name.find(':');
  if (colon != std::string_view::npos) {
    if (name.find(':', colon + 1) != std::string_view::npos) return std::nullopt;
    result.prefix = std::string(name.substr(0, colon)); name.remove_prefix(colon + 1);
  }
  if (!ValidIdentifier(name) || (result.prefix && !ValidIdentifier(*result.prefix))) return std::nullopt;
  result.name = name;
  std::size_t offset = predicate;
  while (offset != std::string_view::npos) {
    if (value[offset] != '[') return std::nullopt;
    const std::size_t close = value.find(']', offset + 1);
    if (close == std::string_view::npos) return std::nullopt;
    result.predicates.emplace_back(Trim(value.substr(offset + 1, close - offset - 1)));
    offset = close + 1;
    if (offset == value.size()) break;
    if (value[offset] != '[') return std::nullopt;
  }
  return result;
}

std::optional<LeafrefPath> ParsePath(std::string_view value) {
  LeafrefPath result;
  value = Trim(value);
  if (value.empty()) return std::nullopt;
  if (value.front() == '/') { result.absolute = true; value.remove_prefix(1); }
  while (!result.absolute && value.starts_with("../")) {
    ++result.parent_steps; value.remove_prefix(3);
  }
  if (!result.absolute && result.parent_steps == 0) return std::nullopt;
  while (!value.empty()) {
    std::size_t slash = std::string_view::npos;
    int bracket_depth = 0;
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (value[index] == '[') ++bracket_depth;
      else if (value[index] == ']') --bracket_depth;
      else if (value[index] == '/' && bracket_depth == 0) { slash = index; break; }
      if (bracket_depth < 0) return std::nullopt;
    }
    const std::string_view text = value.substr(0, slash);
    auto segment = ParseSegment(text);
    if (!segment) return std::nullopt;
    result.segments.push_back(std::move(*segment));
    if (slash == std::string_view::npos) break;
    value.remove_prefix(slash + 1);
    if (value.empty()) return std::nullopt;
  }
  return result.segments.empty() ? std::nullopt : std::optional<LeafrefPath>(std::move(result));
}

std::optional<std::string> ModuleFor(const ResolvedModule& source,
                                     const PathSegment& segment) {
  if (!segment.prefix || *segment.prefix == source.prefix) {
    return source.belongs_to.value_or(source.name);
  }
  const auto imported = source.imports.find(*segment.prefix);
  return imported == source.imports.end()
      ? std::nullopt : std::optional<std::string>(imported->second->name);
}

std::optional<SchemaNodeId> FindStep(const SchemaTree& tree,
                                     std::optional<SchemaNodeId> parent,
                                     const SchemaName& name) {
  if (const auto direct = tree.FindChild(parent, name)) {
    const SchemaNode& node = tree.Get(*direct);
    if (node.origin == SchemaNodeOrigin::kImplicitCase) {
      if (const auto nested = tree.FindChild(*direct, name)) return nested;
    }
    return direct;
  }
  const auto& candidates = parent ? tree.Get(*parent).children : tree.roots();
  for (const SchemaNodeId id : candidates) {
    if (tree.Get(id).kind == SchemaNodeKind::kCase) {
      if (const auto nested = tree.FindChild(id, name)) return nested;
    }
  }
  return std::nullopt;
}

const ResolvedModule* FindModuleByName(const SchemaContext& schemas,
                                       std::string_view name) {
  for (const ResolvedModule* module : schemas.modules()) if (module->name == name) return module;
  return nullptr;
}

}  // namespace

std::size_t SchemaNodeRefHash::operator()(const SchemaNodeRef& value) const noexcept {
  return std::hash<const void*>{}(value.tree_module) ^
         (std::hash<SchemaNodeId>{}(value.node) << 1U);
}

std::optional<SchemaNodeRef> LeafrefContext::Target(
    const SchemaNodeRef& leafref) const {
  const auto found = targets_.find(leafref);
  return found == targets_.end() ? std::nullopt
                                 : std::optional<SchemaNodeRef>(found->second);
}

std::shared_ptr<const ResolvedType> LeafrefContext::EffectiveType(
    const SchemaContext& schemas, const SchemaNodeRef& node) const {
  std::unordered_set<SchemaNodeRef, SchemaNodeRefHash> visited;
  SchemaNodeRef current = node;
  while (visited.insert(current).second) {
    const SchemaTree* tree = schemas.Find(*current.tree_module);
    if (!tree) return nullptr;
    const SchemaNode& schema_node = tree->Get(current.node);
    if (!schema_node.type || schema_node.type->builtin != BuiltinType::kLeafRef) return schema_node.type;
    const auto target = Target(current);
    if (!target) return nullptr;
    current = *target;
  }
  return nullptr;
}

const std::vector<LeafrefPredicate>& LeafrefContext::Predicates(
    const SchemaNodeRef& leafref) const {
  static const std::vector<LeafrefPredicate> empty;
  const auto found = predicates_.find(leafref);
  return found == predicates_.end() ? empty : found->second;
}

std::optional<LeafrefContext> LeafrefResolver::Resolve(
    const SchemaContext& schemas) {
  LeafrefContext result;
  bool valid = true;

  const auto resolve_relative_expression = [&](const ResolvedModule& source,
                                               const SchemaTree& tree,
                                               SchemaNodeId original,
                                               std::string_view expression)
      -> std::optional<SchemaNodeId> {
    expression = Trim(expression);
    if (!expression.starts_with("current()")) return std::nullopt;
    expression.remove_prefix(9);
    std::optional<SchemaNodeId> current = original;
    while (expression.starts_with("/..")) {
      if (!current || !tree.Get(*current).parent) return std::nullopt;
      current = tree.Get(*current).parent; expression.remove_prefix(3);
    }
    while (expression.starts_with('/')) {
      expression.remove_prefix(1);
      const std::size_t slash = expression.find('/');
      auto segment = ParseSegment(expression.substr(0, slash));
      if (!segment || !segment->predicates.empty()) return std::nullopt;
      const auto module = ModuleFor(source, *segment);
      if (!module) return std::nullopt;
      current = FindStep(tree, current, {*module, segment->name});
      if (!current) return std::nullopt;
      if (slash == std::string_view::npos) { expression = {}; break; }
      expression.remove_prefix(slash);
    }
    return expression.empty() ? current : std::nullopt;
  };

  for (const ResolvedModule* tree_module : schemas.modules()) {
    const SchemaTree* tree = schemas.Find(*tree_module);
    if (!tree) continue;
    for (std::size_t index = 0; index < tree->size(); ++index) {
      const SchemaNodeId id = static_cast<SchemaNodeId>(index);
      const SchemaNode& leaf = tree->Get(id);
      if (!leaf.supported || !leaf.type ||
          leaf.type->builtin != BuiltinType::kLeafRef ||
          !leaf.type->leafref_path || !leaf.source_module) continue;
      const auto parsed = ParsePath(*leaf.type->leafref_path);
      if (!parsed) {
        diagnostics_.Report({DiagnosticCode::kInvalidLeafrefPath, DiagnosticSeverity::kError,
                             fmt::format("invalid leafref path '{}'", *leaf.type->leafref_path), {}});
        valid = false; continue;
      }
      const ResolvedModule& source = *leaf.source_module;
      const SchemaTree* active_tree = tree;
      const ResolvedModule* active_module = tree_module;
      std::optional<SchemaNodeId> current;
      if (parsed->absolute) {
        const auto first_module = ModuleFor(source, parsed->segments.front());
        active_module = nullptr;
        if (parsed->segments.front().prefix &&
            *parsed->segments.front().prefix != source.prefix) {
          const auto imported = source.imports.find(*parsed->segments.front().prefix);
          if (imported != source.imports.end()) active_module = imported->second.get();
        } else if (first_module) {
          active_module = FindModuleByName(schemas, *first_module);
        }
        active_tree = active_module ? schemas.Find(*active_module) : nullptr;
        if (!active_tree) current = std::nullopt;
      } else {
        current = id;
        for (std::size_t step = 0; step < parsed->parent_steps && current; ++step) {
          current = active_tree->Get(*current).parent;
        }
      }
      bool path_valid = active_tree != nullptr && (parsed->absolute || current.has_value());
      std::vector<LeafrefPredicate> resolved_predicates;
      for (const auto& segment : parsed->segments) {
        if (!path_valid) break;
        const auto module = ModuleFor(source, segment);
        if (!module) { path_valid = false; break; }
        current = FindStep(*active_tree, current, {*module, segment.name});
        if (!current) { path_valid = false; break; }
        const SchemaNode& selected = active_tree->Get(*current);
        for (const auto& predicate_text : segment.predicates) {
          const std::size_t equals = predicate_text.find('=');
          if (equals == std::string::npos || predicate_text.find('=', equals + 1) != std::string::npos) {
            path_valid = false; break;
          }
          auto key_segment = ParseSegment(Trim(std::string_view(predicate_text).substr(0, equals)));
          const std::string_view rhs = Trim(std::string_view(predicate_text).substr(equals + 1));
          if (!key_segment || !key_segment->predicates.empty() ||
              selected.kind != SchemaNodeKind::kList) { path_valid = false; break; }
          const auto key_module = ModuleFor(source, *key_segment);
          const auto key = key_module
              ? FindStep(*active_tree, *current, {*key_module, key_segment->name})
              : std::nullopt;
          const auto rhs_node = resolve_relative_expression(source, *tree, id, rhs);
          if (!key || active_tree->Get(*key).kind != SchemaNodeKind::kLeaf ||
              std::ranges::find(selected.key, *key) == selected.key.end() || !rhs_node) {
            path_valid = false; break;
          }
          resolved_predicates.push_back({
              {active_module, *current}, {active_module, *key},
              {tree_module, *rhs_node}});
        }
      }
      if (!path_valid || !current) {
        diagnostics_.Report({DiagnosticCode::kInvalidLeafrefPath, DiagnosticSeverity::kError,
                             fmt::format("leafref path '{}' cannot be resolved", *leaf.type->leafref_path), {}});
        valid = false; continue;
      }
      const SchemaNode& target = active_tree->Get(*current);
      if (target.kind != SchemaNodeKind::kLeaf && target.kind != SchemaNodeKind::kLeafList) {
        diagnostics_.Report({DiagnosticCode::kInvalidLeafrefTarget, DiagnosticSeverity::kError,
                             fmt::format("leafref path '{}' does not target a leaf or leaf-list",
                                         *leaf.type->leafref_path), {}});
        valid = false; continue;
      }
      if (leaf.effective_config && !target.effective_config) {
        diagnostics_.Report({
            DiagnosticCode::kInvalidLeafrefTarget,
            DiagnosticSeverity::kError,
            "a configuration leafref cannot target state data", {}});
        valid = false;
        continue;
      }
      result.targets_[{tree_module, id}] = {active_module, *current};
      result.predicates_[{tree_module, id}] = std::move(resolved_predicates);
    }
  }

  enum class Mark { kVisiting, kDone };
  std::unordered_map<SchemaNodeRef, Mark, SchemaNodeRefHash> marks;
  std::function<void(const SchemaNodeRef&)> check_cycle;
  check_cycle = [&](const SchemaNodeRef& leafref) {
    const auto existing = marks.find(leafref);
    if (existing != marks.end() && existing->second == Mark::kDone) return;
    if (existing != marks.end() && existing->second == Mark::kVisiting) {
      diagnostics_.Report({DiagnosticCode::kLeafrefCycle, DiagnosticSeverity::kError,
                           "leafref target cycle detected", {}});
      valid = false; return;
    }
    marks[leafref] = Mark::kVisiting;
    const auto target = result.Target(leafref);
    if (target && result.targets_.contains(*target)) check_cycle(*target);
    marks[leafref] = Mark::kDone;
  };
  for (const auto& [leafref, target] : result.targets_) { (void)target; check_cycle(leafref); }
  if (!valid) return std::nullopt;
  return result;
}

}  // namespace yang::semantic
