// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_RESOURCE_LIMITS_H_
#define YANG_RESOURCE_LIMITS_H_

#include <cstddef>
#include <string>
#include <string_view>

#include <pugixml.hpp>

namespace yang {

/** Deterministic ceilings applied to untrusted input and bounded work. */
struct ResourceLimits {
  std::size_t maximum_source_bytes = 8 * 1024 * 1024;
  std::size_t maximum_statements = 1'000'000;
  std::size_t maximum_yang_depth = 256;
  std::size_t maximum_xml_bytes = 16 * 1024 * 1024;
  std::size_t maximum_xml_nodes = 1'000'000;
  std::size_t maximum_xml_depth = 256;
  std::size_t maximum_xpath_bytes = 64 * 1024;
  std::size_t maximum_xpath_steps = 1024;
  std::size_t maximum_snapshot_bytes = 64 * 1024 * 1024;
};

/** Returns the process-independent default policy. */
[[nodiscard]] const ResourceLimits& DefaultResourceLimits() noexcept;

/** Checks XML byte, element-count, and nesting-depth ceilings. */
[[nodiscard]] bool XmlWithinResourceLimits(
    const pugi::xml_document& document, std::string_view source,
    const ResourceLimits& limits, std::string* error);

/** Checks XPath byte and location-step ceilings. */
[[nodiscard]] bool XPathWithinResourceLimits(
    std::string_view expression, const ResourceLimits& limits,
    std::string* error);

}  // namespace yang

#endif  // YANG_RESOURCE_LIMITS_H_
