// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SCHEMA_PATH_H_
#define YANG_SCHEMA_PATH_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "yang/schema_tree.h"

namespace yang::semantic {

enum class SchemaPathKind { kAbsolute, kDescendant };

struct SchemaPathSegment {
  std::optional<std::string> prefix;
  std::string identifier;
};

struct SchemaPath {
  SchemaPathKind kind = SchemaPathKind::kDescendant;
  std::vector<SchemaPathSegment> segments;
};

/** Parses absolute and descendant schema-node identifiers. */
class SchemaPathParser {
 public:
  explicit SchemaPathParser(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<SchemaPath> Parse(
      std::string_view value, SchemaPathKind expected_kind, SourceRange range);

 private:
  DiagnosticSink& diagnostics_;
};

/** Resolves schema paths against one effective tree. */
class SchemaPathResolver {
 public:
  SchemaPathResolver(const ResolvedModule& source, const SchemaTree& tree,
                     DiagnosticSink& diagnostics)
      : source_(source), tree_(tree), diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<SchemaNodeId> ResolveAbsolute(
      const SchemaPath& path, SourceRange range) const;
  [[nodiscard]] std::optional<SchemaNodeId> ResolveDescendant(
      SchemaNodeId start, const SchemaPath& path, SourceRange range) const;

 private:
  [[nodiscard]] std::optional<std::string> ModuleFor(
      const SchemaPathSegment& segment, SourceRange range) const;
  [[nodiscard]] std::optional<SchemaNodeId> FindStep(
      std::optional<SchemaNodeId> parent, const SchemaName& name) const;
  const ResolvedModule& source_;
  const SchemaTree& tree_;
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_SCHEMA_PATH_H_
