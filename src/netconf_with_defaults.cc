// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_with_defaults.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <ranges>
#include <sstream>

#include <pugixml.hpp>

namespace yang::netconf {
namespace {

constexpr std::string_view kNetconfNamespace =
    "urn:ietf:params:xml:ns:netconf:base:1.0";
constexpr std::string_view kWithDefaultsNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults";

bool IsSchemaDefault(const config::RuntimeSchemaNode& schema,
                     std::string_view value) {
  if (schema.default_value == value) return true;
  return std::ranges::find(schema.default_values, value) !=
         schema.default_values.end();
}

std::string Print(const pugi::xml_document& document) {
  std::ostringstream output;
  document.print(output, "  ", pugi::format_default, pugi::encoding_utf8);
  return output.str();
}

}  // namespace

std::optional<WithDefaultsMode> ParseWithDefaultsMode(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  if (value == "report-all") return WithDefaultsMode::kReportAll;
  if (value == "report-all-tagged") return WithDefaultsMode::kReportAllTagged;
  if (value == "trim") return WithDefaultsMode::kTrim;
  if (value == "explicit") return WithDefaultsMode::kExplicit;
  return std::nullopt;
}

std::string SerializeWithDefaults(const config::RuntimeSchema& schema,
                                  const config::ConfigDocument& document,
                                  WithDefaultsMode mode) {
  pugi::xml_document xml;
  pugi::xml_node data = xml.append_child("data");
  data.append_attribute("xmlns") = kNetconfNamespace.data();
  if (mode == WithDefaultsMode::kReportAllTagged) {
    data.append_attribute("xmlns:wd") = kWithDefaultsNamespace.data();
  }

  const auto append_element = [&](pugi::xml_node parent,
                                  const config::RuntimeSchemaNode& schema_node,
                                  std::string_view inherited_namespace) {
    pugi::xml_node element =
        parent.append_child(schema_node.name.local_name.c_str());
    if (schema_node.name.namespace_uri != inherited_namespace) {
      element.append_attribute("xmlns") = schema_node.name.namespace_uri.c_str();
    }
    return element;
  };

  if (mode == WithDefaultsMode::kExplicit ||
      mode == WithDefaultsMode::kTrim) {
    std::function<void(config::ConfigNodeId, pugi::xml_node, std::string_view)>
        append;
    append = [&](config::ConfigNodeId id, pugi::xml_node parent,
                 std::string_view inherited_namespace) {
      const config::ConfigNode& node = document.Get(id);
      const config::RuntimeSchemaNode& schema_node = schema.Get(node.schema);
      if (mode == WithDefaultsMode::kTrim && node.value &&
          IsSchemaDefault(schema_node, *node.value)) {
        return;
      }
      pugi::xml_node element =
          append_element(parent, schema_node, inherited_namespace);
      if (node.value) element.text().set(node.value->c_str());
      for (config::ConfigNodeId child : node.children) {
        append(child, element, schema_node.name.namespace_uri);
      }
      if (mode == WithDefaultsMode::kTrim && !node.value &&
          schema_node.kind == semantic::SchemaNodeKind::kContainer &&
          !schema_node.presence_container && element.first_child().empty()) {
        parent.remove_child(element);
      }
    };
    for (config::ConfigNodeId root : document.roots()) {
      append(root, data, kNetconfNamespace);
    }
    return Print(xml);
  }

  const config::EffectiveDataView effective =
      config::EffectiveDataView::Build(schema, document);
  std::function<void(config::EffectiveNodeId, pugi::xml_node,
                     std::string_view)>
      append;
  append = [&](config::EffectiveNodeId id, pugi::xml_node parent,
               std::string_view inherited_namespace) {
    const config::EffectiveNode& node = effective.Get(id);
    const config::RuntimeSchemaNode& schema_node = schema.Get(node.schema);
    pugi::xml_node element =
        append_element(parent, schema_node, inherited_namespace);
    if (node.value) element.text().set(node.value->c_str());
    if (mode == WithDefaultsMode::kReportAllTagged && !node.explicit_node &&
        node.value && IsSchemaDefault(schema_node, *node.value)) {
      element.append_attribute("wd:default") = "true";
    }
    for (config::EffectiveNodeId child : node.children) {
      append(child, element, schema_node.name.namespace_uri);
    }
  };
  for (config::EffectiveNodeId root : effective.roots()) {
    append(root, data, kNetconfNamespace);
  }
  return Print(xml);
}

}  // namespace yang::netconf
