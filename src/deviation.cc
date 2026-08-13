// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/deviation.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include <fmt/format.h>

#include "yang/schema_path.h"

namespace yang::semantic {
namespace {

std::optional<bool> BooleanValue(const Statement& statement) {
  if (!statement.argument) return std::nullopt;
  if (*statement.argument == "true") return true;
  if (*statement.argument == "false") return false;
  return std::nullopt;
}

std::optional<std::uint64_t> UnsignedValue(const Statement& statement) {
  if (!statement.argument || *statement.argument == "unbounded") {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  const auto [end, error] = std::from_chars(
      statement.argument->data(),
      statement.argument->data() + statement.argument->size(), value);
  return error == std::errc{} &&
                 end == statement.argument->data() + statement.argument->size()
             ? std::optional<std::uint64_t>(value)
             : std::nullopt;
}

bool PropertyAllowed(SchemaNodeKind kind, std::string_view property) {
  const bool scalar = kind == SchemaNodeKind::kLeaf ||
                      kind == SchemaNodeKind::kLeafList;
  if (property == "type" || property == "units") return scalar;
  if (property == "min-elements" || property == "max-elements") {
    return kind == SchemaNodeKind::kList || kind == SchemaNodeKind::kLeafList;
  }
  if (property == "unique") return kind == SchemaNodeKind::kList;
  if (property == "default") {
    return scalar || kind == SchemaNodeKind::kChoice;
  }
  if (property == "mandatory") {
    return kind == SchemaNodeKind::kLeaf || kind == SchemaNodeKind::kChoice ||
           kind == SchemaNodeKind::kAnydata || kind == SchemaNodeKind::kAnyxml;
  }
  if (property == "config") {
    return kind != SchemaNodeKind::kRpc && kind != SchemaNodeKind::kAction &&
           kind != SchemaNodeKind::kInput && kind != SchemaNodeKind::kOutput &&
           kind != SchemaNodeKind::kNotification;
  }
  return property == "must";
}

}  // namespace

bool DeviationApplier::Apply(SchemaContext& schemas, const TypeContext& types) {
  bool valid = true;
  auto report = [&](const Statement& statement, std::string message) {
    diagnostics_.Report({DiagnosticCode::kInvalidDeviate,
                         DiagnosticSeverity::kError, std::move(message),
                         statement.range});
    valid = false;
  };

  std::vector<const ResolvedModule*> modules = schemas.modules();
  std::sort(modules.begin(), modules.end(), [](const auto* left,
                                                const auto* right) {
    return left->name < right->name;
  });
  for (const ResolvedModule* module : modules) {
    const auto process_unit = [&](const ResolvedModule& source) {
      if (!source.syntax || source.syntax->roots().empty()) return;
      const Statement& root = source.syntax->Get(source.syntax->roots().front());
      for (const StatementId statement_id : root.children) {
        const Statement& deviation = source.syntax->Get(statement_id);
        if (deviation.keyword != "deviation" || !deviation.argument) continue;
        SchemaPathParser parser(diagnostics_);
        const auto path = parser.Parse(*deviation.argument,
                                       SchemaPathKind::kAbsolute,
                                       deviation.range);
        if (!path) {
          valid = false;
          continue;
        }
        const ResolvedModule* target_module = nullptr;
        const SchemaPathSegment& first = path->segments.front();
        if (!first.prefix || *first.prefix == source.prefix) {
          for (const ResolvedModule* candidate : modules) {
            if (candidate->name == source.belongs_to.value_or(source.name)) {
              target_module = candidate;
              break;
            }
          }
        } else {
          const auto imported = source.imports.find(*first.prefix);
          if (imported != source.imports.end()) target_module = imported->second.get();
        }
        SchemaTree* tree = target_module ? schemas.Find(*target_module) : nullptr;
        if (!tree) {
          diagnostics_.Report({DiagnosticCode::kInvalidDeviationTarget,
                               DiagnosticSeverity::kError,
                               fmt::format("deviation target module for '{}' was not found",
                                           *deviation.argument),
                               deviation.range});
          valid = false;
          continue;
        }
        SchemaPathResolver resolver(source, *tree, diagnostics_);
        const auto target_id = resolver.ResolveAbsolute(*path, deviation.range);
        if (!target_id) {
          valid = false;
          continue;
        }
        SchemaNode& target = tree->Get(*target_id);
        std::size_t deviate_count = 0;
        bool has_not_supported = false;
        for (const StatementId child_id : deviation.children) {
          const Statement& child = source.syntax->Get(child_id);
          if (child.keyword != "deviate") continue;
          ++deviate_count;
          has_not_supported |= child.argument == "not-supported";
        }
        if (has_not_supported && deviate_count != 1) {
          report(deviation,
                 "deviate not-supported must be the only deviate statement");
          continue;
        }
        bool removed = false;
        for (const StatementId deviate_id : deviation.children) {
          const Statement& deviate = source.syntax->Get(deviate_id);
          if (deviate.keyword != "deviate" || !deviate.argument) continue;
          const std::string_view operation = *deviate.argument;
          if (operation == "not-supported") {
            if (removed || !deviate.children.empty()) {
              report(deviate, "deviate not-supported cannot have properties");
              continue;
            }
            tree->DisableSubtree(*target_id);
            removed = true;
            continue;
          }
          if (removed || (operation != "add" && operation != "replace" &&
                          operation != "delete")) {
            report(deviate, fmt::format("invalid deviate operation '{}'", operation));
            continue;
          }
          const bool adding = operation == "add";
          const bool replacing = operation == "replace";
          for (const StatementId property_id : deviate.children) {
            const Statement& property = source.syntax->Get(property_id);
            if (!PropertyAllowed(target.kind, property.keyword)) {
              report(property,
                     fmt::format("'{}' cannot deviate this schema node kind",
                                 property.keyword));
              continue;
            }
            const auto require_presence = [&](bool present) {
              if ((adding && present) || ((replacing || operation == "delete") &&
                                           !present)) {
                report(property,
                       fmt::format("deviate {} cannot apply '{}' because it is {}",
                                   operation, property.keyword,
                                   present ? "already present" : "not present"));
                return false;
              }
              return true;
            };
            if (property.keyword == "config") {
              const auto value = BooleanValue(property);
              if (!value) {
                report(property, "config must be true or false");
                continue;
              }
              if (!require_presence(target.declared_config.has_value())) continue;
              if (operation == "delete") {
                if (target.declared_config != value) {
                  report(property, "deleted config value does not match");
                } else {
                  target.declared_config.reset();
                }
              } else {
                target.declared_config = value;
              }
            } else if (property.keyword == "mandatory") {
              const auto value = BooleanValue(property);
              if (!value) {
                report(property, "mandatory must be true or false");
                continue;
              }
              if (!require_presence(target.declared_mandatory.has_value())) continue;
              if (operation == "delete") {
                if (target.declared_mandatory != value) {
                  report(property, "deleted mandatory value does not match");
                } else {
                  target.declared_mandatory.reset();
                  target.mandatory = false;
                }
              } else {
                target.declared_mandatory = value;
                target.mandatory = *value;
              }
            } else if (property.keyword == "default") {
              if (!property.argument ||
                  !require_presence(target.default_value.has_value())) continue;
              if (operation == "delete") {
                if (target.default_value != property.argument) {
                  report(property, "deleted default value does not match");
                } else {
                  target.default_value.reset();
                }
              } else {
                target.default_value = property.argument;
              }
            } else if (property.keyword == "units") {
              if (!property.argument || !require_presence(target.units.has_value())) continue;
              if (operation == "delete") {
                if (target.units != property.argument) {
                  report(property, "deleted units value does not match");
                } else {
                  target.units.reset();
                }
              } else {
                target.units = property.argument;
              }
            } else if (property.keyword == "min-elements") {
              auto& slot = target.min_elements;
              const auto value = UnsignedValue(property);
              if (!value) {
                report(property, "min-elements must be an unsigned integer");
                continue;
              }
              if (!require_presence(slot.has_value())) continue;
              if (operation == "delete") {
                if (slot != value) {
                  report(property, "deleted element bound does not match");
                } else {
                  slot.reset();
                }
              } else {
                slot = value;
              }
            } else if (property.keyword == "max-elements") {
              const bool unbounded = property.argument == "unbounded";
              const auto value = UnsignedValue(property);
              if (!unbounded && !value) {
                report(property,
                       "max-elements must be an unsigned integer or unbounded");
                continue;
              }
              const bool present = target.max_elements.has_value() ||
                                   target.max_elements_unbounded;
              if (!require_presence(present)) continue;
              if (operation == "delete") {
                const bool matches = unbounded
                    ? target.max_elements_unbounded
                    : target.max_elements == value;
                if (!matches) {
                  report(property, "deleted max-elements value does not match");
                } else {
                  target.max_elements.reset();
                  target.max_elements_unbounded = false;
                }
              } else {
                target.max_elements = value;
                target.max_elements_unbounded = unbounded;
              }
            } else if (property.keyword == "type" && replacing) {
              const auto replacement = types.Find(source, deviate.id);
              if (!replacement) {
                report(property, "replacement type could not be resolved");
              } else {
                target.type = replacement;
              }
            } else if (property.keyword == "must") {
              if (!property.argument || replacing) {
                report(property, "must can only be added or deleted by a deviation");
                continue;
              }
              if (adding) {
                target.must_constraints.push_back(
                    {.source_module = &source, .statement = property.id});
                continue;
              }
              const auto found = std::find_if(
                  target.must_constraints.begin(), target.must_constraints.end(),
                  [&](const SchemaConstraint& constraint) {
                    const Statement& existing =
                        constraint.source_module->syntax->Get(constraint.statement);
                    return existing.argument == property.argument;
                  });
              if (found == target.must_constraints.end()) {
                report(property, "deleted must expression was not present");
              } else {
                target.must_constraints.erase(found);
              }
            } else if (property.keyword == "unique") {
              if (!property.argument || replacing ||
                  target.kind != SchemaNodeKind::kList) {
                report(property,
                       "unique can only be added to or deleted from a list");
                continue;
              }
              std::istringstream input(*property.argument);
              std::string token;
              std::vector<SchemaNodeId> resolved_unique;
              std::unordered_set<SchemaNodeId> seen;
              SchemaPathParser path_parser(diagnostics_);
              SchemaPathResolver path_resolver(source, *tree, diagnostics_);
              bool paths_valid = true;
              while (input >> token) {
                const auto unique_path = path_parser.Parse(
                    token, SchemaPathKind::kDescendant, property.range);
                const auto unique_target = unique_path
                    ? path_resolver.ResolveDescendant(*target_id, *unique_path,
                                                      property.range)
                    : std::nullopt;
                if (!unique_target ||
                    (tree->Get(*unique_target).kind != SchemaNodeKind::kLeaf &&
                     tree->Get(*unique_target).kind !=
                         SchemaNodeKind::kLeafList) ||
                    !seen.insert(*unique_target).second) {
                  paths_valid = false;
                  break;
                }
                resolved_unique.push_back(*unique_target);
              }
              if (!paths_valid || resolved_unique.empty()) {
                report(property, "deviated unique path is invalid");
                continue;
              }
              if (adding) {
                if (std::find(target.unique.begin(), target.unique.end(),
                              resolved_unique) != target.unique.end()) {
                  report(property, "added unique constraint already exists");
                } else {
                  target.unique.push_back(std::move(resolved_unique));
                }
                continue;
              }
              const auto found = std::find(target.unique.begin(),
                                           target.unique.end(), resolved_unique);
              if (found == target.unique.end()) {
                report(property, "deleted unique constraint was not present");
              } else {
                target.unique.erase(found);
              }
            } else {
              report(property, fmt::format("'{}' is not valid for deviate {}",
                                           property.keyword, operation));
            }
          }
        }
      }
    };
    process_unit(*module);
    for (const auto& included : module->includes) process_unit(*included);
  }

  for (auto& [module, tree] : schemas.trees_) {
    (void)module;
    std::function<void(SchemaNodeId, bool)> inherit;
    inherit = [&](SchemaNodeId id, bool parent_config) {
      SchemaNode& node = tree.Get(id);
      node.effective_config = parent_config && node.declared_config.value_or(true);
      if (node.default_value && node.mandatory) {
        diagnostics_.Report({DiagnosticCode::kInvalidDeviate,
                             DiagnosticSeverity::kError,
                             fmt::format("deviated node '{}:{}' is mandatory and has a default",
                                         node.name.module, node.name.local_name),
                             {}});
        valid = false;
      }
      if (node.min_elements && node.max_elements &&
          *node.min_elements > *node.max_elements) {
        diagnostics_.Report({DiagnosticCode::kInvalidDeviate,
                             DiagnosticSeverity::kError,
                             "deviation makes min-elements greater than max-elements", {}});
        valid = false;
      }
      for (const SchemaNodeId child : node.children) inherit(child, node.effective_config);
    };
    for (const SchemaNodeId root : tree.roots()) inherit(root, true);
    for (std::size_t index = 0; index < tree.size(); ++index) {
      const SchemaNode& node = tree.Get(static_cast<SchemaNodeId>(index));
      if (!node.supported || node.kind != SchemaNodeKind::kList) continue;
      for (const SchemaNodeId key : node.key) {
        if (!tree.Get(key).supported) {
          diagnostics_.Report({DiagnosticCode::kInvalidDeviate,
                               DiagnosticSeverity::kError,
                               "deviation removes a list key leaf", {}});
          valid = false;
        }
      }
    }
  }
  return valid;
}

}  // namespace yang::semantic
