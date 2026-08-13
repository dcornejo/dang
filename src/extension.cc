// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/extension.h"

#include <functional>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

namespace yang::semantic {
namespace {

const Statement* FindChild(const ResolvedModule& source, const Statement& parent,
                           std::string_view keyword) {
  for (const StatementId id : parent.children) {
    const Statement& child = source.syntax->Get(id);
    if (child.keyword == keyword) return &child;
  }
  return nullptr;
}

std::string Key(std::string_view module, std::string_view name) {
  return std::string(module) + ':' + std::string(name);
}

}  // namespace

std::optional<ExtensionContext> ExtensionResolver::Resolve(
    const SemanticContext& semantics,
    const std::vector<ExtensionHandler>& handlers) {
  if (!semantics.root()) return std::nullopt;
  ExtensionContext result;
  bool valid = true;
  std::vector<std::shared_ptr<const ModuleSymbols>> modules;
  std::unordered_set<const ResolvedModule*> visited;
  std::function<void(std::shared_ptr<const ModuleSymbols>)> collect;
  collect = [&](std::shared_ptr<const ModuleSymbols> module) {
    if (!module || !visited.insert(module->module().get()).second) return;
    modules.push_back(module);
    for (const auto& [prefix, imported] : module->module()->imports) {
      (void)prefix;
      collect(semantics.FindModule(*imported));
    }
    for (const auto& included : module->module()->includes) {
      for (const auto& [prefix, imported] : included->imports) {
        (void)prefix;
        collect(semantics.FindModule(*imported));
      }
    }
  };
  collect(semantics.root());

  std::unordered_map<std::string, std::size_t> definitions;
  for (const auto& owner : modules) {
    for (const auto& [name, symbol] : owner->symbols(SymbolKind::kExtension)) {
      const Statement& declaration =
          symbol.source_module->syntax->Get(symbol.statement);
      ExtensionDefinition definition{
          .name = {owner->module()->name, name},
          .source_module = symbol.source_module,
          .statement = symbol.statement};
      if (const Statement* argument =
              FindChild(*symbol.source_module, declaration, "argument")) {
        definition.argument_name = argument->argument;
        if (const Statement* yin_element =
                FindChild(*symbol.source_module, *argument, "yin-element")) {
          definition.yin_element = yin_element->argument == "true";
        }
      }
      definitions.emplace(Key(definition.name.module, definition.name.name),
                          result.definitions_.size());
      result.definitions_.push_back(std::move(definition));
    }
  }

  for (const auto& owner : modules) {
    const auto process_unit = [&](const std::shared_ptr<const ResolvedModule>& source) {
      if (!source->syntax || source->syntax->roots().empty()) return;
      std::function<void(const Statement&)> visit;
      visit = [&](const Statement& statement) {
        const std::size_t colon = statement.keyword.find(':');
        if (colon != std::string::npos) {
          const std::string prefix = statement.keyword.substr(0, colon);
          const std::string local = statement.keyword.substr(colon + 1);
          std::string module_name;
          if (prefix == source->prefix || prefix == owner->module()->prefix) {
            module_name = owner->module()->name;
          } else if (const auto imported = source->imports.find(prefix);
                     imported != source->imports.end()) {
            module_name = imported->second->name;
          }
          const auto definition = definitions.find(Key(module_name, local));
          if (module_name.empty() || definition == definitions.end()) {
            diagnostics_.Report({DiagnosticCode::kInvalidExtensionInstance,
                                 DiagnosticSeverity::kError,
                                 fmt::format("extension instance '{}' cannot be resolved",
                                             statement.keyword),
                                 statement.range});
            valid = false;
          } else {
            const ExtensionDefinition& metadata =
                result.definitions_[definition->second];
            if (metadata.argument_name.has_value() !=
                statement.argument.has_value()) {
              diagnostics_.Report({
                  DiagnosticCode::kInvalidExtensionInstance,
                  DiagnosticSeverity::kError,
                  fmt::format("extension '{}' {} an argument",
                              statement.keyword,
                              metadata.argument_name ? "requires" : "does not take"),
                  statement.range});
              valid = false;
            } else {
              ExtensionInstance instance{metadata, source, statement.id,
                                         statement.argument};
              for (const ExtensionHandler& handler : handlers) {
                valid = handler(instance, diagnostics_) && valid;
              }
              result.instances_.push_back(std::move(instance));
            }
          }
        }
        for (const StatementId child_id : statement.children) {
          visit(source->syntax->Get(child_id));
        }
      };
      visit(source->syntax->Get(source->syntax->roots().front()));
    };
    process_unit(owner->module());
    for (const auto& included : owner->module()->includes) {
      process_unit(included);
    }
  }
  if (!valid) return std::nullopt;
  return result;
}

}  // namespace yang::semantic
