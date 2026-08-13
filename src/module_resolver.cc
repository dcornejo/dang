// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/module_resolver.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

#include <fmt/format.h>

#include "yang/parser.h"
#include "yang/resource_limits.h"
#include "yang/validator.h"

namespace yang {
namespace {

const Statement* FindChild(const SyntaxTree& tree, const Statement& parent,
                           std::string_view keyword) {
  for (const StatementId id : parent.children) {
    const Statement& child = tree.Get(id);
    if (child.keyword == keyword) return &child;
  }
  return nullptr;
}

std::optional<std::string> RevisionOf(const SyntaxTree& tree, const Statement& root) {
  std::optional<std::string> latest;
  for (const StatementId id : root.children) {
    const Statement& child = tree.Get(id);
    if (child.keyword == "revision" && child.argument &&
        (!latest || *child.argument > *latest)) latest = *child.argument;
  }
  return latest;
}

std::optional<std::string_view> ChildArgument(const SyntaxTree& tree,
                                               const Statement& parent,
                                               std::string_view keyword) {
  const Statement* child = FindChild(tree, parent, keyword);
  if (child == nullptr || !child->argument) return std::nullopt;
  return *child->argument;
}

std::string Key(std::string_view name, std::optional<std::string_view> revision,
                ModuleKind kind) {
  return fmt::format("{}:{}@{}", kind == ModuleKind::kModule ? "module" : "submodule",
                     name, revision.value_or("latest"));
}

}  // namespace

std::shared_ptr<const SourceFile> FilesystemModuleRepository::Load(
    std::string_view name, std::optional<std::string_view> revision,
    ModuleKind /*kind*/, DiagnosticSink& diagnostics) {
  std::vector<std::filesystem::path> candidates;
  for (const auto& directory : search_paths_) {
    if (revision) {
      candidates.push_back(directory / fmt::format("{}@{}.yang", name, *revision));
      candidates.push_back(directory / fmt::format("{}.yang", name));
    } else {
      std::vector<std::filesystem::path> revisioned;
      std::error_code error;
      for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end;
           it.increment(error)) {
        const std::string filename = it->path().filename().string();
        const std::string prefix = std::string(name) + "@";
        if (filename.starts_with(prefix) && filename.ends_with(".yang")) {
          revisioned.push_back(it->path());
        }
      }
      std::sort(revisioned.begin(), revisioned.end(), std::greater<>());
      candidates.insert(candidates.end(), revisioned.begin(), revisioned.end());
      candidates.push_back(directory / fmt::format("{}.yang", name));
    }
  }
  for (const auto& path : candidates) {
    std::error_code size_error;
    const std::uintmax_t size = std::filesystem::file_size(path, size_error);
    if (!size_error && size > DefaultResourceLimits().maximum_source_bytes) {
      diagnostics.Report({DiagnosticCode::kResourceLimitExceeded,
                          DiagnosticSeverity::kError,
                          "YANG module exceeds the byte limit", {}});
      return nullptr;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) continue;
    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return SourceFile::Create(path.string(), std::move(contents), diagnostics);
  }
  return nullptr;
}

void InMemoryModuleRepository::Add(std::string name, std::string contents,
                                   std::optional<std::string> revision) {
  entries_.push_back({std::move(name), std::move(contents), std::move(revision)});
}

std::shared_ptr<const SourceFile> InMemoryModuleRepository::Load(
    std::string_view name, std::optional<std::string_view> revision,
    ModuleKind /*kind*/, DiagnosticSink& diagnostics) {
  const Entry* selected = nullptr;
  for (const auto& entry : entries_) {
    if (entry.name != name) continue;
    if (revision && entry.revision != revision) continue;
    if (selected == nullptr || entry.revision.value_or("") > selected->revision.value_or("")) {
      selected = &entry;
    }
  }
  if (selected == nullptr) return nullptr;
  const std::string filename = selected->revision
      ? fmt::format("{}@{}.yang", name, *selected->revision)
      : fmt::format("{}.yang", name);
  return SourceFile::Create(filename, selected->contents, diagnostics);
}

std::shared_ptr<const ResolvedModule> ModuleResolver::Resolve(
    std::shared_ptr<const SourceFile> source) {
  return ResolveSource(std::move(source), std::nullopt, std::nullopt,
                       ModuleKind::kModule, std::nullopt);
}

std::shared_ptr<const ResolvedModule> ModuleResolver::ResolveDependency(
    std::string_view name, std::optional<std::string_view> revision,
    ModuleKind kind, std::optional<std::string_view> including_module,
    SourceRange range) {
  const std::string key = Key(name, revision, kind);
  if (const auto cached = cache_.find(key); cached != cache_.end()) return cached->second;
  if (std::find(resolving_.begin(), resolving_.end(), key) != resolving_.end()) {
    diagnostics_.Report({DiagnosticCode::kDependencyCycle, DiagnosticSeverity::kError,
                         fmt::format("dependency cycle detected while resolving '{}'", name), range});
    return nullptr;
  }
  auto source = repository_.Load(name, revision, kind, diagnostics_);
  if (!source) {
    diagnostics_.Report({DiagnosticCode::kModuleNotFound, DiagnosticSeverity::kError,
        fmt::format("{} '{}'{} was not found", kind == ModuleKind::kModule ? "module" : "submodule",
                    name, revision ? fmt::format(" at revision {}", *revision) : ""), range});
    return nullptr;
  }
  return ResolveSource(std::move(source), name, revision, kind, including_module);
}

