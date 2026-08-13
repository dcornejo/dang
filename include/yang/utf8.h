// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_UTF8_H_
#define YANG_UTF8_H_

#include <cstddef>
#include <optional>
#include <string_view>

namespace yang::utf8 {
struct DecodeResult { char32_t code_point = U'\0'; std::size_t byte_count = 0; };
[[nodiscard]] std::optional<DecodeResult> Decode(std::string_view input) noexcept;
[[nodiscard]] bool IsNoncharacter(char32_t code_point) noexcept;
[[nodiscard]] bool IsYangCharacter(char32_t code_point) noexcept;
}  // namespace yang::utf8
#endif  // YANG_UTF8_H_
