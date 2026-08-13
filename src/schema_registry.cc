// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/schema_registry.h"

#include <array>

namespace yang {
namespace {

// RFC 7950 section 13 and section 11.  The final flag identifies arguments
// represented by a child element instead of an XML attribute in YIN.
constexpr std::array kBuiltins{
    StatementSpec{"action", ArgumentRequirement::kRequired, "name", false,
                  YangVersion::k1_1},
    StatementSpec{"anydata", ArgumentRequirement::kRequired, "name", false,
                  YangVersion::k1_1},
    StatementSpec{"anyxml", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"argument", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"augment", ArgumentRequirement::kRequired, "target-node"},
    StatementSpec{"base", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"belongs-to", ArgumentRequirement::kRequired, "module"},
    StatementSpec{"bit", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"case", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"choice", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"config", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"contact", ArgumentRequirement::kRequired, "text", true},
    StatementSpec{"container", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"default", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"description", ArgumentRequirement::kRequired, "text", true},
    StatementSpec{"deviate", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"deviation", ArgumentRequirement::kRequired, "target-node"},
    StatementSpec{"enum", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"error-app-tag", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"error-message", ArgumentRequirement::kRequired, "value", true},
    StatementSpec{"extension", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"feature", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"fraction-digits", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"grouping", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"identity", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"if-feature", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"import", ArgumentRequirement::kRequired, "module"},
    StatementSpec{"include", ArgumentRequirement::kRequired, "module"},
    StatementSpec{"input", ArgumentRequirement::kNone, ""},
    StatementSpec{"key", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"leaf", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"leaf-list", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"length", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"list", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"mandatory", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"max-elements", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"min-elements", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"modifier", ArgumentRequirement::kRequired, "value", false,
                  YangVersion::k1_1},
    StatementSpec{"module", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"must", ArgumentRequirement::kRequired, "condition"},
    StatementSpec{"namespace", ArgumentRequirement::kRequired, "uri"},
    StatementSpec{"notification", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"ordered-by", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"organization", ArgumentRequirement::kRequired, "text", true},
    StatementSpec{"output", ArgumentRequirement::kNone, ""},
    StatementSpec{"path", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"pattern", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"position", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"prefix", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"presence", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"range", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"reference", ArgumentRequirement::kRequired, "text", true},
    StatementSpec{"refine", ArgumentRequirement::kRequired, "target-node"},
    StatementSpec{"require-instance", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"revision", ArgumentRequirement::kRequired, "date"},
    StatementSpec{"revision-date", ArgumentRequirement::kRequired, "date"},
    StatementSpec{"rpc", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"status", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"submodule", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"type", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"typedef", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"unique", ArgumentRequirement::kRequired, "tag"},
    StatementSpec{"units", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"uses", ArgumentRequirement::kRequired, "name"},
    StatementSpec{"value", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"when", ArgumentRequirement::kRequired, "condition"},
    StatementSpec{"yang-version", ArgumentRequirement::kRequired, "value"},
    StatementSpec{"yin-element", ArgumentRequirement::kRequired, "value"},
};

}  // namespace

std::span<const StatementSpec> SchemaRegistry::Builtins() noexcept {
  return kBuiltins;
}

std::optional<StatementSpec> SchemaRegistry::Find(
    std::string_view keyword) noexcept {
  for (const auto& spec : kBuiltins) {
    if (spec.keyword == keyword) return spec;
  }
  return std::nullopt;
}

bool SchemaRegistry::IsAvailable(std::string_view keyword,
                                 YangVersion version) noexcept {
  const auto spec = Find(keyword);
  return spec && (version == YangVersion::k1_1 ||
                  spec->introduced == YangVersion::k1);
}

std::span<const std::string_view> SchemaRegistry::RequiredChildren(
    std::string_view parent) noexcept {
  static constexpr std::array<std::string_view, 2> module{"namespace", "prefix"};
  static constexpr std::array<std::string_view, 1> submodule{"belongs-to"};
  static constexpr std::array<std::string_view, 1> prefix{"prefix"};
  static constexpr std::array<std::string_view, 1> type{"type"};
  static constexpr std::array<std::string_view, 0> none{};
  if (parent == "module") return module;
  if (parent == "submodule") return submodule;
  if (parent == "belongs-to" || parent == "import") return prefix;
  if (parent == "leaf" || parent == "leaf-list" || parent == "typedef") {
    return type;
  }
  return none;
}

}  // namespace yang
