// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_STRING_DECODER_H_
#define YANG_STRING_DECODER_H_

#include <optional>
#include <string>

#include "yang/token.h"

namespace yang {
/** Decodes quoted token values according to RFC 7950 section 6.1.3. */
class StringDecoder {
 public:
  explicit StringDecoder(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<std::string> Decode(const Token& token);

 private:
  DiagnosticSink& diagnostics_;
};
}  // namespace yang
#endif  // YANG_STRING_DECODER_H_
