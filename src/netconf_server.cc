// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_server.h"

#include "yang/netconf_filter.h"
#include "yang/resource_limits.h"

#include <charconv>
#include <cctype>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <sstream>

#include <pugixml.hpp>

namespace yang::netconf {
namespace {

constexpr std::string_view kNetconfNamespace =
    "urn:ietf:params:xml:ns:netconf:base:1.0";
constexpr std::string_view kNotificationNamespace =
    "urn:ietf:params:xml:ns:netconf:notification:1.0";
constexpr std::string_view kYangActionNamespace =
    "urn:ietf:params:xml:ns:yang:1";
constexpr std::string_view kMonitoringNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring";
constexpr std::string_view kNmdaNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-netconf-nmda";
constexpr std::string_view kDatastoresNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-datastores";
constexpr std::string_view kOriginNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-origin";

std::string_view LocalName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::optional<std::string> NamespaceFor(const pugi::xml_node& node) {
  const std::string_view name = node.name();
  const std::size_t colon = name.find(':');
  const std::string attribute_name = colon == std::string_view::npos
      ? "xmlns" : "xmlns:" + std::string(name.substr(0, colon));
  for (pugi::xml_node current = node; current; current = current.parent()) {
    if (const pugi::xml_attribute attribute =
            current.attribute(attribute_name.c_str())) {
      return std::string(attribute.value());
    }
  }
  return std::nullopt;
}

std::string Escape(std::string_view value) {
  std::string result;
  for (char character : value) {
    if (character == '&') result += "&amp;";
    else if (character == '<') result += "&lt;";
    else if (character == '>') result += "&gt;";
    else if (character == '"') result += "&quot;";
    else if (character == '\'') result += "&apos;";
    else result += character;
  }
  return result;
}

std::optional<Datastore> ParseDatastore(const pugi::xml_node& parent) {
  const pugi::xml_node selection = parent.first_child();
  if (!selection || selection.next_sibling()) return std::nullopt;
  const std::string_view name = LocalName(selection.name());
  if (name == "running") return Datastore::kRunning;
  if (name == "candidate") return Datastore::kCandidate;
  if (name == "startup") return Datastore::kStartup;
  return std::nullopt;
}

std::optional<Datastore> ParseNmdaDatastore(const pugi::xml_node& node) {
  if (!node) return std::nullopt;
  std::string_view value = node.text().as_string();
  while (!value.empty() && std::isspace(
             static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
  while (!value.empty() && std::isspace(
             static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
  const std::size_t colon = value.find(':');
  const std::string_view prefix = colon == std::string_view::npos
      ? std::string_view() : value.substr(0, colon);
  const std::string_view local = colon == std::string_view::npos
      ? value : value.substr(colon + 1);
  const std::string attribute = prefix.empty()
      ? "xmlns" : "xmlns:" + std::string(prefix);
  std::optional<std::string_view> namespace_uri;
  for (pugi::xml_node current = node; current; current = current.parent()) {
    if (const pugi::xml_attribute declaration =
            current.attribute(attribute.c_str())) {
      namespace_uri = declaration.value();
      break;
    }
  }
  if (!namespace_uri || *namespace_uri != kDatastoresNamespace)
    return std::nullopt;
  if (local == "running") return Datastore::kRunning;
  if (local == "candidate") return Datastore::kCandidate;
  if (local == "startup") return Datastore::kStartup;
  if (local == "intended") return Datastore::kIntended;
  if (local == "operational") return Datastore::kOperational;
  return std::nullopt;
}

std::string ApplyMaximumDepth(std::string_view xml, unsigned int maximum) {
  pugi::xml_document document;
  if (!document.load_buffer(xml.data(), xml.size())) return {};
  std::function<void(pugi::xml_node, unsigned int)> prune;
  prune = [&](pugi::xml_node parent, unsigned int depth) {
    for (pugi::xml_node child = parent.first_child(); child;) {
      pugi::xml_node next = child.next_sibling();
      if (child.type() == pugi::node_element) {
        if (depth >= maximum) {
          for (pugi::xml_node descendant = child.first_child(); descendant;) {
            pugi::xml_node after = descendant.next_sibling();
            if (descendant.type() == pugi::node_element)
              child.remove_child(descendant);
            descendant = after;
          }
        } else {
          prune(child, depth + 1);
        }
      }
      child = next;
    }
  };
  prune(document.document_element(), 1);
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

std::string FilterConfigKind(std::string_view xml,
                             const config::RuntimeSchema& schema,
                             bool want_config) {
  pugi::xml_document document;
  if (!document.load_buffer(xml.data(), xml.size())) return {};
  std::function<bool(pugi::xml_node, config::RuntimeSchemaNodeId, bool)> visit;
  visit = [&](pugi::xml_node node, config::RuntimeSchemaNodeId schema_id,
              bool list_key) {
    const config::RuntimeSchemaNode& metadata = schema.Get(schema_id);
    bool has_selected_descendant = false;
    for (pugi::xml_node child = node.first_child(); child;) {
      pugi::xml_node next = child.next_sibling();
      if (child.type() == pugi::node_element) {
        const auto child_schema = schema.FindChild(
            schema_id, {NamespaceFor(child).value_or(""),
                        std::string(LocalName(child.name()))});
        bool selected = false;
        if (child_schema) {
          const bool child_is_key =
              std::ranges::find(metadata.keys, *child_schema) !=
              metadata.keys.end();
          selected = visit(child, *child_schema, child_is_key);
        }
        if (!selected) node.remove_child(child);
        has_selected_descendant = has_selected_descendant || selected;
      }
      child = next;
    }
    return metadata.config == want_config ||
           (!want_config && list_key) || has_selected_descendant;
  };
  pugi::xml_node root = document.document_element();
  for (pugi::xml_node child = root.first_child(); child;) {
    pugi::xml_node next = child.next_sibling();
    bool selected = false;
    if (child.type() == pugi::node_element) {
      const auto schema_id = schema.FindRoot(
          {NamespaceFor(child).value_or(""),
           std::string(LocalName(child.name()))});
      if (schema_id) selected = visit(child, *schema_id, false);
    }
    if (child.type() == pugi::node_element && !selected)
      root.remove_child(child);
    child = next;
  }
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

std::string AnnotateIntendedOrigin(std::string_view xml,
                                   const config::RuntimeSchema& schema,
                                   bool include_annotations) {
  pugi::xml_document document;
  if (!document.load_buffer(xml.data(), xml.size())) return {};
  pugi::xml_node root = document.document_element();
  if (include_annotations)
    root.append_attribute("xmlns:or") = kOriginNamespace.data();
  for (pugi::xml_node child : root.children()) {
    if (child.type() != pugi::node_element) continue;
    const auto schema_id = schema.FindRoot(
        {NamespaceFor(child).value_or(""),
         std::string(LocalName(child.name()))});
    if (!schema_id || !schema.Get(*schema_id).config) continue;
    if (include_annotations)
      child.append_attribute("or:origin") = "or:intended";
  }
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

bool OriginValueIsIntended(const pugi::xml_node& node) {
  const std::string_view value = node.text().as_string();
  const std::size_t colon = value.find(':');
  return (colon == std::string_view::npos ? value : value.substr(colon + 1)) ==
         "intended";
}

std::optional<config::EditOperation> ParseDefaultOperation(
    std::string_view value) {
  if (value.empty() || value == "merge") return config::EditOperation::kMerge;
  if (value == "replace") return config::EditOperation::kReplace;
  if (value == "none") return config::EditOperation::kNone;
  return std::nullopt;
}

std::optional<TestOption> ParseTestOption(std::string_view value) {
  if (value.empty() || value == "test-then-set") return TestOption::kTestThenSet;
  if (value == "set") return TestOption::kSet;
  if (value == "test-only") return TestOption::kTestOnly;
  return std::nullopt;
}

std::optional<ErrorOption> ParseErrorOption(std::string_view value) {
  if (value.empty() || value == "stop-on-error") return ErrorOption::kStopOnError;
  if (value == "continue-on-error") return ErrorOption::kContinueOnError;
  if (value == "rollback-on-error") return ErrorOption::kRollbackOnError;
  return std::nullopt;
}

pugi::xml_node Child(const pugi::xml_node& parent, std::string_view local) {
  for (const pugi::xml_node child : parent.children()) {
    if (child.type() == pugi::node_element && LocalName(child.name()) == local)
      return child;
  }
  return {};
}

std::string Serialize(const pugi::xml_node& node) {
  std::ostringstream stream;
  node.print(stream, "", pugi::format_raw);
  return stream.str();
}

std::string SerializeConfig(const pugi::xml_node& config) {
  std::string result = "<config xmlns=\"" + std::string(kNetconfNamespace) +
                       "\">";
  for (const pugi::xml_node child : config.children()) {
    if (child.type() == pugi::node_element) result += Serialize(child);
  }
  return result + "</config>";
}

std::string SerializeSelfContained(const pugi::xml_node& node) {
  pugi::xml_document document;
  pugi::xml_node copy = document.append_copy(node);
  for (pugi::xml_node ancestor = node.parent(); ancestor;
       ancestor = ancestor.parent()) {
    for (const pugi::xml_attribute attribute : ancestor.attributes()) {
      const std::string_view name = attribute.name();
      if ((name == "xmlns" || name.starts_with("xmlns:")) &&
          !copy.attribute(attribute.name())) {
        copy.append_attribute(attribute.name()).set_value(attribute.value());
      }
    }
  }
  return Serialize(copy);
}

std::string SchemaInstanceComponent(
    const pugi::xml_node& instance,
    const config::RuntimeSchemaNode& metadata,
    const config::RuntimeSchema& schema) {
  std::string result = "/{" + metadata.name.namespace_uri + "}" +
                       metadata.name.local_name;
  for (config::RuntimeSchemaNodeId key_id : metadata.keys) {
    const config::RuntimeSchemaNode& key = schema.Get(key_id);
    for (const pugi::xml_node child : instance.children()) {
      if (child.type() != pugi::node_element ||
          LocalName(child.name()) != key.name.local_name ||
          NamespaceFor(child).value_or("") != key.name.namespace_uri) continue;
      const std::string value = child.text().as_string();
      if (value.find('\'') == std::string::npos) {
        result += "[{" + key.name.namespace_uri + "}" + key.name.local_name +
                  "='" + value + "']";
      } else if (value.find('"') == std::string::npos) {
        result += "[{" + key.name.namespace_uri + "}" + key.name.local_name +
                  "=\"" + value + "\"]";
      }
      break;
    }
  }
  return result;
}

struct ActionInstance {
  struct AncestorSelector {
    config::QualifiedXmlName name;
    std::vector<std::pair<config::QualifiedXmlName, std::string>> keys;
  };
  config::RuntimeSchemaNodeId schema = config::kInvalidRuntimeSchemaNodeId;
  std::string path;
  std::vector<NacmDataNode> ancestors;
  std::vector<AncestorSelector> selectors;
  bool has_all_keys = true;
  bool has_representable_keys = true;
  pugi::xml_node xml;
};

std::optional<ActionInstance> FindActionInstance(
    const config::RuntimeSchema& schema, const pugi::xml_node& wrapper) {
  std::function<std::optional<ActionInstance>(
      pugi::xml_node, config::RuntimeSchemaNodeId, std::string,
      std::vector<NacmDataNode>, std::vector<ActionInstance::AncestorSelector>,
      bool, bool)> visit;
  visit = [&](pugi::xml_node instance, config::RuntimeSchemaNodeId schema_id,
              std::string parent_path, std::vector<NacmDataNode> ancestors,
              std::vector<ActionInstance::AncestorSelector> selectors,
              bool has_all_keys, bool has_representable_keys)
      -> std::optional<ActionInstance> {
    const config::RuntimeSchemaNode& metadata = schema.Get(schema_id);
    std::string path = parent_path +
        SchemaInstanceComponent(instance, metadata, schema);
    if (metadata.kind == semantic::SchemaNodeKind::kAction)
      return ActionInstance{schema_id, std::move(path),
                            std::move(ancestors), std::move(selectors),
                            has_all_keys, has_representable_keys, instance};
    ancestors.push_back({metadata.module_name, path,
                         metadata.nacm_default_deny_all});
    ActionInstance::AncestorSelector selector{metadata.name, {}};
    for (config::RuntimeSchemaNodeId key_id : metadata.keys) {
      const config::RuntimeSchemaNode& key = schema.Get(key_id);
      bool found = false;
      for (const pugi::xml_node child : instance.children()) {
        if (child.type() == pugi::node_element &&
            NamespaceFor(child).value_or("") == key.name.namespace_uri &&
            LocalName(child.name()) == key.name.local_name) {
          selector.keys.emplace_back(key.name, child.text().as_string());
          const std::string_view value = child.text().as_string();
          if (value.find('\'') != std::string_view::npos &&
              value.find('"') != std::string_view::npos)
            has_representable_keys = false;
          found = true;
          break;
        }
      }
      has_all_keys = has_all_keys && found;
    }
    selectors.push_back(std::move(selector));
    for (const pugi::xml_node child : instance.children()) {
      if (child.type() != pugi::node_element) continue;
      const config::QualifiedXmlName child_name{
          NamespaceFor(child).value_or(""),
          std::string(LocalName(child.name()))};
      auto child_schema = schema.FindChild(schema_id, child_name);
      if (!child_schema)
        child_schema = schema.FindChildOperation(
            schema_id, child_name, semantic::SchemaNodeKind::kAction);
      if (!child_schema) continue;
      const auto kind = schema.Get(*child_schema).kind;
      if (kind != semantic::SchemaNodeKind::kAction &&
          kind != semantic::SchemaNodeKind::kContainer &&
          kind != semantic::SchemaNodeKind::kList) continue;
      if (auto found = visit(child, *child_schema, path, ancestors, selectors,
                             has_all_keys, has_representable_keys)) return found;
    }
    return std::nullopt;
  };
  for (const pugi::xml_node child : wrapper.children()) {
    if (child.type() != pugi::node_element) continue;
    const auto root = schema.FindRoot(
        {NamespaceFor(child).value_or(""), std::string(LocalName(child.name()))});
    if (root) {
      if (auto found = visit(child, *root, "", {}, {}, true, true)) return found;
    }
  }
  return std::nullopt;
}

bool ActionParentExists(std::string_view data_xml,
                        const ActionInstance& action) {
  pugi::xml_document document;
  if (!document.load_buffer(data_xml.data(), data_xml.size())) return false;
  std::vector<pugi::xml_node> candidates{document.document_element()};
  for (const ActionInstance::AncestorSelector& selector : action.selectors) {
    std::vector<pugi::xml_node> matches;
    for (const pugi::xml_node parent : candidates) {
      for (const pugi::xml_node child : parent.children()) {
        if (child.type() != pugi::node_element ||
            NamespaceFor(child).value_or("") != selector.name.namespace_uri ||
            LocalName(child.name()) != selector.name.local_name) continue;
        const bool keys_match = std::ranges::all_of(
            selector.keys, [&](const auto& expected) {
              for (const pugi::xml_node key : child.children()) {
                if (key.type() == pugi::node_element &&
                    NamespaceFor(key).value_or("") ==
                        expected.first.namespace_uri &&
                    LocalName(key.name()) == expected.first.local_name &&
                    key.text().as_string() == expected.second) return true;
              }
              return false;
            });
        if (keys_match) matches.push_back(child);
      }
    }
    if (matches.empty()) return false;
    candidates = std::move(matches);
  }
  return !candidates.empty();
}

std::optional<config::RuntimeSchemaNodeId> FindNamedSchema(
    const config::RuntimeSchema& schema,
    std::span<const config::RuntimeSchemaNodeId> candidates,
    const pugi::xml_node& xml) {
  const config::QualifiedXmlName name{NamespaceFor(xml).value_or(""),
                                      std::string(LocalName(xml.name()))};
  const auto found = std::ranges::find_if(
      candidates, [&](config::RuntimeSchemaNodeId id) {
        return schema.Get(id).name == name;
      });
  return found == candidates.end()
      ? std::nullopt : std::optional<config::RuntimeSchemaNodeId>(*found);
}

TransactionResult ValidateOperationData(
    const config::RuntimeSchema& schema,
    config::RuntimeSchemaNodeId operation, semantic::SchemaNodeKind io_kind,
    const pugi::xml_node& operation_xml) {
  std::vector<config::ValidationFinding> findings;
  const std::string_view direction =
      io_kind == semantic::SchemaNodeKind::kOutput ? "output" : "input";
  std::function<void(const pugi::xml_node&,
                     const std::vector<config::RuntimeSchemaNodeId>&,
                     std::string_view)> validate_children;
  validate_children = [&](const pugi::xml_node& parent,
                          const std::vector<config::RuntimeSchemaNodeId>& allowed,
                          std::string_view parent_path) {
    std::map<config::RuntimeSchemaNodeId, std::size_t> counts;
    for (const pugi::xml_node child : parent.children()) {
      if (child.type() != pugi::node_element) continue;
      const auto schema_id = FindNamedSchema(schema, allowed, child);
      if (!schema_id) {
        config::ValidationFinding finding;
        finding.message = "operation " + std::string(direction) +
                          " contains an unknown data node";
        finding.instance_path = std::string(parent_path) + "/" +
                                std::string(LocalName(child.name()));
        finding.netconf_error_tag = "unknown-element";
        findings.push_back(std::move(finding));
        continue;
      }
      const config::RuntimeSchemaNode& metadata = schema.Get(*schema_id);
      ++counts[*schema_id];
      const std::string path = std::string(parent_path) + "/{" +
          metadata.name.namespace_uri + "}" + metadata.name.local_name;
      const bool scalar = metadata.kind == semantic::SchemaNodeKind::kLeaf ||
                          metadata.kind == semantic::SchemaNodeKind::kLeafList;
      if (scalar && metadata.type) {
        const bool empty = metadata.type->builtin ==
                           semantic::BuiltinType::kEmpty;
        const bool valid = empty
            ? !std::ranges::any_of(child.children(), [](pugi::xml_node node) {
                return node.type() == pugi::node_element;
              }) && std::string_view(child.text().as_string()).empty()
            : semantic::ValueMatchesType(*metadata.type,
                                         child.text().as_string());
        if (!valid) {
          config::ValidationFinding finding;
          finding.message = "operation " + std::string(direction) +
                            " value does not match its YANG type";
          finding.instance_path = path;
          finding.module_name = metadata.module_name;
          finding.netconf_error_tag = "invalid-value";
          findings.push_back(std::move(finding));
        }
      } else if (!scalar) {
        validate_children(child, schema.DataChildren(*schema_id), path);
      }
    }
    for (config::RuntimeSchemaNodeId id : allowed) {
      const config::RuntimeSchemaNode& metadata = schema.Get(id);
      const std::size_t count = counts[id];
      if ((metadata.kind != semantic::SchemaNodeKind::kList &&
           metadata.kind != semantic::SchemaNodeKind::kLeafList && count > 1) ||
          (metadata.max_elements && count > *metadata.max_elements) ||
          (metadata.min_elements && count < *metadata.min_elements) ||
          (metadata.mandatory && count == 0)) {
        config::ValidationFinding finding;
        finding.message = count == 0
            ? "mandatory operation " + std::string(direction) + " is absent"
            : "operation " + std::string(direction) +
                  " has an invalid element count";
        finding.instance_path = std::string(parent_path) + "/{" +
            metadata.name.namespace_uri + "}" + metadata.name.local_name;
        finding.module_name = metadata.module_name;
        finding.netconf_error_tag = count == 0 ? "missing-element"
                                               : "invalid-value";
        findings.push_back(std::move(finding));
      }
    }
  };
  validate_children(operation_xml,
                    schema.OperationDataChildren(operation, io_kind), "");
  return {findings.empty(), std::move(findings), {}};
}

TransactionResult ProtocolFailure(std::string message, std::string tag,
                                  std::string error_path = {},
                                  std::string error_path_namespace = {});

TransactionResult ValidateOperationOutput(
    const config::RuntimeSchema& schema,
    config::RuntimeSchemaNodeId operation, std::string_view output_xml) {
  pugi::xml_document output;
  if (output_xml.empty()) {
    output.append_child("output");
    return ValidateOperationData(schema, operation,
                                 semantic::SchemaNodeKind::kOutput,
                                 output.document_element());
  }
  if (!output.load_buffer(output_xml.data(), output_xml.size()))
    return ProtocolFailure("plugin returned malformed operation output",
                           "operation-failed");
  return ValidateOperationData(schema, operation,
                               semantic::SchemaNodeKind::kOutput, output);
}

std::string FilterOperationOutput(
    const NacmPolicy& nacm, const RpcSessionContext& session,
    const config::RuntimeSchema& schema,
    config::RuntimeSchemaNodeId operation, std::string_view base_path,
    std::string_view output_xml) {
  pugi::xml_document document;
  if (!document.load_buffer(output_xml.data(), output_xml.size())) return {};
  std::function<void(pugi::xml_node,
                     const std::vector<config::RuntimeSchemaNodeId>&,
                     std::string_view)> filter;
  filter = [&](pugi::xml_node parent,
               const std::vector<config::RuntimeSchemaNodeId>& allowed,
               std::string_view parent_path) {
    for (pugi::xml_node child = parent.first_child(); child;) {
      pugi::xml_node next = child.next_sibling();
      if (child.type() == pugi::node_element) {
        const auto schema_id = FindNamedSchema(schema, allowed, child);
        if (!schema_id) {
          parent.remove_child(child);
          child = next;
          continue;
        }
        const config::RuntimeSchemaNode& metadata = schema.Get(*schema_id);
        const std::string path = std::string(parent_path) + "/{" +
            metadata.name.namespace_uri + "}" + metadata.name.local_name;
        if (!nacm.AuthorizeData(
                session.username, metadata.module_name, AccessOperation::kRead,
                path, session.external_groups,
                metadata.nacm_default_deny_all,
                metadata.nacm_default_deny_write)) {
          parent.remove_child(child);
        } else {
          filter(child, schema.DataChildren(*schema_id), path);
        }
      }
      child = next;
    }
  };
  filter(document, schema.OperationDataChildren(
                       operation, semantic::SchemaNodeKind::kOutput), base_path);
  return Serialize(document);
}

std::string FilterDatastoreCopySource(
    const NacmPolicy& nacm, const RpcSessionContext& session,
    const config::RuntimeSchema& schema,
    const config::ConfigDocument& source) {
  const std::string data = "<data xmlns=\"" + std::string(kNetconfNamespace) +
      "\">" + source.ToXml(false) + "</data>";
  const std::string filtered = nacm.FilterReadableData(
      session.username, data, session.external_groups, &schema);
  pugi::xml_document parsed;
  if (!parsed.load_buffer(filtered.data(), filtered.size())) return {};
  pugi::xml_document result;
  pugi::xml_node config = result.append_child("config");
  config.append_attribute("xmlns") = kNetconfNamespace.data();
  for (const pugi::xml_node child : parsed.document_element().children())
    if (child.type() == pugi::node_element) config.append_copy(child);
  return Serialize(result);
}

TransactionResult ProtocolFailure(std::string message, std::string tag,
                                  std::string error_path,
                                  std::string error_path_namespace) {
  config::ValidationFinding finding;
  finding.code = config::ValidationCode::kInvalidValue;
  finding.state = config::FindingState::kInvalid;
  finding.message = std::move(message);
  finding.netconf_error_tag = std::move(tag);
  finding.netconf_error_path = std::move(error_path);
  finding.netconf_error_path_namespace = std::move(error_path_namespace);
  return {false, {std::move(finding)}, {}};
}

bool UrlAllowed(const UrlDatastoreProvider& provider, std::string_view url) {
  const std::size_t colon = url.find(':');
  if (colon == std::string_view::npos || colon == 0) return false;
  const std::string_view scheme = url.substr(0, colon);
  const std::vector<std::string> schemes = provider.Schemes();
  return std::ranges::find(schemes, scheme) != schemes.end();
}

TransactionResult ValidateCompleteConfig(const config::RuntimeSchema& schema,
                                         std::string_view xml) {
  config::ConfigParseResult parsed = config::ParseDatastoreXml(schema, xml);
  if (!parsed.document) return {false, std::move(parsed.findings), {}};
  config::ConfigValidator validator;
  config::ValidationResult validated = validator.Validate(
      {schema, *parsed.document, config::ValidationScope::kComplete});
  return {validated.valid, std::move(validated.findings), {}};
}

TransactionResult AuthorizeReplacement(
    const config::RuntimeSchema& schema, std::string_view before_xml,
    std::string_view after_xml,
    const std::function<bool(const config::ChangeEvent&)>& authorize_change) {
  config::ConfigParseResult before =
      config::ParseDatastoreXml(schema, before_xml);
  config::ConfigParseResult after = config::ParseDatastoreXml(schema, after_xml);
  if (!before.document || !after.document) {
    std::vector<config::ValidationFinding> findings = std::move(before.findings);
    findings.insert(findings.end(),
                    std::make_move_iterator(after.findings.begin()),
                    std::make_move_iterator(after.findings.end()));
    return {false, std::move(findings), {}};
  }
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema, *before.document, *after.document);
  const auto denied = std::ranges::find_if(
      changes, [&](const config::ChangeEvent& change) {
        return !authorize_change(change);
      });
  if (denied != changes.end()) {
    config::ValidationFinding finding;
    finding.code = config::ValidationCode::kInvalidValue;
    finding.state = config::FindingState::kInvalid;
    finding.message = "access to the proposed URL replacement is denied";
    finding.instance_path = denied->instance_path;
    finding.netconf_error_tag = "access-denied";
    return {false, {std::move(finding)}, {}};
  }
  return {true, {}, std::move(changes)};
}

TransactionResult UrlFailure(const UrlResult& result,
                             std::string_view fallback) {
  return ProtocolFailure(result.error.value_or(std::string(fallback)),
                         result.error_tag.value_or("operation-failed"));
}

std::string Reply(std::string_view message_id, const TransactionResult& result,
                  std::string_view payload = {}) {
  std::string xml = "<rpc-reply xmlns=\"" + std::string(kNetconfNamespace) +
                    "\"";
  if (!message_id.empty())
    xml += " message-id=\"" + Escape(message_id) + "\"";
  xml += ">";
  if (result.ok) {
    xml += payload.empty() ? "<ok/>" : std::string(payload);
  } else {
    for (const config::ValidationFinding& error : result.errors) {
      xml += "<rpc-error><error-type>application</error-type>";
      xml += "<error-tag>" + Escape(error.netconf_error_tag.empty()
                                        ? "operation-failed"
                                        : error.netconf_error_tag) +
             "</error-tag><error-severity>error</error-severity>";
      if (!error.netconf_error_app_tag.empty())
        xml += "<error-app-tag>" + Escape(error.netconf_error_app_tag) +
               "</error-app-tag>";
      if (!error.netconf_error_path.empty()) {
        xml += "<error-path xmlns:nc=\"" + std::string(kNetconfNamespace) +
               "\"";
        if (!error.netconf_error_path_namespace.empty())
          xml += " xmlns:op=\"" +
                 Escape(error.netconf_error_path_namespace) + "\"";
        xml += ">" + Escape(error.netconf_error_path) + "</error-path>";
      } else if (!error.instance_path.empty())
        xml += "<error-path>" + Escape(error.instance_path) + "</error-path>";
      std::string message = error.message;
      if (!error.module_name.empty() || !error.instance_path.empty()) {
        message += " (";
        if (!error.module_name.empty())
          message += "module: " + error.module_name;
        if (!error.module_name.empty() && !error.instance_path.empty())
          message += ", ";
        if (!error.instance_path.empty())
          message += "path: " + error.instance_path;
        message += ")";
      }
      xml += "<error-message xml:lang=\"en\">" + Escape(message) +
             "</error-message></rpc-error>";
    }
  }
  return xml + "</rpc-reply>";
}

}  // namespace

std::vector<std::string> NetconfServer::Capabilities() {
  return {
      "urn:ietf:params:netconf:base:1.0",
      "urn:ietf:params:netconf:base:1.1",
      "urn:ietf:params:netconf:capability:writable-running:1.0",
      "urn:ietf:params:netconf:capability:candidate:1.0",
      "urn:ietf:params:netconf:capability:startup:1.0",
      "urn:ietf:params:netconf:capability:validate:1.1",
      "urn:ietf:params:netconf:capability:xpath:1.0",
      "urn:ietf:params:netconf:capability:rollback-on-error:1.0",
      "urn:ietf:params:netconf:capability:confirmed-commit:1.1"};
}

std::vector<std::string> NetconfServer::AdvertisedCapabilities() const {
  std::vector<std::string> result = Capabilities();
  if (urls_ != nullptr && !urls_->Schemes().empty()) {
    std::string capability = "urn:ietf:params:netconf:capability:url:1.0?scheme=";
    for (const std::string& scheme : urls_->Schemes()) {
      if (capability.back() != '=') capability += ',';
      capability += scheme;
    }
    result.push_back(std::move(capability));
  }
  if (notifications_ != nullptr && notifications_->configured()) {
    result.push_back(
        "urn:ietf:params:netconf:capability:notification:1.0");
    result.push_back("urn:ietf:params:netconf:capability:interleave:1.0");
  }
  if (with_defaults_) {
    result.push_back(
        "urn:ietf:params:netconf:capability:with-defaults:1.0?basic-mode="
        "explicit&also-supported=report-all,report-all-tagged,trim");
    result.push_back(
        "urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults?module="
        "ietf-netconf-with-defaults&revision=2011-06-01");
  }
  if (operational_ != nullptr) {
    std::vector<std::string> capabilities = operational_->Capabilities();
    result.insert(result.end(),
                  std::make_move_iterator(capabilities.begin()),
                  std::make_move_iterator(capabilities.end()));
  }
  return result;
}

std::string NetconfServer::ServerHello(std::uint32_t session_id) const {
  std::string xml = "<hello xmlns=\"" + std::string(kNetconfNamespace) +
                    "\"><capabilities>";
  for (const std::string& capability : AdvertisedCapabilities())
    xml += "<capability>" + Escape(capability) + "</capability>";
  return xml + "</capabilities><session-id>" + std::to_string(session_id) +
         "</session-id></hello>";
}

RpcResponse NetconfServer::Process(std::string_view session,
                                   std::string_view rpc_xml) {
  return Process({0, session, session, {}}, rpc_xml);
}

RpcResponse NetconfServer::Process(const RpcSessionContext& session,
                                   std::string_view rpc_xml) {
  if (rpc_xml.size() > DefaultResourceLimits().maximum_xml_bytes) {
    return {Reply("", ProtocolFailure("RPC XML exceeds the byte limit",
                                      "too-big")),
            false};
  }
  const std::optional<NacmPolicy> policy_snapshot =
      nacm_ == nullptr ? std::nullopt
                       : std::optional<NacmPolicy>(*nacm_);
  const NacmPolicy* const nacm =
      policy_snapshot ? &*policy_snapshot : nullptr;
  pugi::xml_document document;
  const pugi::xml_parse_result parsed =
      document.load_buffer(rpc_xml.data(), rpc_xml.size(), pugi::parse_default);
  if (!parsed) {
    TransactionResult error = ProtocolFailure(
        std::string("malformed RPC XML: ") + parsed.description(),
        "malformed-message");
    return {Reply("", error), false};
  }
  std::string resource_error;
  if (!XmlWithinResourceLimits(document, rpc_xml, DefaultResourceLimits(),
                               &resource_error)) {
    return {Reply("", ProtocolFailure(resource_error, "too-big")), false};
  }
  const pugi::xml_node rpc = document.document_element();
  const std::string message_id = rpc.attribute("message-id").value();
  if (LocalName(rpc.name()) != "rpc" || NamespaceFor(rpc) != kNetconfNamespace ||
      message_id.empty()) {
    return {Reply(message_id, ProtocolFailure(
        "expected an rpc element with a message-id", "missing-attribute")),
        false};
  }
  pugi::xml_node operation;
  for (const pugi::xml_node child : rpc.children()) {
    if (child.type() != pugi::node_element) continue;
    if (operation) {
      return {Reply(message_id, ProtocolFailure(
          "an rpc must contain exactly one operation", "malformed-message")),
          false};
    }
    operation = child;
  }
  if (!operation) {
    return {Reply(message_id, ProtocolFailure(
        "rpc operation is missing", "missing-element")), false};
  }
  const std::string operation_namespace = NamespaceFor(operation).value_or("");
  const bool create_subscription =
      LocalName(operation.name()) == "create-subscription" &&
      operation_namespace == kNotificationNamespace &&
      notifications_ != nullptr && notifications_->configured();
  const bool get_schema = LocalName(operation.name()) == "get-schema" &&
                          operation_namespace == kMonitoringNamespace &&
                          operational_ != nullptr;
  const bool get_data = LocalName(operation.name()) == "get-data" &&
                        operation_namespace == kNmdaNamespace;
  const bool edit_data = LocalName(operation.name()) == "edit-data" &&
                         operation_namespace == kNmdaNamespace;
  const auto custom_rpc = (create_subscription || get_schema || get_data ||
                           edit_data || operation_namespace == kNetconfNamespace)
      ? std::optional<config::RuntimeSchemaNodeId>{}
      : datastores_.schema().FindTopLevelOperation(
            {operation_namespace, std::string(LocalName(operation.name()))},
            semantic::SchemaNodeKind::kRpc);
  const bool action_request = LocalName(operation.name()) == "action" &&
                              operation_namespace == kYangActionNamespace;
  const auto action = action_request
      ? FindActionInstance(datastores_.schema(), operation)
      : std::optional<ActionInstance>{};
  if (action_request && !action) {
    return {Reply(message_id, ProtocolFailure(
        "action does not identify one schema action instance", "bad-element")),
        false};
  }
  if (operation_namespace != kNetconfNamespace && !create_subscription &&
      !get_schema && !get_data && !edit_data && !custom_rpc &&
      !action_request) {
    return {Reply(message_id, ProtocolFailure(
        "RPC operation is not in the NETCONF base namespace",
        "unknown-namespace")), false};
  }
  const std::string_view name = LocalName(operation.name());
  TransactionResult result;
  std::string payload;
  bool close_session = false;
  std::function<bool(const config::ChangeEvent&)> authorize_change;
  if (nacm != nullptr) {
    authorize_change = [nacm, schema = &datastores_.schema(),
                        username = session.username,
                        groups = session.external_groups](
                           const config::ChangeEvent& change) {
      AccessOperation access = AccessOperation::kUpdate;
      if (change.kind == config::ChangeKind::kCreated)
        access = AccessOperation::kCreate;
      else if (change.kind == config::ChangeKind::kDeleted)
        access = AccessOperation::kDelete;
      const config::RuntimeSchemaNode* node =
          change.schema == config::kInvalidRuntimeSchemaNodeId
              ? nullptr
              : &schema->Get(change.schema);
      return nacm->AuthorizeData(
          username, node == nullptr ? "" : node->module_name, access,
          change.instance_path, groups,
          node != nullptr && node->nacm_default_deny_all,
          node != nullptr && node->nacm_default_deny_write);
    };
  }
  const config::RuntimeSchemaNode* operation_schema =
      custom_rpc ? &datastores_.schema().Get(*custom_rpc) : nullptr;
  const config::RuntimeSchemaNode* action_schema = action
      ? &datastores_.schema().Get(action->schema) : nullptr;
  const bool authorized = nacm == nullptr ||
      (action_schema != nullptr
          ? nacm->AuthorizeAction(
                session.username, action_schema->module_name,
                action_schema->name.local_name, action->path,
                action->ancestors, session.external_groups,
                action_schema->nacm_default_deny_all)
          : nacm->AuthorizeRpc(
                session.username,
                operation_schema != nullptr
                    ? operation_schema->module_name
                    : (create_subscription ? "notifications"
                        : (get_schema ? "ietf-netconf-monitoring"
                           : ((get_data || edit_data) ? "ietf-netconf-nmda"
                                                      : "ietf-netconf"))),
                name, session.external_groups,
                operation_schema != nullptr &&
                    operation_schema->nacm_default_deny_all));
  if (!authorized) {
    const bool base_operation = operation_namespace == kNetconfNamespace;
    const std::string error_path = "/nc:rpc/" +
        std::string(base_operation ? "nc:" : "op:") + std::string(name);
    return {Reply(message_id, ProtocolFailure(
        "execution of the requested RPC is denied", "access-denied",
        error_path, base_operation ? "" : std::string(operation_namespace))),
        false};
  }
  if (action) {
    if (!action->has_all_keys) {
      result = ProtocolFailure(
          "action instance is missing one or more list keys",
          "missing-element");
    } else if (!action->has_representable_keys) {
      result = ProtocolFailure(
          "an action list key cannot be represented in an instance path",
          "invalid-value");
    } else {
      std::string action_data =
          "<data>" + datastores_.Read(Datastore::kRunning).ToXml(false) +
          "</data>";
      if (operational_ != nullptr)
        action_data = operational_->AugmentDataXml(action_data);
      if (!ActionParentExists(action_data, *action)) {
        result = ProtocolFailure(
            "the parent data instance for the requested action does not exist",
            "data-missing");
      } else {
        result = ValidateOperationData(datastores_.schema(), action->schema,
                                       semantic::SchemaNodeKind::kInput,
                                       action->xml);
      }
    }
    if (!result.ok) {
      // Schema-invalid input never crosses the plugin boundary.
    } else if (operations_ == nullptr) {
      result = ProtocolFailure("no handler is registered for the requested action",
                               "operation-not-supported");
    } else {
      OperationResult invoked = operations_->InvokeAction(
          session, *action_schema, action->path,
          SerializeSelfContained(action->xml));
      result = std::move(invoked.result);
      payload = std::move(invoked.output_xml);
      if (result.ok) {
        result = ValidateOperationOutput(datastores_.schema(), action->schema,
                                         payload);
      }
      if (result.ok && nacm != nullptr && !payload.empty()) {
        payload = FilterOperationOutput(
            *nacm, session, datastores_.schema(), action->schema,
            action->path, payload);
      }
    }
  } else if (get_data) {
    const auto source = ParseNmdaDatastore(Child(operation, "datastore"));
    const pugi::xml_node subtree = Child(operation, "subtree-filter");
    const pugi::xml_node xpath = Child(operation, "xpath-filter");
    const pugi::xml_node config_filter = Child(operation, "config-filter");
    const pugi::xml_node maximum_depth = Child(operation, "max-depth");
    const pugi::xml_node with_origin = Child(operation, "with-origin");
    bool has_origin_filter = false;
    bool origin_includes_intended = false;
    bool origin_excludes_intended = false;
    for (const pugi::xml_node child : operation.children()) {
      const std::string_view child_name = LocalName(child.name());
      if (child_name == "origin-filter") {
        has_origin_filter = true;
        origin_includes_intended = origin_includes_intended ||
                                   OriginValueIsIntended(child);
      } else if (child_name == "negated-origin-filter") {
        origin_excludes_intended = origin_excludes_intended ||
                                   OriginValueIsIntended(child);
      }
    }
    unsigned int depth = 0;
    if (maximum_depth &&
        std::string_view(maximum_depth.text().as_string()) != "unbounded") {
      const std::string_view value = maximum_depth.text().as_string();
      const auto converted = std::from_chars(value.data(),
                                             value.data() + value.size(), depth);
      if (converted.ec != std::errc() ||
          converted.ptr != value.data() + value.size() || depth == 0)
        result = ProtocolFailure("invalid get-data max-depth", "invalid-value");
    }
    if (!source || (subtree && xpath) ||
        (*source == Datastore::kOperational && operational_ == nullptr)) {
      result = ProtocolFailure("get-data requires one supported datastore",
                               "invalid-value");
    } else if ((with_origin || has_origin_filter || origin_excludes_intended) &&
               *source != Datastore::kOperational) {
      result = ProtocolFailure(
          "origin selection is only valid for the operational datastore",
          "invalid-value");
    } else if (config_filter &&
               std::string_view(config_filter.text().as_string()) != "true" &&
               std::string_view(config_filter.text().as_string()) != "false") {
      result = ProtocolFailure("invalid get-data config-filter",
                               "invalid-value");
    } else if (result.errors.empty()) {
      result.ok = true;
      payload = "<data xmlns=\"" + std::string(kNmdaNamespace) + "\">" +
                datastores_.Read(*source).ToXml(false) + "</data>";
      if (*source == Datastore::kOperational)
        payload = operational_->AugmentDataXml(payload);
      if (*source == Datastore::kOperational &&
          ((has_origin_filter && !origin_includes_intended) ||
           origin_excludes_intended))
        payload = FilterConfigKind(payload, datastores_.schema(), false);
      if (nacm != nullptr)
        payload = nacm->FilterReadableData(
            session.username, payload, session.external_groups,
            &datastores_.schema());
      if (config_filter) {
        payload = FilterConfigKind(
            payload, datastores_.schema(),
            std::string_view(config_filter.text().as_string()) == "true");
      }
      if (*source == Datastore::kOperational && with_origin)
        payload = AnnotateIntendedOrigin(payload, datastores_.schema(), true);
      if (subtree) {
        pugi::xml_document filter_document;
        pugi::xml_node filter = filter_document.append_child("filter");
        filter.append_attribute("type") = "subtree";
        for (const pugi::xml_attribute attribute : subtree.attributes()) {
          const std::string_view attribute_name = attribute.name();
          if (attribute_name == "xmlns" ||
              attribute_name.starts_with("xmlns:"))
            filter.append_attribute(attribute.name()) = attribute.value();
        }
        for (const pugi::xml_node child : subtree.children())
          if (child.type() == pugi::node_element) filter.append_copy(child);
        FilterResult filtered = ApplySubtreeFilter(payload,
                                                   Serialize(filter_document));
        if (!filtered.xml) {
          result = ProtocolFailure(*filtered.error,
              filtered.error_tag.value_or("invalid-value"));
          payload.clear();
        } else {
          payload = std::move(*filtered.xml);
        }
      } else if (xpath) {
        pugi::xml_document filter_document;
        pugi::xml_node filter = filter_document.append_child("filter");
        filter.append_attribute("type") = "xpath";
        filter.append_attribute("select") = xpath.text().as_string();
        for (pugi::xml_node current = xpath; current; current = current.parent()) {
          for (const pugi::xml_attribute attribute : current.attributes()) {
            const std::string_view attribute_name = attribute.name();
            if ((attribute_name == "xmlns" ||
                 attribute_name.starts_with("xmlns:")) &&
                !filter.attribute(attribute.name()))
              filter.append_attribute(attribute.name()) = attribute.value();
          }
        }
        FilterResult filtered = ApplyXPathFilter(payload,
                                                 Serialize(filter_document));
        if (!filtered.xml) {
          result = ProtocolFailure(*filtered.error,
              filtered.error_tag.value_or("invalid-value"));
          payload.clear();
        } else {
          payload = std::move(*filtered.xml);
        }
      }
      if (result.ok && depth != 0) payload = ApplyMaximumDepth(payload, depth);
    }
  } else if (edit_data) {
    const auto target = ParseNmdaDatastore(Child(operation, "datastore"));
    const auto default_operation = ParseDefaultOperation(
        Child(operation, "default-operation").text().as_string());
    const pugi::xml_node config = Child(operation, "config");
    if (!target || *target == Datastore::kIntended ||
        *target == Datastore::kOperational || !default_operation ||
        !config || Child(operation, "url")) {
      result = ProtocolFailure("invalid or read-only edit-data target/content",
                               "invalid-value");
    } else {
      config::EditParseResult edit = config::ParseEditXml(
          datastores_.schema(), SerializeConfig(config));
      if (!edit.document) {
        result = {false, std::move(edit.findings), {}};
      } else {
        EditConfigRequest request{
            std::string(session.datastore_owner), *target,
            {std::move(*edit.document)}, *default_operation,
            TestOption::kTestThenSet, ErrorOption::kRollbackOnError, {}};
        request.authorize_change = authorize_change;
        result = datastores_.EditConfig(request);
      }
    }
  } else if (custom_rpc) {
    result = ValidateOperationData(datastores_.schema(), *custom_rpc,
                                   semantic::SchemaNodeKind::kInput,
                                   operation);
    if (!result.ok) {
      // Schema-invalid input never crosses the plugin boundary.
    } else if (operations_ == nullptr) {
      result = ProtocolFailure("no handler is registered for the requested RPC",
                               "operation-not-supported");
    } else {
      OperationResult invoked = operations_->InvokeRpc(
          session, *operation_schema, SerializeSelfContained(operation));
      result = std::move(invoked.result);
      payload = std::move(invoked.output_xml);
      if (result.ok) {
        result = ValidateOperationOutput(datastores_.schema(), *custom_rpc,
                                         payload);
      }
      if (result.ok && nacm != nullptr && !payload.empty()) {
        payload = FilterOperationOutput(
            *nacm, session, datastores_.schema(), *custom_rpc, {}, payload);
      }
    }
  } else if (get_schema) {
    const pugi::xml_node identifier = Child(operation, "identifier");
    if (!identifier || std::string_view(identifier.text().as_string()).empty()) {
      result = ProtocolFailure("get-schema requires an identifier",
                               "missing-element");
    } else {
      const pugi::xml_node version = Child(operation, "version");
      const pugi::xml_node format = Child(operation, "format");
      const std::optional<std::string_view> requested_version =
          version ? std::optional<std::string_view>(version.text().as_string())
                  : std::nullopt;
      const std::string_view requested_format =
          format ? std::string_view(format.text().as_string()) : "yang";
      const auto lookup = operational_->GetSchema(
          identifier.text().as_string(), requested_version, requested_format);
      using Status = OperationalDataProvider::SchemaLookup::Status;
      if (lookup.status == Status::kFound) {
        result.ok = true;
        payload = "<data xmlns=\"" + std::string(kMonitoringNamespace) +
                  "\">" + Escape(lookup.content) + "</data>";
      } else if (lookup.status == Status::kNotUnique) {
        result = ProtocolFailure("more than one schema matches the request",
                                 "operation-failed");
        result.errors.front().netconf_error_app_tag = "data-not-unique";
      } else if (lookup.status == Status::kUnsupportedFormat) {
        result = ProtocolFailure("requested schema format is not available",
                                 "invalid-value");
      } else {
        result = ProtocolFailure("requested schema does not exist",
                                 "invalid-value");
      }
    }
  } else if (create_subscription) {
    SubscriptionRequest request;
    request.session_id = session.session_id;
    request.username = std::string(session.username);
    request.external_groups.assign(session.external_groups.begin(),
                                   session.external_groups.end());
    if (const pugi::xml_node stream = Child(operation, "stream"))
      request.stream = stream.text().as_string();
    if (const pugi::xml_node filter = Child(operation, "filter"))
      request.filter_xml = SerializeSelfContained(filter);
    const pugi::xml_node start = Child(operation, "startTime");
    const pugi::xml_node stop = Child(operation, "stopTime");
    if (start) request.start_time = ParseNotificationTime(start.text().as_string());
    if (stop) request.stop_time = ParseNotificationTime(stop.text().as_string());
    if ((start && !request.start_time) || (stop && !request.stop_time)) {
      result = ProtocolFailure("invalid RFC 3339 subscription time",
                               "bad-element");
    } else {
      const SubscriptionResult subscribed =
          notifications_->Subscribe(std::move(request));
      result = subscribed.ok
          ? TransactionResult{true, {}, {}}
          : ProtocolFailure(subscribed.error, subscribed.error_tag);
    }
  } else if (name == "get-config" || name == "get") {
    Datastore source = Datastore::kRunning;
    WithDefaultsMode defaults_mode = WithDefaultsMode::kExplicit;
    if (const pugi::xml_node requested = Child(operation, "with-defaults")) {
      if (!with_defaults_) {
        result = ProtocolFailure("with-defaults capability is not configured",
                                 "operation-not-supported");
      } else if (const auto parsed_mode =
                     ParseWithDefaultsMode(requested.text().as_string())) {
        defaults_mode = *parsed_mode;
      } else {
        result = ProtocolFailure("invalid with-defaults retrieval mode",
                                 "invalid-value");
      }
    }
    if (name == "get-config") {
      const auto parsed_source = ParseDatastore(Child(operation, "source"));
      if (!parsed_source) result = ProtocolFailure(
          "get-config requires one valid source datastore", "invalid-value");
      else source = *parsed_source;
    }
    if (result.errors.empty()) {
      result.ok = true;
      payload = with_defaults_
                    ? SerializeWithDefaults(datastores_.schema(),
                                            datastores_.Read(source),
                                            defaults_mode)
                    : "<data>" + datastores_.Read(source).ToXml(false) +
                          "</data>";
      if (name == "get" && operational_ != nullptr) {
        payload = operational_->AugmentDataXml(payload);
      }
      if (nacm != nullptr)
        payload = nacm->FilterReadableData(
            session.username, payload, session.external_groups,
            &datastores_.schema());
      if (const pugi::xml_node filter = Child(operation, "filter")) {
        const std::string_view type = filter.attribute("type").value();
        FilterResult filtered = type == "xpath"
            ? ApplyXPathFilter(payload, SerializeSelfContained(filter))
            : ApplySubtreeFilter(payload, SerializeSelfContained(filter));
        if (!filtered.xml) {
          result = ProtocolFailure(
              *filtered.error,
              filtered.error_tag.value_or("operation-not-supported"));
          payload.clear();
        } else {
          payload = std::move(*filtered.xml);
        }
      }
    }
  } else if (name == "edit-config") {
    const auto target = ParseDatastore(Child(operation, "target"));
    const auto default_operation = ParseDefaultOperation(
        Child(operation, "default-operation").text().as_string());
    const auto test_option = ParseTestOption(
        Child(operation, "test-option").text().as_string());
    const auto error_option = ParseErrorOption(
        Child(operation, "error-option").text().as_string());
    pugi::xml_node config = Child(operation, "config");
    const pugi::xml_node url = Child(operation, "url");
    std::string url_config;
    pugi::xml_document url_document;
    if (!config && url && urls_ != nullptr &&
        UrlAllowed(*urls_, url.text().as_string())) {
      UrlResult read = urls_->Read(url.text().as_string());
      if (!read.config_xml) {
        result = UrlFailure(read, "URL read failed");
      } else {
        url_config = std::move(*read.config_xml);
        if (url_config.size() > DefaultResourceLimits().maximum_xml_bytes) {
          result = ProtocolFailure("URL configuration exceeds the byte limit",
                                   "too-big");
        } else if (url_document.load_buffer(url_config.data(), url_config.size(),
                                            pugi::parse_default)) {
          config = url_document.document_element();
        }
      }
    }
    if (!result.errors.empty()) {
      // Preserve the provider failure.
    } else if (!target || !default_operation || !test_option || !error_option ||
               !config || (url && Child(operation, "config"))) {
      result = ProtocolFailure("invalid edit-config parameters", "invalid-value");
    } else {
      config::EditParseResult edit =
          config::ParseEditXml(datastores_.schema(), SerializeConfig(config));
      if (!edit.document) {
        result = {false, std::move(edit.findings), {}};
      } else {
        EditConfigRequest request{
            std::string(session.datastore_owner), *target,
            {std::move(*edit.document)},
            *default_operation, *test_option, *error_option, {}};
        request.authorize_change = authorize_change;
        result = datastores_.EditConfig(request);
      }
    }
  } else if (name == "lock" || name == "unlock") {
    const auto target = ParseDatastore(Child(operation, "target"));
    if (!target) result = ProtocolFailure("invalid lock target", "invalid-value");
    else result = name == "lock"
        ? datastores_.Lock(*target, session.datastore_owner)
        : datastores_.Unlock(*target, session.datastore_owner);
  } else if (name == "validate") {
    const pugi::xml_node source_node = Child(operation, "source");
    const auto source = ParseDatastore(source_node);
    const pugi::xml_node url = Child(source_node, "url");
    if (source) result = datastores_.Validate(*source);
    else if (url && urls_ != nullptr && UrlAllowed(*urls_, url.text().as_string())) {
      UrlResult read = urls_->Read(url.text().as_string());
      result = read.config_xml
          ? ValidateCompleteConfig(datastores_.schema(), *read.config_xml)
          : UrlFailure(read, "URL read failed");
    } else result = ProtocolFailure("invalid validate source", "invalid-value");
  } else if (name == "discard-changes") {
    result = datastores_.DiscardChanges(session.datastore_owner);
  } else if (name == "commit") {
    const pugi::xml_node persist_id = Child(operation, "persist-id");
    if (persist_id && Child(operation, "confirmed")) {
      std::chrono::seconds timeout(600);
      if (const pugi::xml_node timeout_node =
              Child(operation, "confirm-timeout")) {
        unsigned long seconds = 0;
        const std::string_view value = timeout_node.text().as_string();
        const auto converted = std::from_chars(
            value.data(), value.data() + value.size(), seconds);
        if (converted.ec != std::errc() ||
            converted.ptr != value.data() + value.size() || seconds == 0) {
          result = ProtocolFailure("invalid confirm-timeout", "invalid-value");
        } else {
          timeout = std::chrono::seconds(seconds);
        }
      }
      if (result.errors.empty()) {
        result = datastores_.ContinueConfirmedCommit(
            session.datastore_owner, persist_id.text().as_string(), timeout);
      }
    } else if (persist_id) {
      result = datastores_.ConfirmCommit(session.datastore_owner,
                                         persist_id.text().as_string());
    } else if (Child(operation, "confirmed")) {
      ConfirmedCommitOptions options;
      if (const pugi::xml_node timeout = Child(operation, "confirm-timeout")) {
        unsigned long seconds = 0;
        const std::string_view value = timeout.text().as_string();
        const auto converted = std::from_chars(value.data(),
                                               value.data() + value.size(), seconds);
        if (converted.ec != std::errc() || converted.ptr != value.data() + value.size() ||
            seconds == 0) {
          result = ProtocolFailure("invalid confirm-timeout", "invalid-value");
        } else {
          options.timeout = std::chrono::seconds(seconds);
        }
      }
      if (const pugi::xml_node persist = Child(operation, "persist"))
        options.persist = persist.text().as_string();
      if (result.errors.empty())
        result = datastores_.Commit(session.datastore_owner, options,
                                    authorize_change);
    } else {
      result = datastores_.Commit(session.datastore_owner, std::nullopt,
                                  authorize_change);
    }
  } else if (name == "cancel-commit") {
    const pugi::xml_node persist_id = Child(operation, "persist-id");
    result = datastores_.CancelCommit(
        session.datastore_owner,
        persist_id ? std::optional<std::string_view>(persist_id.text().as_string())
                   : std::nullopt);
  } else if (name == "copy-config") {
    const pugi::xml_node source_node = Child(operation, "source");
    const auto source = ParseDatastore(source_node);
    const pugi::xml_node target_node = Child(operation, "target");
    const auto target = ParseDatastore(target_node);
    const pugi::xml_node inline_config = Child(source_node, "config");
    const pugi::xml_node source_url_node = Child(source_node, "url");
    const pugi::xml_node target_url_node = Child(target_node, "url");
    if ((!target && !target_url_node) ||
        (!source && !inline_config && !source_url_node)) {
      result = ProtocolFailure("invalid copy-config source or target",
                               "invalid-value");
    } else if (target_url_node) {
      if (urls_ == nullptr ||
          !UrlAllowed(*urls_, target_url_node.text().as_string())) {
        result = ProtocolFailure("unsupported URL target", "invalid-value");
      } else {
        if (source_url_node &&
            std::string_view(source_url_node.text().as_string()) ==
                target_url_node.text().as_string()) {
          result = ProtocolFailure("URL source and target must be different",
                                   "invalid-value");
        }
        std::string config_xml;
        if (source) {
          config_xml = nacm == nullptr
              ? datastores_.Read(*source).ToXml()
              : FilterDatastoreCopySource(
                    *nacm, session, datastores_.schema(),
                    datastores_.Read(*source));
        } else if (inline_config) config_xml = SerializeConfig(inline_config);
        else if (urls_ != nullptr &&
                 UrlAllowed(*urls_, source_url_node.text().as_string())) {
          UrlResult read = urls_->Read(source_url_node.text().as_string());
          if (read.config_xml) config_xml = std::move(*read.config_xml);
          else result = UrlFailure(read, "URL read failed");
        }
        if (result.errors.empty()) {
          result = ValidateCompleteConfig(datastores_.schema(), config_xml);
        }
        if (result.ok && authorize_change) {
          UrlResult current =
              urls_->Read(target_url_node.text().as_string());
          if (!current.config_xml) {
            result = UrlFailure(current,
                                "URL target cannot be read for authorization");
          } else {
            result = AuthorizeReplacement(datastores_.schema(),
                                          *current.config_xml, config_xml,
                                          authorize_change);
          }
        }
        if (result.ok) {
          UrlResult written = urls_->Write(target_url_node.text().as_string(),
                                           config_xml);
          result = written.error
              ? UrlFailure(written, "URL write failed")
              : TransactionResult{true, {}, {}};
        }
      }
    } else if (source) {
      if (*source == *target) {
        result = ProtocolFailure(
            "copy-config source and target must be different", "invalid-value");
      } else if (*source == Datastore::kRunning &&
                 *target == Datastore::kStartup) {
        result = datastores_.CopyConfig(session.datastore_owner, *source,
                                        *target);
      } else {
        const std::string source_xml = nacm == nullptr
            ? datastores_.Read(*source).ToXml()
            : FilterDatastoreCopySource(
                  *nacm, session, datastores_.schema(),
                  datastores_.Read(*source));
        config::ConfigParseResult parsed_source = config::ParseDatastoreXml(
            datastores_.schema(), source_xml);
        if (!parsed_source.document) {
          result = {false, std::move(parsed_source.findings), {}};
        } else {
          result = datastores_.CopyConfig(
              session.datastore_owner, *parsed_source.document, *target,
              authorize_change);
        }
      }
    } else {
      std::string source_xml;
      if (source_url_node) {
        if (urls_ == nullptr ||
            !UrlAllowed(*urls_, source_url_node.text().as_string())) {
          result = ProtocolFailure("unsupported URL source", "invalid-value");
        } else {
          UrlResult read = urls_->Read(source_url_node.text().as_string());
          if (read.config_xml) source_xml = std::move(*read.config_xml);
          else result = UrlFailure(read, "URL read failed");
        }
      } else {
        source_xml = SerializeConfig(inline_config);
      }
      bool has_edit_attributes = false;
      std::function<void(pugi::xml_node)> inspect = [&](pugi::xml_node parent) {
        for (const pugi::xml_node child : parent.children()) {
          if (child.type() != pugi::node_element) continue;
          for (const pugi::xml_attribute attribute : child.attributes()) {
            const std::string_view attribute_name = LocalName(attribute.name());
            if (attribute_name == "operation" || attribute_name == "insert" ||
                attribute_name == "key" || attribute_name == "value") {
              has_edit_attributes = true;
            }
          }
          inspect(child);
        }
      };
      if (inline_config) inspect(inline_config);
      if (has_edit_attributes) {
        result = ProtocolFailure(
            "inline copy-config source cannot contain edit attributes",
            "invalid-value");
      } else {
        config::ConfigParseResult parsed_source = config::ParseDatastoreXml(
            datastores_.schema(), source_xml);
        if (!parsed_source.document) {
          result = {false, std::move(parsed_source.findings), {}};
        } else {
          result = datastores_.CopyConfig(
              session.datastore_owner, *parsed_source.document, *target,
              authorize_change);
        }
      }
    }
  } else if (name == "delete-config") {
    const pugi::xml_node target_node = Child(operation, "target");
    const auto target = ParseDatastore(target_node);
    const pugi::xml_node url = Child(target_node, "url");
    if (target) result = datastores_.DeleteConfig(session.datastore_owner, *target);
    else if (url && urls_ != nullptr && UrlAllowed(*urls_, url.text().as_string())) {
      UrlResult deleted = urls_->Delete(url.text().as_string());
      result = deleted.error ? UrlFailure(deleted, "URL delete failed")
                             : TransactionResult{true, {}, {}};
    } else result = ProtocolFailure("invalid delete-config target", "invalid-value");
  } else if (name == "close-session") {
    datastores_.CloseSession(session.datastore_owner);
    result.ok = true;
    close_session = true;
  } else if (name == "kill-session") {
    const pugi::xml_node session_id_node = Child(operation, "session-id");
    const std::string_view value = session_id_node.text().as_string();
    std::uint32_t target = 0;
    const auto converted = std::from_chars(
        value.data(), value.data() + value.size(), target);
    if (!session_id_node || session_id_node.next_sibling() || value.empty() ||
        converted.ec != std::errc() ||
        converted.ptr != value.data() + value.size() || target == 0) {
      result = ProtocolFailure("kill-session requires one valid session-id",
                               "invalid-value");
    } else if (target == session.session_id) {
      result = ProtocolFailure("a session cannot terminate itself",
                               "invalid-value");
    } else if (!sessions_.RequestClose(target)) {
      result = ProtocolFailure("the requested session is not active",
                               "invalid-value");
    } else {
      datastores_.CloseSession(std::to_string(target));
      result.ok = true;
    }
  } else {
    result = ProtocolFailure("unsupported RPC operation", "operation-not-supported");
  }
  return {Reply(message_id, result, payload), close_session};
}

bool NetconfServer::RegisterSession(std::uint32_t session_id,
                                    std::string username) {
  return sessions_.Register(session_id, std::move(username));
}

void NetconfServer::SessionClosed(std::uint32_t session_id,
                                  std::string_view datastore_owner) {
  datastores_.CloseSession(datastore_owner);
  if (notifications_ != nullptr) notifications_->RemoveSession(session_id);
  sessions_.Unregister(session_id);
}

std::vector<std::string> NetconfServer::DrainNotifications(
    std::uint32_t session_id) {
  return notifications_ == nullptr ? std::vector<std::string>{}
                                   : notifications_->Drain(session_id);
}

bool NetconfServer::CloseRequested(std::uint32_t session_id) const {
  return sessions_.CloseRequested(session_id);
}

std::vector<SessionInfo> NetconfServer::Sessions() const {
  return sessions_.List();
}

}  // namespace yang::netconf
