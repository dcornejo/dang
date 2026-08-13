// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/lexer.h"

#include "yang/utf8.h"

namespace yang {
Lexer::Lexer(std::shared_ptr<const SourceFile> source, DiagnosticSink& diagnostics)
    : source_(std::move(source)), diagnostics_(diagnostics) {}
bool Lexer::AtEnd() const noexcept { return location_.byte_offset >= source_->contents().size(); }
char Lexer::Current() const noexcept { return AtEnd() ? '\0' : source_->contents()[location_.byte_offset]; }
char Lexer::Peek() const noexcept {
  return location_.byte_offset + 1 >= source_->contents().size() ? '\0' : source_->contents()[location_.byte_offset + 1];
}
void Lexer::Advance(bool inside_quoted_string) {
  if (!inside_quoted_string && Current() == '\r' && Peek() != '\n') {
    const SourceLocation begin = location_;
    SourceLocation end = begin;
    ++end.byte_offset;
    ++end.column;
    diagnostics_.Report({DiagnosticCode::kInvalidLineEnding,
                         DiagnosticSeverity::kError,
                         "a carriage return outside a quoted string must be followed by a line feed",
                         {begin, end}});
  }
  const auto decoded = utf8::Decode(source_->contents().substr(location_.byte_offset));
  if (!decoded) return;
  location_.byte_offset += decoded->byte_count;
  if (decoded->code_point == U'\n') { ++location_.line; location_.column = 1; }
  else { ++location_.column; }
}
Token Lexer::MakeToken(TokenKind kind, SourceLocation begin) const {
  return {kind, source_, {begin, location_}};
}
void Lexer::SkipTrivia() {
  for (;;) {
    while (!AtEnd() && (Current() == ' ' || Current() == '\t' || Current() == '\r' || Current() == '\n')) Advance();
    if (Current() == '/' && Peek() == '/') {
      while (!AtEnd() && Current() != '\n') Advance();
      continue;
    }
    if (Current() == '/' && Peek() == '*') {
      const SourceLocation begin = location_; Advance(); Advance();
      while (!AtEnd() && !(Current() == '*' && Peek() == '/')) Advance();
      if (AtEnd()) {
        diagnostics_.Report({DiagnosticCode::kUnterminatedBlockComment, DiagnosticSeverity::kError,
                             "unterminated block comment", {begin, location_}});
        return;
      }
      Advance(); Advance(); continue;
    }
    return;
  }
}
Token Lexer::LexQuoted(char quote, TokenKind kind) {
  const SourceLocation begin = location_; Advance();
  while (!AtEnd()) {
    if (Current() == quote) { Advance(true); return MakeToken(kind, begin); }
    if (quote == '"' && Current() == '\\') {
      Advance(true);
      if (!AtEnd()) Advance(true);
    } else {
      Advance(true);
    }
  }
  diagnostics_.Report({DiagnosticCode::kUnterminatedQuotedString, DiagnosticSeverity::kError,
                       "unterminated quoted string", {begin, location_}});
  return MakeToken(TokenKind::kInvalid, begin);
}
Token Lexer::LexUnquoted() {
  const SourceLocation begin = location_;
  while (!AtEnd()) {
    const char c = Current();
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\'' || c == '"' ||
        c == '+' || c == ';' || c == '{' || c == '}' ||
        (c == '/' && (Peek() == '/' || Peek() == '*'))) break;
    Advance();
  }
  return MakeToken(TokenKind::kUnquotedString, begin);
}
Token Lexer::Next() {
  SkipTrivia();
  const SourceLocation begin = location_;
  if (AtEnd()) return MakeToken(TokenKind::kEndOfFile, begin);
  switch (Current()) {
    case '\'': return LexQuoted('\'', TokenKind::kSingleQuotedString);
    case '"': return LexQuoted('"', TokenKind::kDoubleQuotedString);
    case '+': Advance(); return MakeToken(TokenKind::kPlus, begin);
    case ';': Advance(); return MakeToken(TokenKind::kSemicolon, begin);
    case '{': Advance(); return MakeToken(TokenKind::kLeftBrace, begin);
    case '}': Advance(); return MakeToken(TokenKind::kRightBrace, begin);
    default: return LexUnquoted();
  }
}
}  // namespace yang