std::shared_ptr<const ResolvedModule> ModuleResolver::ResolveSource(
    std::shared_ptr<const SourceFile> source, std::optional<std::string_view> expected_name,
    std::optional<std::string_view> expected_revision, ModuleKind expected_kind,
    std::optional<std::string_view> including_module) {
  Parser parser(source, diagnostics_);
  auto syntax = std::make_shared<SyntaxTree>(parser.Parse());
  Validator validator(diagnostics_);
  if (!validator.Validate(*syntax) || syntax->roots().size() != 1) return nullptr;
  const Statement& root = syntax->Get(syntax->roots().front());
  const ModuleKind actual_kind = root.keyword == "module" ? ModuleKind::kModule : ModuleKind::kSubmodule;
  if (actual_kind != expected_kind || !root.argument) {
    diagnostics_.Report({DiagnosticCode::kModuleNameMismatch, DiagnosticSeverity::kError,
                         "loaded source has the wrong module kind", root.range});
    return nullptr;
  }
  if (expected_name && *root.argument != *expected_name) {
    diagnostics_.Report({DiagnosticCode::kModuleNameMismatch, DiagnosticSeverity::kError,
        fmt::format("expected '{}', but loaded source declares '{}'", *expected_name, *root.argument), root.range});
    return nullptr;
  }
  const auto revision = RevisionOf(*syntax, root);
  if (expected_revision && revision != expected_revision) {
    diagnostics_.Report({DiagnosticCode::kRevisionMismatch, DiagnosticSeverity::kError,
        fmt::format("module '{}' does not declare requested revision '{}'", *root.argument, *expected_revision), root.range});
    return nullptr;
  }
  if (actual_kind == ModuleKind::kSubmodule && including_module) {
    const auto belongs_to = ChildArgument(*syntax, root, "belongs-to");
    if (!belongs_to || *belongs_to != *including_module) {
      diagnostics_.Report({DiagnosticCode::kBelongsToMismatch, DiagnosticSeverity::kError,
          fmt::format("submodule '{}' does not belong to module '{}'", *root.argument, *including_module), root.range});
      return nullptr;
    }
  }

  const std::string requested_key = Key(*root.argument, expected_revision, actual_kind);
  const std::string actual_key = Key(*root.argument, revision, actual_kind);
  if (const auto cached = cache_.find(actual_key); cached != cache_.end()) return cached->second;
  if (std::find(resolving_.begin(), resolving_.end(), requested_key) != resolving_.end()) {
    diagnostics_.Report({DiagnosticCode::kDependencyCycle, DiagnosticSeverity::kError,
                         fmt::format("dependency cycle detected at '{}'", *root.argument), root.range});
    return nullptr;
  }
  resolving_.push_back(requested_key);

  auto resolved = std::make_shared<ResolvedModule>();
  resolved->kind = actual_kind;
  resolved->name = *root.argument;
  resolved->revision = revision;
  resolved->syntax = std::move(syntax);
  if (actual_kind == ModuleKind::kModule) {
    resolved->namespace_uri = std::string(ChildArgument(*resolved->syntax, root, "namespace").value_or(""));
    resolved->prefix = std::string(ChildArgument(*resolved->syntax, root, "prefix").value_or(""));
  } else if (const Statement* belongs_to = FindChild(*resolved->syntax, root, "belongs-to")) {
    resolved->belongs_to = belongs_to->argument;
    resolved->prefix = std::string(ChildArgument(*resolved->syntax, *belongs_to, "prefix").value_or(""));
  }

  bool success = true;
  for (const StatementId id : root.children) {
    const Statement& dependency = resolved->syntax->Get(id);
    if ((dependency.keyword != "import" && dependency.keyword != "include") || !dependency.argument) continue;
    const ModuleKind kind = dependency.keyword == "import" ? ModuleKind::kModule : ModuleKind::kSubmodule;
    const auto requested_revision = ChildArgument(*resolved->syntax, dependency, "revision-date");
    auto target = ResolveDependency(*dependency.argument, requested_revision, kind,
        kind == ModuleKind::kSubmodule ? std::optional<std::string_view>(resolved->name) : std::nullopt,
        dependency.range);
    if (!target) { success = false; continue; }
    if (kind == ModuleKind::kModule) {
      const auto prefix = ChildArgument(*resolved->syntax, dependency, "prefix");
      if (!prefix || resolved->imports.contains(std::string(*prefix)) || *prefix == resolved->prefix) {
        diagnostics_.Report({DiagnosticCode::kDuplicatePrefix, DiagnosticSeverity::kError,
                             fmt::format("prefix '{}' is not unique", prefix.value_or("")), dependency.range});
        success = false;
      } else {
        resolved->imports.emplace(*prefix, std::move(target));
      }
    } else {
      resolved->includes.push_back(std::move(target));
    }
  }
  resolving_.pop_back();
  if (!success) return nullptr;
  cache_[actual_key] = resolved;
  cache_[requested_key] = resolved;
  return resolved;
}

}  // namespace yang
