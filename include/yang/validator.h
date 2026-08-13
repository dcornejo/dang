// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_VALIDATOR_H_
#define YANG_VALIDATOR_H_

#include <string_view>

#include "yang/diagnostic.h"
#include "yang/schema_registry.h"
#include "yang/syntax_tree.h"

namespace yang {

/** Performs context-free RFC 7950 structural validation. */
class Validator {
 public:
  explicit Validator(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}

  /** Validates roots, arguments, legal substatements, and core cardinalities. */
  [[nodiscard]] bool Validate(const SyntaxTree& tree);

 private:
  [[nodiscard]] bool ValidateStatement(
      const SyntaxTree& tree, StatementId id, std::string_view parent);
  [[nodiscard]] bool ValidateChildren(const SyntaxTree& tree, const Statement& statement);
  DiagnosticSink& diagnostics_;
  YangVersion version_ = YangVersion::k1;
};

}  // namespace yang
#endif  // YANG_VALIDATOR_H_
