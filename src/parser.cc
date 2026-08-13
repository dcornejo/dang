// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/parser.h"

#include <cctype>
#include <utility>

#include "yang/string_decoder.h"

namespace yang {
Parser::Parser(std::shared_ptr<const SourceFile> source,
               DiagnosticSink& diagnostics, const ResourceLimits& limits)
    : diagnostics_(diagnostics),
      lexer_(std::move(source), diagnostics),
      current_(lexer_.Next()),
      limits_(limits) {}
void Parser::Advance() { current_ = lexer_.Next(); }
bool Parser::IsKeyword(std::string_view text) const noexcept {
  if (text.empty()) return false;
  auto valid_start = [](unsigned char c) { return std::isalpha(c) != 0 || c == '_'; };
  auto valid_rest = [&](unsigned char c) { return valid_start(c) || std::isdigit(c) != 0 || c == '-' || c == '.'; };
  std::size_t colon = text.find(':');
  if (colon == std::string_view::npos) colon = text.size();
  auto valid_identifier = [&](std::string_view value) {
    if (value.empty() || !valid_start(static_cast<unsigned char>(value.front()))) return false;
    for (char c : value.substr(1)) if (!valid_rest(static_cast<unsigned char>(c))) return false;
    return true;
  };
  return valid_identifier(text.substr(0, colon)) &&
      (colon == text.size() || (text.find(':', colon + 1) == std::string_view::npos && valid_identifier(text.substr(colon + 1))));
}
std::optional<std::string> Parser::ParseArgument() {
  if (current_.kind != TokenKind::kUnquotedString && current_.kind != TokenKind::kSingleQuotedString &&
      current_.kind != TokenKind::kDoubleQuotedString) return std::nullopt;
  StringDecoder decoder(diagnostics_);
  const bool quoted = current_.kind != TokenKind::kUnquotedString;
  auto result = decoder.Decode(current_); Advance();
  while (current_.kind == TokenKind::kPlus) {
    const SourceRange plus_range = current_.range; Advance();
    if (!quoted || (current_.kind != TokenKind::kSingleQuotedString && current_.kind != TokenKind::kDoubleQuotedString)) {
      diagnostics_.Report({DiagnosticCode::kInvalidStringConcatenation, DiagnosticSeverity::kError,
                           "'+' may concatenate only quoted strings", plus_range});
      return result;
    }
    auto part = decoder.Decode(current_); Advance();
    if (result && part) result->append(*part); else result = std::nullopt;
  }
  return result;
}
void Parser::Recover() {
  int depth = 0;
  while (current_.kind != TokenKind::kEndOfFile) {
    if (current_.kind == TokenKind::kLeftBrace) { ++depth; Advance(); continue; }
    if (current_.kind == TokenKind::kRightBrace) {
      if (depth == 0) return;
      --depth; Advance(); if (depth == 0) return; continue;
    }
    if (current_.kind == TokenKind::kSemicolon && depth == 0) { Advance(); return; }
    Advance();
  }
}
std::optional<StatementId> Parser::ParseStatement(SyntaxTree& tree,
                                                   std::size_t depth) {
  if (depth > limits_.maximum_yang_depth) {
    diagnostics_.Report({DiagnosticCode::kResourceLimitExceeded,
                         DiagnosticSeverity::kError,
                         "YANG input exceeds the nesting-depth limit",
                         current_.range});
    Recover();
    return std::nullopt;
  }
  if (current_.kind != TokenKind::kUnquotedString || !IsKeyword(current_.text())) {
    diagnostics_.Report({DiagnosticCode::kInvalidKeyword, DiagnosticSeverity::kError,
                         "expected a YANG statement keyword", current_.range});
    Recover(); return std::nullopt;
  }
  Statement statement; statement.keyword = std::string(current_.text()); statement.range.begin = current_.range.begin;
  Advance();
  if (current_.kind != TokenKind::kSemicolon && current_.kind != TokenKind::kLeftBrace) {
    statement.argument = ParseArgument();
    if (!statement.argument) {
      diagnostics_.Report({DiagnosticCode::kMissingArgument, DiagnosticSeverity::kError,
                           "expected a statement argument", current_.range});
      Recover(); return std::nullopt;
    }
  }
  if (tree.size() >= limits_.maximum_statements) {
    diagnostics_.Report({DiagnosticCode::kResourceLimitExceeded,
                         DiagnosticSeverity::kError,
                         "YANG input exceeds the statement-count limit",
                         current_.range});
    Recover();
    return std::nullopt;
  }
  const StatementId id = tree.Add(std::move(statement));
  if (current_.kind == TokenKind::kSemicolon) {
    tree.Get(id).range.end = current_.range.end; Advance(); return id;
  }
  if (current_.kind != TokenKind::kLeftBrace) {
    diagnostics_.Report({DiagnosticCode::kMissingTerminator, DiagnosticSeverity::kError,
                         "expected ';' or '{' after statement", current_.range});
    Recover(); tree.Get(id).range.end = current_.range.end; return id;
  }
  Advance();
  while (current_.kind != TokenKind::kRightBrace && current_.kind != TokenKind::kEndOfFile) {
    if (auto child = ParseStatement(tree, depth + 1))
      tree.Get(id).children.push_back(*child);
  }
  if (current_.kind == TokenKind::kRightBrace) { tree.Get(id).range.end = current_.range.end; Advance(); }
  else {
    tree.Get(id).range.end = current_.range.end;
    diagnostics_.Report({DiagnosticCode::kMissingRightBrace, DiagnosticSeverity::kError,
                         "expected '}' before end of file", current_.range});
  }
  return id;
}
SyntaxTree Parser::Parse() {
  SyntaxTree tree(current_.source);
  while (current_.kind != TokenKind::kEndOfFile) {
    if (current_.kind == TokenKind::kRightBrace) {
      diagnostics_.Report({DiagnosticCode::kUnexpectedToken, DiagnosticSeverity::kError,
                           "unexpected '}' at top level", current_.range}); Advance(); continue;
    }
    if (auto root = ParseStatement(tree, 1)) tree.AddRoot(*root);
  }
  return tree;
}
}  // namespace yang
