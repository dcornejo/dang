// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_filter.h"

#include "yang/resource_limits.h"
#include "yang/xml_security.h"

#include <cctype>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <pugixml.hpp>

namespace yang::netconf {
namespace {

std::pair<std::string_view, std::string_view> SplitName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos
      ? std::pair(std::string_view(), name)
      : std::pair(name.substr(0, colon), name.substr(colon + 1));
}

std::string_view LocalName(std::string_view name) {
  return SplitName(name).second;
}

std::optional<std::string> NamespaceFor(const pugi::xml_node& node,
                                        std::string_view prefix) {
  const std::string attribute_name = prefix.empty()
      ? "xmlns" : "xmlns:" + std::string(prefix);
  for (pugi::xml_node current = node; current; current = current.parent()) {
    if (const pugi::xml_attribute attribute =
            current.attribute(attribute_name.c_str())) {
      return std::string(attribute.value());
    }
  }
  return prefix.empty() ? std::optional<std::string>("") : std::nullopt;
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

bool HasElementChildren(const pugi::xml_node& node) {
  for (const pugi::xml_node child : node.children())
    if (child.type() == pugi::node_element) return true;
  return false;
}

bool IsContentMatch(const pugi::xml_node& node) {
  return !HasElementChildren(node) && !Trim(node.text().as_string()).empty();
}

bool NameMatches(const pugi::xml_node& data, const pugi::xml_node& filter) {
  const auto [data_prefix, data_local] = SplitName(data.name());
  const auto [filter_prefix, filter_local] = SplitName(filter.name());
  if (data_local != filter_local) return false;
  const auto filter_namespace = NamespaceFor(filter, filter_prefix);
  if (!filter_namespace || filter_namespace->empty()) return true;
  return NamespaceFor(data, data_prefix) == filter_namespace;
}

bool AttributesMatch(const pugi::xml_node& data,
                     const pugi::xml_node& filter) {
  for (const pugi::xml_attribute expected : filter.attributes()) {
    const std::string_view expected_name = expected.name();
    if (expected_name == "xmlns" || expected_name.starts_with("xmlns:") ||
        expected_name == "type" || expected_name == "select") continue;
    const auto [expected_prefix, expected_local] = SplitName(expected_name);
    const auto expected_namespace = NamespaceFor(filter, expected_prefix);
    bool matched = false;
    for (const pugi::xml_attribute actual : data.attributes()) {
      const std::string_view actual_name = actual.name();
      if (actual_name == "xmlns" || actual_name.starts_with("xmlns:")) continue;
      const auto [actual_prefix, actual_local] = SplitName(actual_name);
      if (actual_local == expected_local && actual.value() == expected.value() &&
          NamespaceFor(data, actual_prefix) == expected_namespace) {
        matched = true;
        break;
      }
    }
    if (!matched) return false;
  }
  return true;
}

void CopyShell(const pugi::xml_node& source, pugi::xml_node target) {
  target.set_name(source.name());
  for (const pugi::xml_attribute attribute : source.attributes())
    target.append_attribute(attribute.name()).set_value(attribute.value());
}

void RenameForXPath(
    pugi::xml_node node,
    const std::unordered_map<std::string, std::string>& prefixes) {
  for (pugi::xml_node child : node.children()) {
    if (child.type() != pugi::node_element) continue;
    const auto [prefix, local] = SplitName(child.name());
    const std::string namespace_uri = NamespaceFor(child, prefix).value_or("");
    const auto found = prefixes.find(namespace_uri);
    if (found != prefixes.end() && !found->second.empty()) {
      child.set_name((found->second + ":" + std::string(local)).c_str());
    } else if (namespace_uri.empty()) {
      child.set_name(std::string(local).c_str());
    }
    RenameForXPath(child, prefixes);
  }
}

bool ContainsSelected(const pugi::xml_node& node,
                      const std::vector<pugi::xml_node>& selected) {
  for (const pugi::xml_node candidate : selected)
    if (candidate == node) return true;
  for (const pugi::xml_node child : node.children())
    if (child.type() == pugi::node_element && ContainsSelected(child, selected))
      return true;
  return false;
}

void CopyXPathSelection(const pugi::xml_node& source,
                        const std::vector<pugi::xml_node>& selected,
                        pugi::xml_node output_parent) {
  if (!ContainsSelected(source, selected)) return;
  if (std::ranges::find(selected, source) != selected.end()) {
    output_parent.append_copy(source);
    return;
  }
  pugi::xml_node output = output_parent.append_child(source.name());
  CopyShell(source, output);
  for (const pugi::xml_node child : source.children()) {
    if (child.type() == pugi::node_element)
      CopyXPathSelection(child, selected, output);
  }
}

bool ApplyNode(const pugi::xml_node& data, const pugi::xml_node& filter,
               pugi::xml_node output_parent) {
  if (!NameMatches(data, filter) || !AttributesMatch(data, filter)) return false;
  bool has_content = false;
  bool has_non_content = false;
  for (const pugi::xml_node filter_child : filter.children()) {
    if (filter_child.type() != pugi::node_element) continue;
    if (IsContentMatch(filter_child)) {
      has_content = true;
      bool matched = false;
      for (const pugi::xml_node data_child : data.children()) {
        if (data_child.type() == pugi::node_element &&
            NameMatches(data_child, filter_child) &&
            AttributesMatch(data_child, filter_child) &&
            Trim(data_child.text().as_string()) ==
                Trim(filter_child.text().as_string())) {
          matched = true;
          break;
        }
      }
      if (!matched) return false;
    } else {
      has_non_content = true;
    }
  }
  if (!HasElementChildren(filter) || (has_content && !has_non_content)) {
    output_parent.append_copy(data);
    return true;
  }
  pugi::xml_node output = output_parent.append_child(data.name());
  CopyShell(data, output);
  bool selected = false;
  for (const pugi::xml_node filter_child : filter.children()) {
    if (filter_child.type() != pugi::node_element) continue;
    if (IsContentMatch(filter_child)) {
      for (const pugi::xml_node data_child : data.children()) {
        if (data_child.type() == pugi::node_element &&
            NameMatches(data_child, filter_child) &&
            Trim(data_child.text().as_string()) ==
                Trim(filter_child.text().as_string())) {
          output.append_copy(data_child);
          selected = true;
          break;
        }
      }
      continue;
    }
    for (const pugi::xml_node data_child : data.children()) {
      if (data_child.type() == pugi::node_element &&
          ApplyNode(data_child, filter_child, output)) selected = true;
    }
  }
  if (!selected) {
    output_parent.remove_child(output);
    return false;
  }
  return true;
}

}  // namespace

FilterResult ApplySubtreeFilter(std::string_view data_xml,
                                std::string_view filter_xml) {
  const ResourceLimits& limits = DefaultResourceLimits();
  pugi::xml_document data_document;
  pugi::xml_document filter_document;
  const UntrustedXmlResult data_parsed =
      ParseUntrustedXml(data_xml, &data_document, {}, limits);
  if (!data_parsed.ok)
    return {std::nullopt, data_parsed.message,
            data_parsed.resource_limit ? "too-big" : "operation-failed"};
  const UntrustedXmlResult filter_parsed =
      ParseUntrustedXml(filter_xml, &filter_document, {}, limits);
  if (!filter_parsed.ok)
    return {std::nullopt, filter_parsed.message,
            filter_parsed.resource_limit ? "too-big" : "invalid-value"};
  const pugi::xml_node data = data_document.document_element();
  const pugi::xml_node filter = filter_document.document_element();
  if (std::string_view(SplitName(filter.name()).second) != "filter")
    return {std::nullopt, "subtree filter root must be filter", "invalid-value"};
  const std::string_view type = filter.attribute("type").value();
  if (!type.empty() && type != "subtree")
    return {std::nullopt, "only subtree filters are supported",
            "operation-not-supported"};

  pugi::xml_document output_document;
  pugi::xml_node output = output_document.append_child(data.name());
  CopyShell(data, output);
  for (const pugi::xml_node filter_child : filter.children()) {
    if (filter_child.type() != pugi::node_element) continue;
    for (const pugi::xml_node data_child : data.children()) {
      if (data_child.type() == pugi::node_element)
        ApplyNode(data_child, filter_child, output);
    }
  }
  std::ostringstream serialized;
  output_document.print(serialized, "", pugi::format_raw);
  return {serialized.str(), std::nullopt, std::nullopt};
}

FilterResult ApplyXPathFilter(std::string_view data_xml,
                              std::string_view filter_xml) {
  const ResourceLimits& limits = DefaultResourceLimits();
  pugi::xml_document data_document;
  pugi::xml_document filter_document;
  const UntrustedXmlResult data_parsed =
      ParseUntrustedXml(data_xml, &data_document, {}, limits);
  if (!data_parsed.ok)
    return {std::nullopt, data_parsed.message,
            data_parsed.resource_limit ? "too-big" : "operation-failed"};
  const UntrustedXmlResult filter_parsed =
      ParseUntrustedXml(filter_xml, &filter_document, {}, limits);
  if (!filter_parsed.ok)
    return {std::nullopt, filter_parsed.message,
            filter_parsed.resource_limit ? "too-big" : "invalid-value"};
  const pugi::xml_node filter = filter_document.document_element();
  if (LocalName(filter.name()) != "filter" ||
      std::string_view(filter.attribute("type").value()) != "xpath") {
    return {std::nullopt, "XPath filter requires type=\"xpath\"",
            "invalid-value"};
  }
  const pugi::xml_attribute select_attribute = filter.attribute("select");
  if (!select_attribute || Trim(select_attribute.value()).empty()) {
    return {std::nullopt, "XPath filter requires a select attribute",
            "missing-attribute"};
  }
  std::string resource_error;
  if (!XPathWithinResourceLimits(select_attribute.value(), limits,
                                 &resource_error)) {
    return {std::nullopt, resource_error, "too-big"};
  }
  if (HasElementChildren(filter) || !Trim(filter.text().as_string()).empty()) {
    return {std::nullopt, "XPath filter element must be empty", "invalid-value"};
  }

  std::unordered_map<std::string, std::string> prefixes;
  for (const pugi::xml_attribute attribute : filter.attributes()) {
    const std::string_view name = attribute.name();
    if (name.starts_with("xmlns:") && name.size() > 6)
      prefixes.try_emplace(attribute.value(), std::string(name.substr(6)));
  }
  pugi::xml_document xpath_document;
  const pugi::xml_node data = data_document.document_element();
  for (const pugi::xml_node child : data.children())
    if (child.type() == pugi::node_element) xpath_document.append_copy(child);
  RenameForXPath(xpath_document, prefixes);

  std::vector<pugi::xml_node> selected;
  try {
    const pugi::xpath_query query(select_attribute.value());
    if (query.return_type() != pugi::xpath_type_node_set) {
      return {std::nullopt, "XPath select expression must return a node-set",
              "invalid-value"};
    }
    const pugi::xpath_node_set nodes = query.evaluate_node_set(xpath_document);
    for (const pugi::xpath_node& match : nodes) {
      if (!match.node() || match.node().type() != pugi::node_element) {
        return {std::nullopt,
                "XPath result contains a node outside the data model",
                "invalid-value"};
      }
      selected.push_back(match.node());
    }
  } catch (const pugi::xpath_exception& error) {
    return {std::nullopt, std::string("invalid XPath expression: ") + error.what(),
            "invalid-value"};
  }

  pugi::xml_document output_document;
  pugi::xml_node output = output_document.append_child(data.name());
  CopyShell(data, output);
  for (const auto& [namespace_uri, prefix] : prefixes) {
    const std::string name = "xmlns:" + prefix;
    if (!output.attribute(name.c_str()))
      output.append_attribute(name.c_str()).set_value(namespace_uri.c_str());
  }
  for (const pugi::xml_node child : xpath_document.children())
    if (child.type() == pugi::node_element)
      CopyXPathSelection(child, selected, output);
  std::ostringstream serialized;
  output_document.print(serialized, "", pugi::format_raw);
  return {serialized.str(), std::nullopt, std::nullopt};
}

}  // namespace yang::netconf
