// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/schema_value.h"

#include <string_view>

#include <fmt/format.h>

namespace yang::semantic {
namespace {

std::optional<QualifiedSymbolName> ResolveIdentityName(
    const ResolvedModule& source, std::string_view text) {
  std::string module = source.belongs_to.value_or(source.name);
  const std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    if (text.empty()) return std::nullopt;
    return QualifiedSymbolName{std::move(module), std::string(text)};
  }
  if (colon == 0 || colon + 1 == text.size() ||
      text.find(':', colon + 1) != std::string_view::npos) {
    return std::nullopt;
  }
  const std::string prefix(text.substr(0, colon));
  if (prefix != source.prefix) {
    const auto imported = source.imports.find(prefix);
    if (imported == source.imports.end()) return std::nullopt;
    module = imported->second->name;
  }
  return QualifiedSymbolName{std::move(module),
                             std::string(text.substr(colon + 1))};
}

}  // namespace

bool SchemaValueValidator::Validate(const SchemaContext& schemas,
                                    const IdentityGraph& identities,
                                    const LeafrefContext& leafrefs) {
  bool valid = true;
  for (const ResolvedModule* tree_module : schemas.modules()) {
    const SchemaTree* tree = schemas.Find(*tree_module);
    if (!tree) continue;
    for (std::size_t index = 0; index < tree->size(); ++index) {
      const SchemaNodeId id = static_cast<SchemaNodeId>(index);
      const SchemaNode& node = tree->Get(id);
      if (!node.supported || !node.type || !node.source_module ||
          (node.default_values.empty() && !node.default_value)) {
        continue;
      }
      std::vector<std::string_view> defaults;
      if (!node.default_values.empty()) {
        for (const std::string& value : node.default_values) {
          defaults.push_back(value);
        }
      } else {
        defaults.push_back(*node.default_value);
      }
      for (const std::string_view default_value : defaults) {
        bool default_valid = true;
        if (node.type->builtin == BuiltinType::kIdentityRef) {
        const auto identity = ResolveIdentityName(*node.source_module,
                                                  default_value);
        default_valid = identity && identities.Contains(*identity);
        if (default_valid) {
          default_valid = false;
          for (const ResolvedIdentityName& resolved_base :
               node.type->resolved_identity_bases) {
            const QualifiedSymbolName base{resolved_base.module,
                                           resolved_base.name};
            if (identities.IsDerivedFrom(*identity, base)) {
              default_valid = true;
              break;
            }
          }
        }
        } else if (node.type->builtin == BuiltinType::kLeafRef) {
        const auto target_type = leafrefs.EffectiveType(
            schemas, {tree_module, id});
        default_valid = target_type &&
                        ValueMatchesType(*target_type, default_value);
        } else {
          continue;
        }
        if (!default_valid) {
        SourceRange range;
        if (node.declaration != kInvalidStatementId) {
          range = node.source_module->syntax->Get(node.declaration).range;
        }
        diagnostics_.Report({
            DiagnosticCode::kInvalidDefaultValue,
            DiagnosticSeverity::kError,
            fmt::format("'{}' is not a valid effective default for '{}:{}'",
                        default_value, node.name.module,
                        node.name.local_name),
            range});
          valid = false;
        }
      }
    }
  }
  return valid;
}

}  // namespace yang::semantic
