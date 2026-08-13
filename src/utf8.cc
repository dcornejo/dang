// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/utf8.h"

#include <cstdint>

namespace yang::utf8 {
namespace {
[[nodiscard]] bool Continuation(std::uint8_t value) { return (value & 0xc0U) == 0x80U; }
[[nodiscard]] std::uint8_t Byte(char value) { return static_cast<std::uint8_t>(static_cast<unsigned char>(value)); }
}  // namespace

std::optional<DecodeResult> Decode(std::string_view input) noexcept {
  if (input.empty()) return std::nullopt;
  const auto b0 = Byte(input[0]);
  if (b0 <= 0x7fU) return DecodeResult{static_cast<char32_t>(b0), 1};
  if (b0 >= 0xc2U && b0 <= 0xdfU) {
    if (input.size() < 2) return std::nullopt;
    const auto b1 = Byte(input[1]);
    if (!Continuation(b1)) return std::nullopt;
    return DecodeResult{static_cast<char32_t>(((b0 & 0x1fU) << 6U) | (b1 & 0x3fU)), 2};
  }
  if (b0 >= 0xe0U && b0 <= 0xefU) {
    if (input.size() < 3) return std::nullopt;
    const auto b1 = Byte(input[1]); const auto b2 = Byte(input[2]);
    if (!Continuation(b1) || !Continuation(b2) || (b0 == 0xe0U && b1 < 0xa0U) ||
        (b0 == 0xedU && b1 >= 0xa0U)) return std::nullopt;
    return DecodeResult{static_cast<char32_t>(((b0 & 0x0fU) << 12U) |
        ((b1 & 0x3fU) << 6U) | (b2 & 0x3fU)), 3};
  }
  if (b0 >= 0xf0U && b0 <= 0xf4U) {
    if (input.size() < 4) return std::nullopt;
    const auto b1 = Byte(input[1]); const auto b2 = Byte(input[2]); const auto b3 = Byte(input[3]);
    if (!Continuation(b1) || !Continuation(b2) || !Continuation(b3) ||
        (b0 == 0xf0U && b1 < 0x90U) || (b0 == 0xf4U && b1 > 0x8fU)) return std::nullopt;
    return DecodeResult{static_cast<char32_t>(((b0 & 0x07U) << 18U) |
        ((b1 & 0x3fU) << 12U) | ((b2 & 0x3fU) << 6U) | (b3 & 0x3fU)), 4};
  }
  return std::nullopt;
}

bool IsNoncharacter(char32_t cp) noexcept {
  return (cp >= 0xfdd0 && cp <= 0xfdef) ||
         (cp <= 0x10ffff && (cp & 0xffff) >= 0xfffe);
}
bool IsYangCharacter(char32_t cp) noexcept {
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || IsNoncharacter(cp)) return false;
  return cp >= 0x20 || cp == U'\t' || cp == U'\n' || cp == U'\r';
}
}  // namespace yang::utf8
