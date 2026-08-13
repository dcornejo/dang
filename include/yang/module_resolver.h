// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_MODULE_RESOLVER_H_
#define YANG_MODULE_RESOLVER_H_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "yang/diagnostic.h"
#include "yang/syntax_tree.h"

namespace yang {

enum class ModuleKind { kModule, kSubmodule };

/** Supplies a named YANG module or submodule to the resolver. */
class ModuleSourceRepository {
 public:
  virtual ~ModuleSourceRepository() = default;
  [[nodiscard]] virtual std::shared_ptr<const SourceFile> Load(
      std::string_view name, std::optional<std::string_view> revision,
      ModuleKind kind, DiagnosticSink& diagnostics) = 0;
};

/** Search-path repository supporting name.yang and name@revision.yang. */
class FilesystemModuleRepository final : public ModuleSourceRepository {
 public:
  explicit FilesystemModuleRepository(std::vector<std::filesystem::path> search_paths)
      : search_paths_(std::move(search_paths)) {}
  [[nodiscard]] std::shared_ptr<const SourceFile> Load(
      std::string_view name, std::optional<std::string_view> revision,
      ModuleKind kind, DiagnosticSink& diagnostics) override;

 private:
  std::vector<std::filesystem::path> search_paths_;
};

/** In-memory repository primarily useful to embedders and tests. */
class InMemoryModuleRepository final : public ModuleSourceRepository {
 public:
  void Add(std::string name, std::string contents,
           std::optional<std::string> revision = std::nullopt);
  [[nodiscard]] std::shared_ptr<const SourceFile> Load(
      std::string_view name, std::optional<std::string_view> revision,
      ModuleKind kind, DiagnosticSink& diagnostics) override;

 private:
  struct Entry { std::string name; std::string contents; std::optional<std::string> revision; };
  std::vector<Entry> entries_;
};

/** A parsed module with its imports, includes, namespaces, and prefixes resolved. */
struct ResolvedModule {
  ModuleKind kind = ModuleKind::kModule;
  std::string name;
  std::optional<std::string> revision;
  std::string namespace_uri;
  std::string prefix;
  std::optional<std::string> belongs_to;
  std::shared_ptr<SyntaxTree> syntax;
  std::unordered_map<std::string, std::shared_ptr<const ResolvedModule>> imports;
  std::vector<std::shared_ptr<const ResolvedModule>> includes;
};

/** Resolves and caches the dependency closure of a YANG module. */
class ModuleResolver {
 public:
  ModuleResolver(ModuleSourceRepository& repository, DiagnosticSink& diagnostics)
      : repository_(repository), diagnostics_(diagnostics) {}

  [[nodiscard]] std::shared_ptr<const ResolvedModule> Resolve(
      std::shared_ptr<const SourceFile> source);
  [[nodiscard]] std::size_t cache_size() const noexcept { return cache_.size(); }

 private:
  [[nodiscard]] std::shared_ptr<const ResolvedModule> ResolveSource(
      std::shared_ptr<const SourceFile> source, std::optional<std::string_view> expected_name,
      std::optional<std::string_view> expected_revision, ModuleKind expected_kind,
      std::optional<std::string_view> including_module);
  [[nodiscard]] std::shared_ptr<const ResolvedModule> ResolveDependency(
      std::string_view name, std::optional<std::string_view> revision,
      ModuleKind kind, std::optional<std::string_view> including_module,
      SourceRange range);

  ModuleSourceRepository& repository_;
  DiagnosticSink& diagnostics_;
  std::unordered_map<std::string, std::shared_ptr<const ResolvedModule>> cache_;
  std::vector<std::string> resolving_;
};

}  // namespace yang
#endif  // YANG_MODULE_RESOLVER_H_
