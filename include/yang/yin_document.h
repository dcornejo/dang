// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_YIN_DOCUMENT_H_
#define YANG_YIN_DOCUMENT_H_

#include <pugixml.hpp>
#include <nlohmann/json_fwd.hpp>

#include <optional>
#include <string>

#include "yang/diagnostic.h"
#include "yang/syntax_tree.h"

namespace yang {
struct ResolvedModule;
namespace semantic {
class SchemaContext;
}
/** An in-memory YIN document backed by pugixml. */
class YinDocument {
 public:
  [[nodiscard]] pugi::xml_document& document() noexcept { return document_; }
  [[nodiscard]] const pugi::xml_document& document() const noexcept { return document_; }
  /** Serializes the document as UTF-8 XML. */
  [[nodiscard]] std::string ToString(bool pretty = true) const;
  /** Serializes the complete ordered XML/YIN tree to library JSON. */
  [[nodiscard]] nlohmann::json ToJson() const;
  /** Reconstructs a YIN tree previously produced by ToJson(). */
  [[nodiscard]] static std::optional<YinDocument> FromJson(
      const nlohmann::json& value, DiagnosticSink& diagnostics);
 private:
  pugi::xml_document document_;
};

/** Converts a validated, self-contained syntax tree to RFC 7950 YIN XML. */
class YinConverter {
 public:
  explicit YinConverter(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<YinDocument> Convert(const SyntaxTree& tree);
  /** Converts a module using resolved imported extension namespaces. */
  [[nodiscard]] std::optional<YinDocument> Convert(const ResolvedModule& module);
  /** Converts an included submodule using its owning module's namespace. */
  [[nodiscard]] std::optional<YinDocument> ConvertSubmodule(
      const ResolvedModule& submodule, const ResolvedModule& owner);
  /** Produces a canonical YIN view of a module's effective schema tree. */
  [[nodiscard]] std::optional<YinDocument> ConvertEffective(
      const semantic::SchemaContext& schemas, const ResolvedModule& module);

 private:
  DiagnosticSink& diagnostics_;
};
}  // namespace yang
#endif  // YANG_YIN_DOCUMENT_H_
