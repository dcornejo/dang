// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/string_decoder.h"

#include <string_view>

namespace yang {
std::optional<std::string> StringDecoder::Decode(const Token& token) {
  if (token.kind == TokenKind::kUnquotedString) return std::string(token.text());
  const std::string_view raw = token.text();
  if (raw.size() < 2) return std::nullopt;
  if (token.kind == TokenKind::kSingleQuotedString) return std::string(raw.substr(1, raw.size() - 2));
  if (token.kind != TokenKind::kDoubleQuotedString) return std::nullopt;

  const std::string_view body = raw.substr(1, raw.size() - 2);
  std::string normalized;
  std::size_t start = 0;
  bool first = true;
  while (start <= body.size()) {
    const std::size_t newline = body.find('\n', start);
    std::string_view line = newline == std::string_view::npos ? body.substr(start) : body.substr(start, newline - start);
    if (newline != std::string_view::npos) {
      while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
    }
    if (!first) {
      std::size_t trim = 0;
      const std::size_t limit = token.range.begin.column;
      while (trim < line.size() && trim < limit && (line[trim] == ' ' || line[trim] == '\t')) ++trim;
      line.remove_prefix(trim);
    }
    normalized.append(line);
    if (newline == std::string_view::npos) break;
    normalized.push_back('\n'); start = newline + 1; first = false;
  }

  std::string result;
  for (std::size_t i = 0; i < normalized.size(); ++i) {
    if (normalized[i] != '\\') { result.push_back(normalized[i]); continue; }
    if (++i >= normalized.size()) {
      diagnostics_.Report({DiagnosticCode::kInvalidEscapeSequence, DiagnosticSeverity::kError,
                           "backslash at end of double-quoted string", token.range});
      return std::nullopt;
    }
    switch (normalized[i]) {
      case 'n': result.push_back('\n'); break;
      case 't': result.push_back('\t'); break;
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      default:
        diagnostics_.Report({DiagnosticCode::kInvalidEscapeSequence, DiagnosticSeverity::kError,
                             "invalid escape sequence in double-quoted string", token.range});
        return std::nullopt;
    }
  }
  return result;
}
}  // namespace yang
