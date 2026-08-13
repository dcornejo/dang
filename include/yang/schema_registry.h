// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SCHEMA_REGISTRY_H_
#define YANG_SCHEMA_REGISTRY_H_

#include <optional>
#include <span>
#include <string_view>

namespace yang {
enum class YangVersion { k1, k1_1 };
enum class ArgumentRequirement { kNone, kRequired };
struct StatementSpec {
  std::string_view keyword;
  ArgumentRequirement argument;
  std::string_view yin_argument;
  bool yin_element = false;
  YangVersion introduced = YangVersion::k1;
};
/** Immutable metadata registry for RFC 7950 built-in statements and YIN mapping. */
class SchemaRegistry {
 public:
  [[nodiscard]] static std::span<const StatementSpec> Builtins() noexcept;
  [[nodiscard]] static std::optional<StatementSpec> Find(std::string_view keyword) noexcept;
  [[nodiscard]] static bool IsAvailable(std::string_view keyword,
                                        YangVersion version) noexcept;
  /** Returns whether child is structurally permitted under parent. */
  [[nodiscard]] static bool IsAllowed(std::string_view parent,
                                      std::string_view child) noexcept;
  /** Returns whether child may occur more than once under parent. */
  [[nodiscard]] static bool IsRepeatable(std::string_view parent,
                                         std::string_view child) noexcept;
  /** Returns the RFC module/submodule section ordering rank. */
  [[nodiscard]] static int StatementOrder(std::string_view child) noexcept;
  /** Returns required direct children for a built-in statement. */
  [[nodiscard]] static std::span<const std::string_view> RequiredChildren(
      std::string_view parent) noexcept;
};
}  // namespace yang
#endif  // YANG_SCHEMA_REGISTRY_H_
