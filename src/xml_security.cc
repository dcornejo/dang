// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/xml_security.h"

#include "yang/utf8.h"

#include <algorithm>
#include <utility>

namespace yang {
namespace {

bool IsXmlCharacter(char32_t value) {
  return value == U'\t' || value == U'\n' || value == U'\r' ||
      (value >= 0x20 && value <= 0xd7ff) ||
      (value >= 0xe000 && value <= 0xfffd) ||
      (value >= 0x10000 && value <= 0x10ffff);
}

std::string ValidateCharacters(std::string_view xml) {
  std::size_t offset = 0;
  while (offset < xml.size()) {
    const auto decoded = utf8::Decode(xml.substr(offset));
    if (!decoded)
      return "XML is not well-formed UTF-8 at byte " + std::to_string(offset);
    if (!IsXmlCharacter(decoded->code_point))
      return "XML contains a forbidden character at byte " +
          std::to_string(offset);
    offset += decoded->byte_count;
  }
  return {};
}

}  // namespace

UntrustedXmlResult ParseUntrustedXml(std::string_view xml,
                                     pugi::xml_document* document,
                                     const UntrustedXmlPolicy& policy,
                                     const ResourceLimits& limits) {
  if (!document) return {false, false, "XML output document is null"};
  document->reset();
  if (xml.size() > limits.maximum_xml_bytes)
    return {false, true, "XML exceeds the byte limit"};
  if (const std::string invalid = ValidateCharacters(xml); !invalid.empty())
    return {false, false, invalid};
  if (xml.find("<!DOCTYPE") != std::string_view::npos ||
      xml.find("<!ENTITY") != std::string_view::npos)
    return {false, false, "DTD and entity declarations are forbidden"};
  const pugi::xml_parse_result parsed = document->load_buffer(
      xml.data(), xml.size(), pugi::parse_default, pugi::encoding_utf8);
  if (!parsed)
    return {false, false,
            std::string("malformed XML: ") + parsed.description()};
  if (policy.require_single_document_element) {
    const std::size_t roots = static_cast<std::size_t>(std::ranges::count_if(
        document->children(), [](pugi::xml_node node) {
          return node.type() == pugi::node_element;
        }));
    if (roots != 1)
      return {false, false, "XML requires exactly one document element"};
  }
  std::string resource_error;
  if (!XmlWithinResourceLimits(*document, xml, limits, &resource_error))
    return {false, true, std::move(resource_error)};
  return {true, false, {}};
}

}  // namespace yang
