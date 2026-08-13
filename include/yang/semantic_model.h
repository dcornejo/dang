// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SEMANTIC_MODEL_H_
#define YANG_SEMANTIC_MODEL_H_

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "yang/module_resolver.h"

namespace yang::semantic {

enum class SymbolKind { kTypedef, kGrouping, kFeature, kIdentity, kExtension, kCount };

/** A declaration and the source unit containing it. */
struct Symbol {
  SymbolKind kind;
  std::string name;
  std::shared_ptr<const ResolvedModule> source_module;
  StatementId statement = kInvalidStatementId;
};

/** Resolved symbols for one module, including declarations from its submodules. */
class ModuleSymbols {
 public:
  [[nodiscard]] const std::shared_ptr<const ResolvedModule>& module() const noexcept {
    return module_;
  }
  [[nodiscard]] const std::unordered_map<std::string, Symbol>& symbols(
      SymbolKind kind) const noexcept;
  [[nodiscard]] std::optional<Symbol> Find(SymbolKind kind,
                                           std::string_view name) const;

 private:
  friend class SymbolResolver;
  std::shared_ptr<const ResolvedModule> module_;
  std::array<std::unordered_map<std::string, Symbol>,
             static_cast<std::size_t>(SymbolKind::kCount)> tables_;
};

/** Semantic result for a resolved dependency closure. */
class SemanticContext {
 public:
  [[nodiscard]] const std::shared_ptr<const ModuleSymbols>& root() const noexcept {
    return root_;
  }
  [[nodiscard]] std::shared_ptr<const ModuleSymbols> FindModule(
      std::string_view name) const;
  [[nodiscard]] std::shared_ptr<const ModuleSymbols> FindModule(
      const ResolvedModule& module) const;

 private:
  friend class SymbolResolver;
  std::shared_ptr<const ModuleSymbols> root_;
  std::unordered_map<std::string, std::shared_ptr<const ModuleSymbols>> modules_;
  std::unordered_map<const ResolvedModule*, std::shared_ptr<const ModuleSymbols>> units_;
};

/** Builds symbol tables and validates cross-module symbolic references. */
class SymbolResolver {
 public:
  explicit SymbolResolver(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<SemanticContext> Resolve(
      std::shared_ptr<const ResolvedModule> root);

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_SEMANTIC_MODEL_H_
