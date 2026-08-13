// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_LEXER_H_
#define YANG_LEXER_H_

#include <memory>

#include "yang/token.h"

namespace yang {
/** Streaming lexer for the RFC 7950 statement grammar. */
class Lexer {
 public:
  Lexer(std::shared_ptr<const SourceFile> source, DiagnosticSink& diagnostics);
  [[nodiscard]] Token Next();

 private:
  void SkipTrivia();
  void Advance(bool inside_quoted_string = false);
  [[nodiscard]] Token MakeToken(TokenKind kind, SourceLocation begin) const;
  [[nodiscard]] Token LexQuoted(char quote, TokenKind kind);
  [[nodiscard]] Token LexUnquoted();
  [[nodiscard]] bool AtEnd() const noexcept;
  [[nodiscard]] char Current() const noexcept;
  [[nodiscard]] char Peek() const noexcept;

  std::shared_ptr<const SourceFile> source_;
  DiagnosticSink& diagnostics_;
  SourceLocation location_;
};
}  // namespace yang
#endif  // YANG_LEXER_H_
