// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_FILTER_H_
#define YANG_NETCONF_FILTER_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace yang::netconf {

struct FilterResult {
  std::optional<std::string> xml;
  std::optional<std::string> error;
  std::optional<std::string> error_tag;
};

/** Applies an RFC 6241 subtree filter to an XML data wrapper. */
[[nodiscard]] FilterResult ApplySubtreeFilter(std::string_view data_xml,
                                              std::string_view filter_xml);
/** Applies an XPath filter and optional selection-relative maximum depth. */
[[nodiscard]] FilterResult ApplyXPathFilter(std::string_view data_xml,
                                            std::string_view filter_xml,
                                            std::optional<std::uint16_t>
                                                maximum_depth = std::nullopt);

}  // namespace yang::netconf

#endif  // YANG_NETCONF_FILTER_H_
