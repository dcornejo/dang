// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/validator.h"

#include <array>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include "yang/schema_registry.h"

namespace yang {
namespace {

template <typename T, std::size_t N>
bool Contains(std::string_view value, const std::array<T, N>& values) {
  for (const auto candidate : values) if (std::string_view(candidate) == value) return true;
  return false;
}

bool IsExtension(std::string_view keyword) { return keyword.find(':') != std::string_view::npos; }

bool IsDataDefinition(std::string_view keyword) {
  constexpr std::array values{"container", "leaf", "leaf-list", "list", "choice",
                              "anydata", "anyxml", "uses"};
  return Contains(keyword, values);
}

}  // namespace

bool SchemaRegistry::IsAllowed(std::string_view parent,
                               std::string_view child) noexcept {
  if (IsExtension(child)) return true;
  if (parent == "module") {
    constexpr std::array values{"yang-version", "namespace", "prefix", "include", "import",
        "organization", "contact", "description", "reference", "revision", "extension",
        "feature", "identity", "typedef", "grouping", "augment", "rpc", "notification",
        "deviation", "container", "leaf", "leaf-list", "list", "choice", "anydata", "anyxml", "uses"};
    return Contains(child, values);
  }
  if (parent == "submodule") {
    constexpr std::array values{"yang-version", "belongs-to", "include", "import", "organization",
        "contact", "description", "reference", "revision", "extension", "feature", "identity",
        "typedef", "grouping", "augment", "rpc", "notification", "deviation", "container",
        "leaf", "leaf-list", "list", "choice", "anydata", "anyxml", "uses"};
    return Contains(child, values);
  }
  if (parent == "container") {
    constexpr std::array values{"when", "if-feature", "must", "presence", "config", "status",
        "description", "reference", "typedef", "grouping", "action", "notification"};
    return IsDataDefinition(child) || Contains(child, values);
  }
  if (parent == "list") {
    constexpr std::array values{"when", "if-feature", "must", "key", "unique", "config",
        "min-elements", "max-elements", "ordered-by", "status", "description", "reference",
        "typedef", "grouping", "action", "notification"};
    return IsDataDefinition(child) || Contains(child, values);
  }
  if (parent == "grouping") {
    constexpr std::array values{"status", "description", "reference", "typedef", "grouping",
        "action", "notification"};
    return IsDataDefinition(child) || Contains(child, values);
  }
  if (parent == "case") {
    constexpr std::array values{"when", "if-feature", "status", "description", "reference"};
    return IsDataDefinition(child) || Contains(child, values);
  }
  if (parent == "input" || parent == "output") {
    constexpr std::array values{"must", "typedef", "grouping"};
    return IsDataDefinition(child) || Contains(child, values);
  }
  if (parent == "notification") {
    constexpr std::array values{"if-feature", "must", "status", "description", "reference",
        "typedef", "grouping"};
    return IsDataDefinition(child) || Contains(child, values);
  }
  if (parent == "leaf") {
    constexpr std::array values{"when", "if-feature", "type", "units", "must", "default",
        "config", "mandatory", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "leaf-list") {
    constexpr std::array values{"when", "if-feature", "type", "units", "must", "default", "config",
        "min-elements", "max-elements", "ordered-by", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "choice") {
    constexpr std::array values{"when", "if-feature", "default", "config", "mandatory", "status",
        "description", "reference", "case", "choice", "container", "leaf", "leaf-list", "list",
        "anydata", "anyxml"};
    return Contains(child, values);
  }
  if (parent == "rpc" || parent == "action") {
    constexpr std::array values{"if-feature", "status", "description", "reference", "typedef",
        "grouping", "input", "output"};
    return Contains(child, values);
  }
  if (parent == "typedef") {
    constexpr std::array values{"type", "units", "default", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "type") {
    constexpr std::array values{"range", "fraction-digits", "length", "pattern", "modifier", "path",
        "require-instance", "base", "bit", "enum", "type"};
    return Contains(child, values);
  }
  if (parent == "uses") {
    constexpr std::array values{"when", "if-feature", "refine", "augment", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "augment") return IsDataDefinition(child) || child == "case" || child == "action" ||
      child == "notification" || child == "when" || child == "if-feature" || child == "status" ||
      child == "description" || child == "reference";
  if (parent == "import") {
    constexpr std::array values{"prefix", "revision-date", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "include") {
    constexpr std::array values{"revision-date", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "belongs-to") return child == "prefix";
  if (parent == "extension") {
    constexpr std::array values{"argument", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "argument") return child == "yin-element";
  if (parent == "feature") {
    constexpr std::array values{"if-feature", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "identity") {
    constexpr std::array values{"if-feature", "base", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "revision") {
    constexpr std::array values{"description", "reference"};
    return Contains(child, values);
  }
  if (parent == "must" || parent == "when") {
    constexpr std::array values{"error-message", "error-app-tag", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "range" || parent == "length") {
    constexpr std::array values{"error-message", "error-app-tag", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "pattern") {
    constexpr std::array values{"modifier", "error-message", "error-app-tag", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "bit" || parent == "enum") {
    constexpr std::array values{"if-feature", "position", "value", "status", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "refine") {
    constexpr std::array values{"must", "presence", "default", "config", "mandatory",
        "min-elements", "max-elements", "description", "reference"};
    return Contains(child, values);
  }
  if (parent == "deviation") return child == "description" || child == "reference" || child == "deviate";
  if (parent == "deviate") {
    constexpr std::array values{"type", "units", "must", "unique", "default", "config", "mandatory",
        "min-elements", "max-elements"};
    return Contains(child, values);
  }
  if (parent == "anydata" || parent == "anyxml") {
    constexpr std::array values{"when", "if-feature", "must", "config", "mandatory", "status",
        "description", "reference"};
    return Contains(child, values);
  }
  return false;
}

bool SchemaRegistry::IsRepeatable(std::string_view parent,
                                  std::string_view child) noexcept {
  if (IsExtension(child) || IsDataDefinition(child)) return true;
  constexpr std::array always{"if-feature", "must", "unique", "typedef", "grouping", "action",
      "notification", "revision", "import", "include", "extension", "feature", "identity", "augment",
      "rpc", "deviation", "deviate", "refine", "bit", "enum", "pattern", "case"};
  if (Contains(child, always)) return true;
  return (parent == "type" && (child == "type" || child == "base")) ||
         (parent == "identity" && child == "base") ||
         (parent == "leaf-list" && child == "default");
}

int SchemaRegistry::StatementOrder(std::string_view child) noexcept {
  if (child == "yang-version") return 0;
  if (child == "namespace" || child == "prefix" || child == "belongs-to") return 1;
  if (child == "import" || child == "include") return 2;
  if (child == "organization" || child == "contact" || child == "description" || child == "reference") return 3;
  if (child == "revision") return 4;
  return 5;
}

bool Validator::Validate(const SyntaxTree& tree) {
  bool valid = true;
  version_ = YangVersion::k1;
  if (tree.roots().size() != 1) {
    const SourceRange range = tree.roots().empty() ? SourceRange{} : tree.Get(tree.roots().front()).range;
    diagnostics_.Report({DiagnosticCode::kInvalidRootStatement, DiagnosticSeverity::kError,
                         "a YANG source must contain exactly one module or submodule statement", range});
    valid = false;
  }
  for (const StatementId id : tree.roots()) {
    const Statement& root = tree.Get(id);
    if (root.keyword != "module" && root.keyword != "submodule") {
      diagnostics_.Report({DiagnosticCode::kInvalidRootStatement, DiagnosticSeverity::kError,
                           "the root statement must be 'module' or 'submodule'", root.range});
      valid = false;
    }
    for (const StatementId child_id : root.children) {
      const Statement& child = tree.Get(child_id);
      if (child.keyword != "yang-version" || !child.argument) continue;
      if (*child.argument == "1.1") {
        version_ = YangVersion::k1_1;
      } else if (*child.argument != "1") {
        diagnostics_.Report({DiagnosticCode::kUnsupportedYangVersion,
                             DiagnosticSeverity::kError,
                             fmt::format("unsupported YANG version '{}'",
                                         *child.argument),
                             child.range});
        valid = false;
      }
    }
    valid = ValidateStatement(tree, id, "") && valid;
  }
  return valid;
}

bool Validator::ValidateStatement(const SyntaxTree& tree, StatementId id, std::string_view parent) {
  const Statement& statement = tree.Get(id);
  bool valid = true;
  const auto spec = SchemaRegistry::Find(statement.keyword);
  if (!spec && !IsExtension(statement.keyword)) {
    diagnostics_.Report({DiagnosticCode::kUnknownStatement, DiagnosticSeverity::kError,
                         fmt::format("unknown YANG statement '{}'", statement.keyword), statement.range});
    valid = false;
  }
  if (spec) {
    if (!SchemaRegistry::IsAvailable(statement.keyword, version_)) {
      diagnostics_.Report({DiagnosticCode::kStatementRequiresYang11,
                           DiagnosticSeverity::kError,
                           fmt::format("statement '{}' requires YANG 1.1",
                                       statement.keyword),
                           statement.range});
      valid = false;
    }
    if (spec->argument == ArgumentRequirement::kRequired && !statement.argument) {
      diagnostics_.Report({DiagnosticCode::kMissingArgument, DiagnosticSeverity::kError,
                           fmt::format("statement '{}' requires an argument", statement.keyword), statement.range});
      valid = false;
    }
    if (spec->argument == ArgumentRequirement::kNone && statement.argument) {
      diagnostics_.Report({DiagnosticCode::kUnexpectedArgument, DiagnosticSeverity::kError,
                           fmt::format("statement '{}' does not take an argument", statement.keyword), statement.range});
      valid = false;
    }
  }
  if (!parent.empty() &&
      !SchemaRegistry::IsAllowed(parent, statement.keyword)) {
    diagnostics_.Report({DiagnosticCode::kInvalidSubstatement, DiagnosticSeverity::kError,
                         fmt::format("statement '{}' is not permitted within '{}'", statement.keyword, parent), statement.range});
    valid = false;
  }
  if (version_ == YangVersion::k1 && parent == "leaf-list" &&
      statement.keyword == "default") {
    diagnostics_.Report({DiagnosticCode::kStatementRequiresYang11,
                         DiagnosticSeverity::kError,
                         "leaf-list defaults require YANG 1.1", statement.range});
    valid = false;
  }
  if (version_ == YangVersion::k1 && statement.keyword == "if-feature" &&
      statement.argument &&
      statement.argument->find_first_of(" ()") != std::string::npos) {
    diagnostics_.Report({DiagnosticCode::kStatementRequiresYang11,
                         DiagnosticSeverity::kError,
                         "boolean if-feature expressions require YANG 1.1",
                         statement.range});
    valid = false;
  }
  return ValidateChildren(tree, statement) && valid;
}

bool Validator::ValidateChildren(const SyntaxTree& tree, const Statement& statement) {
  bool valid = true;
  std::unordered_map<std::string_view, std::size_t> counts;
  int previous_order = -1;
  for (const StatementId child_id : statement.children) {
    const Statement& child = tree.Get(child_id);
    const std::size_t count = ++counts[child.keyword];
    if (count > 1 &&
        !SchemaRegistry::IsRepeatable(statement.keyword, child.keyword)) {
      diagnostics_.Report({DiagnosticCode::kDuplicateStatement, DiagnosticSeverity::kError,
                           fmt::format("statement '{}' may appear at most once within '{}'", child.keyword, statement.keyword), child.range});
      valid = false;
    }
    if ((statement.keyword == "module" || statement.keyword == "submodule") && !IsExtension(child.keyword)) {
      const int order = SchemaRegistry::StatementOrder(child.keyword);
      if (order < previous_order) {
        diagnostics_.Report({DiagnosticCode::kInvalidStatementOrder, DiagnosticSeverity::kError,
                             fmt::format("statement '{}' appears out of RFC 7950 order", child.keyword), child.range});
        valid = false;
      }
      previous_order = order;
    }
    valid = ValidateStatement(tree, child_id, statement.keyword) && valid;
  }
  const auto require = [&](std::string_view keyword) {
    if (!counts.contains(keyword)) {
      diagnostics_.Report({DiagnosticCode::kMissingRequiredStatement, DiagnosticSeverity::kError,
                           fmt::format("statement '{}' requires a '{}' substatement", statement.keyword, keyword), statement.range});
      valid = false;
    }
  };
  for (const std::string_view required :
       SchemaRegistry::RequiredChildren(statement.keyword)) {
    require(required);
  }
  if (version_ == YangVersion::k1 && statement.keyword == "identity" &&
      counts["base"] > 1) {
    diagnostics_.Report({DiagnosticCode::kStatementRequiresYang11,
                         DiagnosticSeverity::kError,
                         "multiple identity bases require YANG 1.1",
                         statement.range});
    valid = false;
  }
  if (version_ == YangVersion::k1 && statement.keyword == "type" &&
      statement.argument == "leafref" && counts.contains("require-instance")) {
    diagnostics_.Report({DiagnosticCode::kStatementRequiresYang11,
                         DiagnosticSeverity::kError,
                         "leafref require-instance requires YANG 1.1",
                         statement.range});
    valid = false;
  }
  return valid;
}

}  // namespace yang
