// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_XPATH_H_
#define YANG_XPATH_H_

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "yang/leafref.h"

namespace yang::semantic {

/** Kind of YANG schema constraint represented by an XPath expression. */
enum class XPathConstraintKind { kMust, kWhen };

/** XPath 1.0 static value categories. */
enum class XPathValueType { kBoolean, kNumber, kString, kNodeSet, kUnknown };

/** A parsed XPath constraint and the schema nodes resolved statically. */
struct ValidatedXPath {
  XPathConstraintKind kind = XPathConstraintKind::kMust;
  SchemaNodeRef constrained_node;
  SchemaNodeRef context;
  const ResolvedModule* source_module = nullptr;
  StatementId statement = kInvalidStatementId;
  std::string expression;
  std::optional<std::string> error_message;
  std::optional<std::string> error_app_tag;
  XPathValueType inferred_type = XPathValueType::kUnknown;
  std::vector<SchemaNodeRef> statically_resolved_nodes;
};

/** Collection of parsed and statically checked schema XPath expressions. */
class XPathContext {
 public:
  /** Returns all validated constraints in effective-schema traversal order. */
  [[nodiscard]] const std::vector<ValidatedXPath>& expressions() const noexcept {
    return expressions_;
  }

 private:
  friend class XPathValidator;
  std::vector<ValidatedXPath> expressions_;
};

/** Parses and statically validates must/when XPath expressions. */
class XPathValidator {
 public:
  explicit XPathValidator(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}

  /**
   * Parses and statically validates all must and when expressions.
   *
   * @return The validated expressions, or std::nullopt if an error is found.
   */
  [[nodiscard]] std::optional<XPathContext> Validate(
      const SchemaContext& schemas);

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_XPATH_H_
