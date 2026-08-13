// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/schema_path.h"

#include <cctype>

#include <fmt/format.h>

namespace yang::semantic {
namespace {

bool ValidIdentifier(std::string_view value) {
  if (value.empty()) return false;
  const auto start = [](unsigned char c) { return std::isalpha(c) != 0 || c == '_'; };
  if (!start(static_cast<unsigned char>(value.front()))) return false;
  for (const char character : value.substr(1)) {
    const unsigned char c = static_cast<unsigned char>(character);
    if (!start(c) && std::isdigit(c) == 0 && c != '-' && c != '.') return false;
  }
  return true;
}

}  // namespace

std::optional<SchemaPath> SchemaPathParser::Parse(
    std::string_view value, SchemaPathKind expected_kind, SourceRange range) {
  const bool absolute = !value.empty() && value.front() == '/';
  if (value.empty() || absolute != (expected_kind == SchemaPathKind::kAbsolute)) {
    diagnostics_.Report({DiagnosticCode::kInvalidSchemaPath, DiagnosticSeverity::kError,
        expected_kind == SchemaPathKind::kAbsolute
            ? "expected an absolute schema-node identifier beginning with '/'"
            : "expected a non-empty descendant schema-node identifier without a leading '/'",
        range});
    return std::nullopt;
  }
  if (absolute) value.remove_prefix(1);
  SchemaPath result{.kind = expected_kind};
  while (!value.empty()) {
    const std::size_t slash = value.find('/');
    const std::string_view part = value.substr(0, slash);
    if (part.empty()) {
      diagnostics_.Report({DiagnosticCode::kInvalidSchemaPath, DiagnosticSeverity::kError,
                           "schema path contains an empty segment", range});
      return std::nullopt;
    }
    SchemaPathSegment segment;
    const std::size_t colon = part.find(':');
    if (colon == std::string_view::npos) {
      segment.identifier = part;
    } else if (part.find(':', colon + 1) == std::string_view::npos) {
      segment.prefix = std::string(part.substr(0, colon));
      segment.identifier = part.substr(colon + 1);
    }
    if (!ValidIdentifier(segment.identifier) ||
        (segment.prefix && !ValidIdentifier(*segment.prefix))) {
      diagnostics_.Report({DiagnosticCode::kInvalidSchemaPath, DiagnosticSeverity::kError,
                           fmt::format("'{}' is not a valid schema path segment", part), range});
      return std::nullopt;
    }
    result.segments.push_back(std::move(segment));
    if (slash == std::string_view::npos) break;
    value.remove_prefix(slash + 1);
    if (value.empty()) {
      diagnostics_.Report({DiagnosticCode::kInvalidSchemaPath, DiagnosticSeverity::kError,
                           "schema path contains an empty segment", range});
      return std::nullopt;
    }
  }
  return result;
}

std::optional<std::string> SchemaPathResolver::ModuleFor(
    const SchemaPathSegment& segment, SourceRange range) const {
  const std::string local_module = source_.belongs_to.value_or(source_.name);
  if (!segment.prefix || *segment.prefix == source_.prefix) return local_module;
  const auto imported = source_.imports.find(*segment.prefix);
  if (imported != source_.imports.end()) return imported->second->name;
  diagnostics_.Report({DiagnosticCode::kUnknownPrefix, DiagnosticSeverity::kError,
                       fmt::format("unknown schema path prefix '{}'", *segment.prefix), range});
  return std::nullopt;
}

std::optional<SchemaNodeId> SchemaPathResolver::FindStep(
    std::optional<SchemaNodeId> parent, const SchemaName& name) const {
  if (const auto direct = tree_.FindChild(parent, name)) {
    const SchemaNode& node = tree_.Get(*direct);
    if (node.origin == SchemaNodeOrigin::kImplicitCase) {
      if (const auto nested = tree_.FindChild(*direct, name)) return nested;
    }
    return direct;
  }
  const auto& candidates = parent ? tree_.Get(*parent).children : tree_.roots();
  for (const SchemaNodeId id : candidates) {
    const SchemaNode& candidate = tree_.Get(id);
    if (candidate.kind != SchemaNodeKind::kCase) continue;
    if (const auto nested = tree_.FindChild(id, name)) return nested;
  }
  return std::nullopt;
}

std::optional<SchemaNodeId> SchemaPathResolver::ResolveAbsolute(
    const SchemaPath& path, SourceRange range) const {
  if (path.kind != SchemaPathKind::kAbsolute || path.segments.empty()) return std::nullopt;
  std::optional<SchemaNodeId> current;
  for (const auto& segment : path.segments) {
    const auto module = ModuleFor(segment, range);
    if (!module) return std::nullopt;
    current = FindStep(current, {*module, segment.identifier});
    if (!current) {
      diagnostics_.Report({DiagnosticCode::kUnknownSchemaNode, DiagnosticSeverity::kError,
                           fmt::format("schema node '{}:{}' was not found", *module, segment.identifier), range});
      return std::nullopt;
    }
  }
  return current;
}

std::optional<SchemaNodeId> SchemaPathResolver::ResolveDescendant(
    SchemaNodeId start, const SchemaPath& path, SourceRange range) const {
  if (path.kind != SchemaPathKind::kDescendant || path.segments.empty()) return std::nullopt;
  std::optional<SchemaNodeId> current = start;
  for (const auto& segment : path.segments) {
    const auto module = ModuleFor(segment, range);
    if (!module) return std::nullopt;
    current = FindStep(current, {*module, segment.identifier});
    if (!current) {
      diagnostics_.Report({DiagnosticCode::kUnknownSchemaNode, DiagnosticSeverity::kError,
                           fmt::format("descendant schema node '{}:{}' was not found", *module,
                                       segment.identifier), range});
      return std::nullopt;
    }
  }
  return current;
}

}  // namespace yang::semantic
