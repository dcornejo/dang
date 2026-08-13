// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_PARSER_H_
#define YANG_PARSER_H_

#include <memory>
#include <optional>

#include "yang/lexer.h"
#include "yang/resource_limits.h"
#include "yang/syntax_tree.h"

namespace yang {
/** Recursive-descent parser with statement-boundary error recovery. */
class Parser {
 public:
  Parser(std::shared_ptr<const SourceFile> source, DiagnosticSink& diagnostics,
         const ResourceLimits& limits = DefaultResourceLimits());
  [[nodiscard]] SyntaxTree Parse();

 private:
  [[nodiscard]] std::optional<StatementId> ParseStatement(SyntaxTree& tree,
                                                           std::size_t depth);
  [[nodiscard]] std::optional<std::string> ParseArgument();
  void Advance();
  void Recover();
  [[nodiscard]] bool IsKeyword(std::string_view text) const noexcept;

  DiagnosticSink& diagnostics_;
  Lexer lexer_;
  Token current_;
  const ResourceLimits& limits_;
};
}  // namespace yang
#endif  // YANG_PARSER_H_
