// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_TOKEN_H_
#define YANG_TOKEN_H_

#include <memory>
#include <string_view>

#include "yang/source_file.h"

namespace yang {
enum class TokenKind { kEndOfFile, kUnquotedString, kSingleQuotedString,
  kDoubleQuotedString, kPlus, kSemicolon, kLeftBrace, kRightBrace, kInvalid };

/** A non-owning token whose source lifetime is retained by shared ownership. */
struct Token {
  TokenKind kind = TokenKind::kInvalid;
  std::shared_ptr<const SourceFile> source;
  SourceRange range;
  [[nodiscard]] std::string_view text() const noexcept {
    return source->contents().substr(range.begin.byte_offset,
                                     range.end.byte_offset - range.begin.byte_offset);
  }
};
}  // namespace yang
#endif  // YANG_TOKEN_H_
