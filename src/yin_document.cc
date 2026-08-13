// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/yin_document.h"

#include <sstream>
#include <string_view>
#include <map>
#include <unordered_map>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "yang/schema_registry.h"
#include "yang/schema_tree.h"
#include "yang/module_resolver.h"
#include "yang/resource_limits.h"
#include "yang/validator.h"

namespace yang {
namespace {

constexpr std::string_view kYinNamespace = "urn:ietf:params:xml:ns:yang:yin:1";

struct ExtensionArgument {
  std::string name = "value";
  bool yin_element = false;
  bool has_argument = false;
};

struct ConversionContext {
  const SyntaxTree& tree;
  DiagnosticSink& diagnostics;
  std::string module_prefix;
  std::string module_namespace;
  std::map<std::string, std::string> namespaces;
  std::unordered_map<std::string, ExtensionArgument> extensions;
};

const Statement* FindChild(const SyntaxTree& tree, const Statement& parent, std::string_view keyword) {
  for (const StatementId id : parent.children) {
    const Statement& child = tree.Get(id);
    if (child.keyword == keyword) return &child;
  }
  return nullptr;
}

void DiscoverExtensions(ConversionContext& context, const SyntaxTree& tree,
                        const Statement& root, std::string_view prefix) {
  for (const StatementId id : root.children) {
    const Statement& extension = tree.Get(id);
    if (extension.keyword != "extension" || !extension.argument) continue;
    ExtensionArgument metadata;
    if (const Statement* argument = FindChild(tree, extension, "argument");
        argument != nullptr && argument->argument) {
      metadata.has_argument = true;
      metadata.name = *argument->argument;
      if (const Statement* yin = FindChild(tree, *argument, "yin-element");
          yin != nullptr && yin->argument == "true") {
        metadata.yin_element = true;
      }
    }
    context.extensions.emplace(std::string(prefix) + ":" + *extension.argument,
                               std::move(metadata));
  }
}

bool AppendStatement(ConversionContext& context, const Statement& statement,
                     pugi::xml_node parent) {
  std::string element_name = statement.keyword;
  std::string argument_name;
  bool yin_element = false;
  bool extension_statement = false;

  if (const auto spec = SchemaRegistry::Find(statement.keyword)) {
    argument_name = spec->yin_argument;
    yin_element = spec->yin_element;
  } else {
    extension_statement = true;
    const std::size_t colon = statement.keyword.find(':');
    if (colon == std::string::npos) return false;
    const std::string prefix = statement.keyword.substr(0, colon);
    const std::string local_name = statement.keyword.substr(colon + 1);
    if (!context.namespaces.contains(prefix)) {
      context.diagnostics.Report({DiagnosticCode::kUnknownPrefix, DiagnosticSeverity::kError,
          fmt::format("cannot generate YIN for unresolved extension prefix '{}'", prefix), statement.range});
      return false;
    }
    const auto found = context.extensions.find(statement.keyword);
    if (found == context.extensions.end()) {
      context.diagnostics.Report({DiagnosticCode::kUnknownStatement, DiagnosticSeverity::kError,
          fmt::format("extension '{}' is not declared by this module", local_name), statement.range});
      return false;
    }
    argument_name = found->second.name;
    yin_element = found->second.yin_element;
    if (found->second.has_argument != statement.argument.has_value()) {
      context.diagnostics.Report({
          DiagnosticCode::kInvalidExtensionInstance,
          DiagnosticSeverity::kError,
          fmt::format("extension '{}' {} an argument", statement.keyword,
                      found->second.has_argument ? "requires" : "does not take"),
          statement.range});
      return false;
    }
  }

  pugi::xml_node node = parent.append_child(element_name.c_str());
  if (statement.argument && !argument_name.empty()) {
    if (yin_element) {
      const std::string child_name = extension_statement
          ? statement.keyword.substr(0, statement.keyword.find(':')) + ":" + argument_name
          : argument_name;
      node.append_child(child_name.c_str()).text().set(statement.argument->c_str());
    } else {
      node.append_attribute(argument_name.c_str()).set_value(statement.argument->c_str());
    }
  }
  for (const StatementId child : statement.children) {
    if (!AppendStatement(context, context.tree.Get(child), node)) return false;
  }
  return true;
}

std::string_view NodeKeyword(semantic::SchemaNodeKind kind) {
  using semantic::SchemaNodeKind;
  switch (kind) {
    case SchemaNodeKind::kContainer: return "container";
    case SchemaNodeKind::kList: return "list";
    case SchemaNodeKind::kLeaf: return "leaf";
    case SchemaNodeKind::kLeafList: return "leaf-list";
    case SchemaNodeKind::kChoice: return "choice";
    case SchemaNodeKind::kCase: return "case";
    case SchemaNodeKind::kAnydata: return "anydata";
    case SchemaNodeKind::kAnyxml: return "anyxml";
    case SchemaNodeKind::kRpc: return "rpc";
    case SchemaNodeKind::kAction: return "action";
    case SchemaNodeKind::kInput: return "input";
    case SchemaNodeKind::kOutput: return "output";
    case SchemaNodeKind::kNotification: return "notification";
  }
  return "container";
}

std::string_view TypeName(semantic::BuiltinType type) {
  using semantic::BuiltinType;
  switch (type) {
    case BuiltinType::kBinary: return "binary";
    case BuiltinType::kBits: return "bits";
    case BuiltinType::kBoolean: return "boolean";
    case BuiltinType::kDecimal64: return "decimal64";
    case BuiltinType::kEmpty: return "empty";
    case BuiltinType::kEnumeration: return "enumeration";
    case BuiltinType::kIdentityRef: return "identityref";
    case BuiltinType::kInstanceIdentifier: return "instance-identifier";
    case BuiltinType::kInt8: return "int8";
    case BuiltinType::kInt16: return "int16";
    case BuiltinType::kInt32: return "int32";
    case BuiltinType::kInt64: return "int64";
    case BuiltinType::kLeafRef: return "leafref";
    case BuiltinType::kString: return "string";
    case BuiltinType::kUint8: return "uint8";
    case BuiltinType::kUint16: return "uint16";
    case BuiltinType::kUint32: return "uint32";
    case BuiltinType::kUint64: return "uint64";
    case BuiltinType::kUnion: return "union";
  }
  return "string";
}

void AppendEffectiveNode(const semantic::SchemaTree& tree,
                         semantic::SchemaNodeId id, pugi::xml_node parent) {
  const semantic::SchemaNode& schema = tree.Get(id);
  if (!schema.supported) return;
  pugi::xml_node node = parent.append_child(NodeKeyword(schema.kind).data());
  if (schema.kind != semantic::SchemaNodeKind::kInput &&
      schema.kind != semantic::SchemaNodeKind::kOutput) {
    node.append_attribute("name").set_value(schema.name.local_name.c_str());
  }
  if (schema.type) {
    node.append_child("type").append_attribute("name").set_value(
        TypeName(schema.type->builtin).data());
  }
  for (const std::string& value : schema.default_values) {
    node.append_child("default").append_attribute("value").set_value(
        value.c_str());
  }
  if (schema.default_values.empty() && schema.default_value) {
    node.append_child("default").append_attribute("value").set_value(
        schema.default_value->c_str());
  }
  if (!schema.effective_config) {
    node.append_child("config").append_attribute("value").set_value("false");
  }
  if (schema.mandatory) {
    node.append_child("mandatory").append_attribute("value").set_value("true");
  }
  for (const semantic::SchemaNodeId child : schema.children) {
    AppendEffectiveNode(tree, child, node);
  }
}

nlohmann::json NodeToJson(const pugi::xml_node& node) {
  nlohmann::json result = {{"name", node.name()}, {"attributes", nlohmann::json::array()},
                           {"children", nlohmann::json::array()}};
  for (const pugi::xml_attribute attribute : node.attributes()) {
    result["attributes"].push_back(
        {{"name", attribute.name()}, {"value", attribute.value()}});
  }
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_element) {
      result["children"].push_back(NodeToJson(child));
    } else if (child.type() == pugi::node_pcdata ||
               child.type() == pugi::node_cdata) {
      result["children"].push_back({{"text", child.value()}});
    }
  }
  return result;
}

