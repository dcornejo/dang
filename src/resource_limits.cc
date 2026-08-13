// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/resource_limits.h"

#include <utility>
#include <vector>

namespace yang {

const ResourceLimits& DefaultResourceLimits() noexcept {
  static const ResourceLimits limits;
  return limits;
}

bool XmlWithinResourceLimits(const pugi::xml_document& document,
                             std::string_view source,
                             const ResourceLimits& limits,
                             std::string* error) {
  if (source.size() > limits.maximum_xml_bytes) {
    *error = "XML input exceeds the byte limit";
    return false;
  }
  std::vector<std::pair<pugi::xml_node, std::size_t>> pending;
  if (const pugi::xml_node root = document.document_element()) {
    pending.emplace_back(root, 1);
  }
  std::size_t nodes = 0;
  while (!pending.empty()) {
    const auto [node, depth] = pending.back();
    pending.pop_back();
    if (++nodes > limits.maximum_xml_nodes) {
      *error = "XML input exceeds the element-count limit";
      return false;
    }
    if (depth > limits.maximum_xml_depth) {
      *error = "XML input exceeds the nesting-depth limit";
      return false;
    }
    for (pugi::xml_node child = node.first_child(); child;
         child = child.next_sibling()) {
      if (child.type() == pugi::node_element) {
        pending.emplace_back(child, depth + 1);
      }
    }
  }
  return true;
}

bool XPathWithinResourceLimits(std::string_view expression,
                               const ResourceLimits& limits,
                               std::string* error) {
  if (expression.size() > limits.maximum_xpath_bytes) {
    *error = "XPath expression exceeds the byte limit";
    return false;
  }
  std::size_t steps = expression.empty() ? 0 : 1;
  bool quoted = false;
  char quote = '\0';
  for (const char character : expression) {
    if (quoted) {
      if (character == quote) quoted = false;
      continue;
    }
    if (character == '\'' || character == '"') {
      quoted = true;
      quote = character;
    } else if (character == '/') {
      ++steps;
      if (steps > limits.maximum_xpath_steps) {
        *error = "XPath expression exceeds the location-step limit";
        return false;
      }
    }
  }
  return true;
}

}  // namespace yang
