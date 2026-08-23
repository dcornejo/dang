// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/platform_config.h"

#include <charconv>

#include <pugixml.hpp>

namespace dangd::ip_management {
namespace {

std::string_view LocalName(std::string_view name) {
  const auto colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node Child(const pugi::xml_node parent, std::string_view name) {
  for (const pugi::xml_node child : parent.children())
    if (LocalName(child.name()) == name) return child;
  return {};
}

void ReadAddresses(const pugi::xml_node family, bool ipv6,
                   std::vector<AddressConfig>* result) {
  for (const pugi::xml_node entry : family.children()) {
    if (LocalName(entry.name()) != "address") continue;
    const pugi::xml_node ip = Child(entry, "ip");
    const pugi::xml_node prefix = Child(entry, "prefix-length");
    if (!ip || !prefix) continue;
    unsigned length = 0;
    const std::string text = prefix.text().as_string();
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), length);
    if (parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
      result->push_back({ip.text().as_string(), length, ipv6});
  }
}

void ReadInterfaces(const pugi::xml_node parent,
                    std::vector<InterfaceConfig>* interfaces) {
  for (const pugi::xml_node root : parent.children()) {
    if (LocalName(root.name()) != "interfaces") {
      ReadInterfaces(root, interfaces);
      continue;
    }
    for (const pugi::xml_node node : root.children()) {
      if (LocalName(node.name()) != "interface") continue;
      const pugi::xml_node name = Child(node, "name");
      if (!name) continue;
      InterfaceConfig interface{.name = name.text().as_string()};
      if (const pugi::xml_node enabled = Child(node, "enabled"))
        interface.enabled = std::string_view(enabled.text().as_string()) == "true";
      ReadAddresses(Child(node, "ipv4"), false, &interface.addresses);
      ReadAddresses(Child(node, "ipv6"), true, &interface.addresses);
      interfaces->push_back(std::move(interface));
    }
  }
}

}  // namespace

bool ParsePlatformConfig(std::string_view xml,
                         std::vector<InterfaceConfig>* interfaces,
                         std::string* error) {
  if (!interfaces) return false;
  interfaces->clear();
  pugi::xml_document document;
  const auto parsed = document.load_buffer(xml.data(), xml.size());
  if (!parsed) {
    if (error) *error = std::string("cannot parse configuration: ") + parsed.description();
    return false;
  }
  ReadInterfaces(document, interfaces);
  for (const InterfaceConfig& interface : *interfaces) {
    if (interface.name.empty() || interface.name.front() == '-') {
      if (error) *error = "interface name cannot be empty or begin with '-'";
      interfaces->clear();
      return false;
    }
  }
  return true;
}

}  // namespace dangd::ip_management