bool JsonToNode(const nlohmann::json& value, pugi::xml_node parent,
                std::size_t depth, std::size_t* nodes) {
  const ResourceLimits& limits = DefaultResourceLimits();
  if (depth > limits.maximum_xml_depth ||
      ++*nodes > limits.maximum_xml_nodes) {
    return false;
  }
  if (!value.is_object() || !value.contains("name") ||
      !value["name"].is_string()) {
    return false;
  }
  pugi::xml_node node = parent.append_child(value["name"].get<std::string>().c_str());
  if (value.contains("attributes")) {
    if (!value["attributes"].is_array()) return false;
    for (const auto& attribute : value["attributes"]) {
      if (!attribute.is_object() || !attribute.contains("name") ||
          !attribute.contains("value") || !attribute["name"].is_string() ||
          !attribute["value"].is_string()) {
        return false;
      }
      node.append_attribute(attribute["name"].get<std::string>().c_str())
          .set_value(attribute["value"].get<std::string>().c_str());
    }
  }
  if (value.contains("children")) {
    if (!value["children"].is_array()) return false;
    for (const auto& child : value["children"]) {
      if (child.is_object() && child.contains("text") &&
          child["text"].is_string()) {
        node.append_child(pugi::node_pcdata).set_value(
            child["text"].get<std::string>().c_str());
      } else if (!JsonToNode(child, node, depth + 1, nodes)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

std::string YinDocument::ToString(bool pretty) const {
  std::ostringstream output;
  document_.save(output, pretty ? "  " : "", pugi::format_default,
                 pugi::encoding_utf8);
  return output.str();
}

nlohmann::json YinDocument::ToJson() const {
  const pugi::xml_node root = document_.document_element();
  return {{"format", "yang-cpp-yin-tree-v1"},
          {"root", root ? NodeToJson(root) : nlohmann::json(nullptr)}};
}

std::optional<YinDocument> YinDocument::FromJson(
    const nlohmann::json& value, DiagnosticSink& diagnostics) {
  if (!value.is_object() || value.value("format", "") !=
                                "yang-cpp-yin-tree-v1" ||
      !value.contains("root") || !value["root"].is_object()) {
    diagnostics.Report({DiagnosticCode::kInvalidYinJson,
                        DiagnosticSeverity::kError,
                        "invalid yang-cpp YIN-tree JSON envelope", {}});
    return std::nullopt;
  }
  YinDocument result;
  result.document().append_child(pugi::node_declaration)
      .append_attribute("version").set_value("1.0");
  std::size_t nodes = 0;
  if (!JsonToNode(value["root"], result.document(), 1, &nodes)) {
    diagnostics.Report({DiagnosticCode::kInvalidYinJson,
                        DiagnosticSeverity::kError,
                        "invalid node in yang-cpp YIN-tree JSON", {}});
    return std::nullopt;
  }
  return result;
}

std::optional<YinDocument> YinConverter::Convert(const SyntaxTree& tree) {
  Validator validator(diagnostics_);
  if (!validator.Validate(tree)) return std::nullopt;
  const Statement& root = tree.Get(tree.roots().front());

  ConversionContext context{.tree = tree, .diagnostics = diagnostics_};
  if (root.keyword == "module") {
    const Statement* prefix = FindChild(tree, root, "prefix");
    const Statement* namespace_statement = FindChild(tree, root, "namespace");
    context.module_prefix = prefix != nullptr ? prefix->argument.value_or("") : "";
    context.module_namespace = namespace_statement != nullptr
        ? namespace_statement->argument.value_or("") : "";
  } else {
    const Statement* belongs_to = FindChild(tree, root, "belongs-to");
    if (belongs_to != nullptr) {
      const Statement* prefix = FindChild(tree, *belongs_to, "prefix");
      context.module_prefix = prefix != nullptr ? prefix->argument.value_or("") : "";
    }
  }
  context.namespaces.emplace(context.module_prefix, context.module_namespace);
  DiscoverExtensions(context, tree, root, context.module_prefix);

  YinDocument result;
  result.document().append_child(pugi::node_declaration)
      .append_attribute("version").set_value("1.0");
  pugi::xml_node xml_root = result.document().append_child(root.keyword.c_str());
  xml_root.append_attribute("xmlns").set_value(std::string(kYinNamespace).c_str());
  if (!context.module_prefix.empty() && !context.module_namespace.empty()) {
    const std::string xmlns = "xmlns:" + context.module_prefix;
    xml_root.append_attribute(xmlns.c_str()).set_value(context.module_namespace.c_str());
  }
  const auto root_spec = SchemaRegistry::Find(root.keyword);
  xml_root.append_attribute(root_spec->yin_argument.data()).set_value(root.argument->c_str());
  for (const StatementId child : root.children) {
    if (!AppendStatement(context, tree.Get(child), xml_root)) return std::nullopt;
  }
  return result;
}

std::optional<YinDocument> YinConverter::Convert(const ResolvedModule& module) {
  if (!module.syntax) return std::nullopt;
  Validator validator(diagnostics_);
  if (!validator.Validate(*module.syntax)) return std::nullopt;
  const SyntaxTree& tree = *module.syntax;
  const Statement& root = tree.Get(tree.roots().front());
  ConversionContext context{.tree = tree,
                            .diagnostics = diagnostics_,
                            .module_prefix = module.prefix,
                            .module_namespace = module.namespace_uri};
  if (!module.prefix.empty()) context.namespaces.emplace(module.prefix, module.namespace_uri);
  DiscoverExtensions(context, tree, root, module.prefix);
  for (const auto& [prefix, imported] : module.imports) {
    context.namespaces.emplace(prefix, imported->namespace_uri);
    if (imported->syntax && !imported->syntax->roots().empty()) {
      const Statement& imported_root = imported->syntax->Get(imported->syntax->roots().front());
      DiscoverExtensions(context, *imported->syntax, imported_root, prefix);
    }
  }
  for (const auto& included : module.includes) {
    if (included->syntax && !included->syntax->roots().empty()) {
      const Statement& included_root = included->syntax->Get(included->syntax->roots().front());
      DiscoverExtensions(context, *included->syntax, included_root, module.prefix);
    }
  }

  YinDocument result;
  result.document().append_child(pugi::node_declaration)
      .append_attribute("version").set_value("1.0");
  pugi::xml_node xml_root = result.document().append_child(root.keyword.c_str());
  xml_root.append_attribute("xmlns").set_value(std::string(kYinNamespace).c_str());
  for (const auto& [prefix, namespace_uri] : context.namespaces) {
    if (prefix.empty() || namespace_uri.empty()) continue;
    const std::string attribute = "xmlns:" + prefix;
    xml_root.append_attribute(attribute.c_str()).set_value(namespace_uri.c_str());
  }
  const auto root_spec = SchemaRegistry::Find(root.keyword);
  xml_root.append_attribute(root_spec->yin_argument.data()).set_value(root.argument->c_str());
  for (const StatementId child : root.children) {
    if (!AppendStatement(context, tree.Get(child), xml_root)) return std::nullopt;
  }
  return result;
}

std::optional<YinDocument> YinConverter::ConvertSubmodule(
    const ResolvedModule& submodule, const ResolvedModule& owner) {
  if (submodule.kind != ModuleKind::kSubmodule ||
      submodule.belongs_to != owner.name) {
    diagnostics_.Report({DiagnosticCode::kBelongsToMismatch,
                         DiagnosticSeverity::kError,
                         fmt::format("submodule '{}' does not belong to module '{}'",
                                     submodule.name, owner.name),
                         {}});
    return std::nullopt;
  }
  ResolvedModule enriched = submodule;
  enriched.prefix = owner.prefix;
  enriched.namespace_uri = owner.namespace_uri;
  for (const auto& [prefix, imported] : owner.imports) {
    enriched.imports.try_emplace(prefix, imported);
  }
  return Convert(enriched);
}

std::optional<YinDocument> YinConverter::ConvertEffective(
    const semantic::SchemaContext& schemas, const ResolvedModule& module) {
  const semantic::SchemaTree* tree = schemas.Find(module);
  if (tree == nullptr) return std::nullopt;
  YinDocument result;
  result.document().append_child(pugi::node_declaration)
      .append_attribute("version").set_value("1.0");
  pugi::xml_node root = result.document().append_child("module");
  root.append_attribute("xmlns").set_value(std::string(kYinNamespace).c_str());
  root.append_attribute("name").set_value(module.name.c_str());
  root.append_child("namespace").append_attribute("uri").set_value(
      module.namespace_uri.c_str());
  root.append_child("prefix").append_attribute("value").set_value(
      module.prefix.c_str());
  for (const semantic::SchemaNodeId id : tree->roots()) {
    AppendEffectiveNode(*tree, id, root);
  }
  return result;
}

}  // namespace yang
