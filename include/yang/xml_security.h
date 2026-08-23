// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_XML_SECURITY_H_
#define YANG_XML_SECURITY_H_

#include "yang/resource_limits.h"

#include <string>
#include <string_view>

#include <pugixml.hpp>

namespace yang {

/** Policy for parsing XML received across an untrusted boundary. */
struct UntrustedXmlPolicy {
  bool require_single_document_element = true;
};

/** Result of strict XML parsing and resource validation. */
struct UntrustedXmlResult {
  bool ok = false;
  bool resource_limit = false;
  std::string message;
};

/**
 * Parses bounded UTF-8 XML without DTD/entity declaration processing.
 *
 * Comments, CDATA, and processing instructions are accepted as inert XML
 * syntax. DTD and entity declarations are rejected before pugixml sees them;
 * pugixml is invoked without parse_doctype or parse_declaration flags and does
 * not perform external entity resolution or XInclude processing.
 */
[[nodiscard]] UntrustedXmlResult ParseUntrustedXml(
    std::string_view xml, pugi::xml_document* document,
    const UntrustedXmlPolicy& policy = {},
    const ResourceLimits& limits = DefaultResourceLimits());

}  // namespace yang

#endif  // YANG_XML_SECURITY_H_
