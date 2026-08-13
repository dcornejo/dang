// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/semantic_model.h"

#include <cctype>
#include <functional>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "yang/schema_registry.h"

namespace yang::semantic {
namespace {

std::size_t Index(SymbolKind kind) { return static_cast<std::size_t>(kind); }

std::optional<SymbolKind> DeclarationKind(std::string_view keyword) {
  if (keyword == "typedef") return SymbolKind::kTypedef;
  if (keyword == "grouping") return SymbolKind::kGrouping;
  if (keyword == "feature") return SymbolKind::kFeature;
  if (keyword == "identity") return SymbolKind::kIdentity;
  if (keyword == "extension") return SymbolKind::kExtension;
  return std::nullopt;
}

bool IsBuiltinType(std::string_view name) {
  static const std::unordered_set<std::string_view> types{
      "binary", "bits", "boolean", "decimal64", "empty", "enumeration",
      "identityref", "instance-identifier", "int8", "int16", "int32", "int64",
      "leafref", "string", "uint8", "uint16", "uint32", "uint64", "union"};
  return types.contains(name);
}

struct QualifiedName {
  std::optional<std::string_view> prefix;
  std::string_view local;
};

std::optional<QualifiedName> SplitName(std::string_view value) {
  const std::size_t colon = value.find(':');
  if (colon == std::string_view::npos) return QualifiedName{std::nullopt, value};
  if (colon == 0 || colon + 1 == value.size() ||
      value.find(':', colon + 1) != std::string_view::npos) return std::nullopt;
  return QualifiedName{value.substr(0, colon), value.substr(colon + 1)};
}

std::vector<std::string_view> FeatureNames(std::string_view expression) {
  std::vector<std::string_view> names;
  std::size_t offset = 0;
  while (offset < expression.size()) {
    const unsigned char c = static_cast<unsigned char>(expression[offset]);
    if (std::isspace(c) || c == '(' || c == ')') { ++offset; continue; }
    const std::size_t begin = offset;
    while (offset < expression.size()) {
      const unsigned char current = static_cast<unsigned char>(expression[offset]);
      if (std::isspace(current) || current == '(' || current == ')') break;
      ++offset;
    }
    const std::string_view token = expression.substr(begin, offset - begin);
    if (token != "and" && token != "or" && token != "not") names.push_back(token);
  }
  return names;
}

}  // namespace

const std::unordered_map<std::string, Symbol>& ModuleSymbols::symbols(
    SymbolKind kind) const noexcept {
  return tables_[Index(kind)];
}

std::optional<Symbol> ModuleSymbols::Find(SymbolKind kind,
                                          std::string_view name) const {
  const auto& table = tables_[Index(kind)];
  const auto found = table.find(std::string(name));
  return found == table.end() ? std::nullopt : std::optional<Symbol>(found->second);
}

std::shared_ptr<const ModuleSymbols> SemanticContext::FindModule(
    std::string_view name) const {
  const auto found = modules_.find(std::string(name));
  return found == modules_.end() ? nullptr : found->second;
}

std::shared_ptr<const ModuleSymbols> SemanticContext::FindModule(
    const ResolvedModule& module) const {
  const auto found = units_.find(&module);
  return found == units_.end() ? nullptr : found->second;
}

std::optional<SemanticContext> SymbolResolver::Resolve(
    std::shared_ptr<const ResolvedModule> root) {
  if (!root || !root->syntax) return std::nullopt;
  SemanticContext context;
  bool valid = true;

  std::function<std::shared_ptr<ModuleSymbols>(std::shared_ptr<const ResolvedModule>)> build;
  build = [&](std::shared_ptr<const ResolvedModule> module) -> std::shared_ptr<ModuleSymbols> {
    if (const auto found = context.units_.find(module.get()); found != context.units_.end()) {
      return std::const_pointer_cast<ModuleSymbols>(found->second);
    }
    auto symbols = std::make_shared<ModuleSymbols>();
    symbols->module_ = module;
    context.modules_.try_emplace(module->name, symbols);
    context.units_.emplace(module.get(), symbols);

    const auto collect_root = [&](const std::shared_ptr<const ResolvedModule>& source) {
      if (!source->syntax || source->syntax->roots().empty()) return;
      const SyntaxTree& tree = *source->syntax;
      const Statement& source_root = tree.Get(tree.roots().front());
      for (const StatementId id : source_root.children) {
        const Statement& statement = tree.Get(id);
        const auto kind = DeclarationKind(statement.keyword);
        if (!kind || !statement.argument) continue;
        auto& table = symbols->tables_[Index(*kind)];
        Symbol declaration{*kind, *statement.argument, source, id};
        if (!table.emplace(declaration.name, declaration).second) {
          diagnostics_.Report({DiagnosticCode::kDuplicateSymbol, DiagnosticSeverity::kError,
              fmt::format("duplicate {} declaration '{}'", statement.keyword, *statement.argument),
              statement.range});
          valid = false;
        }
      }
    };
    collect_root(module);
    for (const auto& included : module->includes) collect_root(included);
    for (const auto& [prefix, imported] : module->imports) { (void)prefix; build(imported); }
    for (const auto& included : module->includes) {
      for (const auto& [prefix, imported] : included->imports) { (void)prefix; build(imported); }
    }
    return symbols;
  };

  context.root_ = build(root);

  using Scope = std::array<std::unordered_map<std::string, Symbol>, 2>;
  std::function<void(const std::shared_ptr<const ModuleSymbols>&,
                     const std::shared_ptr<const ResolvedModule>&)> validate_unit;
  validate_unit = [&](const std::shared_ptr<const ModuleSymbols>& owner,
                      const std::shared_ptr<const ResolvedModule>& source) {
    if (!source->syntax || source->syntax->roots().empty()) return;
    const SyntaxTree& tree = *source->syntax;
    std::vector<Scope> scopes;
    Scope module_scope;
    module_scope[0] = owner->tables_[Index(SymbolKind::kTypedef)];
    module_scope[1] = owner->tables_[Index(SymbolKind::kGrouping)];
    scopes.push_back(std::move(module_scope));

    const auto report_unknown = [&](SymbolKind kind, std::string_view name, SourceRange range) {
      static constexpr std::array labels{"typedef", "grouping", "feature", "identity", "extension"};
      diagnostics_.Report({DiagnosticCode::kUnknownSymbol, DiagnosticSeverity::kError,
          fmt::format("unknown {} '{}'", labels[Index(kind)], name), range});
      valid = false;
    };

    const auto resolve_reference = [&](SymbolKind kind, std::string_view name,
                                       SourceRange range) -> bool {
      const auto qualified = SplitName(name);
      if (!qualified || qualified->local.empty()) {
        diagnostics_.Report({DiagnosticCode::kInvalidQualifiedName, DiagnosticSeverity::kError,
                             fmt::format("'{}' is not a valid qualified identifier", name), range});
        valid = false;
        return false;
      }
      if (!qualified->prefix) {
        if (kind == SymbolKind::kTypedef || kind == SymbolKind::kGrouping) {
          const std::size_t scope_index = kind == SymbolKind::kTypedef ? 0 : 1;
          for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
            if ((*scope)[scope_index].contains(std::string(qualified->local))) return true;
          }
        } else if (owner->Find(kind, qualified->local)) {
          return true;
        }
        report_unknown(kind, name, range);
        return false;
      }
      std::shared_ptr<const ModuleSymbols> target;
      if (*qualified->prefix == source->prefix || *qualified->prefix == owner->module()->prefix) {
        target = owner;
      } else {
        const auto imported = source->imports.find(std::string(*qualified->prefix));
        if (imported != source->imports.end()) {
          const auto target_symbols = context.units_.find(imported->second.get());
          if (target_symbols != context.units_.end()) target = target_symbols->second;
        }
      }
      if (!target) {
        diagnostics_.Report({DiagnosticCode::kUnknownPrefix, DiagnosticSeverity::kError,
                             fmt::format("unknown prefix '{}'", *qualified->prefix), range});
        valid = false;
        return false;
      }
      if (!target->Find(kind, qualified->local)) {
        report_unknown(kind, name, range);
        return false;
      }
      return true;
    };

    std::function<void(const Statement&, bool)> visit;
    visit = [&](const Statement& statement, bool is_root) {
      if (statement.keyword == "type" && statement.argument &&
          (statement.argument->find(':') != std::string::npos || !IsBuiltinType(*statement.argument))) {
        resolve_reference(SymbolKind::kTypedef, *statement.argument, statement.range);
      } else if (statement.keyword == "uses" && statement.argument) {
        resolve_reference(SymbolKind::kGrouping, *statement.argument, statement.range);
      } else if (statement.keyword == "base" && statement.argument) {
        resolve_reference(SymbolKind::kIdentity, *statement.argument, statement.range);
      } else if (statement.keyword == "if-feature" && statement.argument) {
        for (const auto name : FeatureNames(*statement.argument)) {
          resolve_reference(SymbolKind::kFeature, name, statement.range);
        }
      }
      if (!SchemaRegistry::Find(statement.keyword) && statement.keyword.find(':') != std::string::npos) {
        resolve_reference(SymbolKind::kExtension, statement.keyword, statement.range);
      }

      Scope local;
      bool has_local = false;
      if (!is_root) {
        for (const StatementId child_id : statement.children) {
          const Statement& child = tree.Get(child_id);
          const auto kind = DeclarationKind(child.keyword);
          if (!kind || (*kind != SymbolKind::kTypedef && *kind != SymbolKind::kGrouping) ||
              !child.argument) continue;
          const std::size_t local_index = *kind == SymbolKind::kTypedef ? 0 : 1;
          Symbol symbol{*kind, *child.argument, source, child_id};
          if (!local[local_index].emplace(symbol.name, symbol).second) {
            diagnostics_.Report({DiagnosticCode::kDuplicateSymbol, DiagnosticSeverity::kError,
                fmt::format("duplicate {} declaration '{}' in the same scope", child.keyword,
                            *child.argument), child.range});
            valid = false;
          }
          has_local = true;
        }
      }
      if (has_local) scopes.push_back(std::move(local));
      for (const StatementId child_id : statement.children) visit(tree.Get(child_id), false);
      if (has_local) scopes.pop_back();
    };
    visit(tree.Get(tree.roots().front()), true);
  };

  std::unordered_set<const ResolvedModule*> validated;
  std::function<void(const std::shared_ptr<const ModuleSymbols>&)> validate_module;
  validate_module = [&](const std::shared_ptr<const ModuleSymbols>& symbols) {
    if (!validated.insert(symbols->module().get()).second) return;
    validate_unit(symbols, symbols->module());
    for (const auto& included : symbols->module()->includes) {
      validate_unit(symbols, included);
      for (const auto& [prefix, imported] : included->imports) {
        (void)prefix;
        validate_module(context.units_.at(imported.get()));
      }
    }
    for (const auto& [prefix, imported] : symbols->module()->imports) {
      (void)prefix;
      validate_module(context.units_.at(imported.get()));
    }
  };
  validate_module(context.root_);
  if (!valid) return std::nullopt;
  return context;
}

}  // namespace yang::semantic
