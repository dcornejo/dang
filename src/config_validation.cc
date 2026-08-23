// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/config_validation.h"
#include "yang/xml_security.h"

#include "yang/resource_limits.h"
#include "yang/schema_registry.h"
#include "yang/xml_schema_regex.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <pugixml.hpp>
#include <fmt/format.h>

namespace yang::config {
namespace {
using semantic::SchemaNodeKind;
constexpr std::string_view kNetconfNamespace =
    "urn:ietf:params:xml:ns:netconf:base:1.0";

bool IsDataNode(SchemaNodeKind kind) {
  return kind == SchemaNodeKind::kContainer || kind == SchemaNodeKind::kList ||
         kind == SchemaNodeKind::kLeaf || kind == SchemaNodeKind::kLeafList ||
         kind == SchemaNodeKind::kAnydata || kind == SchemaNodeKind::kAnyxml;
}
bool IsTransparent(SchemaNodeKind kind) {
  return kind == SchemaNodeKind::kChoice || kind == SchemaNodeKind::kCase;
}
bool IsScalar(SchemaNodeKind kind) {
  return kind == SchemaNodeKind::kLeaf || kind == SchemaNodeKind::kLeafList;
}

bool ValueMatchesOrdinaryUnionMember(const semantic::ResolvedType& type,
                                     std::string_view value) {
  if (type.builtin == semantic::BuiltinType::kUnion) {
    return std::ranges::any_of(type.union_members, [&](const auto& member) {
      return ValueMatchesOrdinaryUnionMember(*member, value);
    });
  }
  if (type.builtin == semantic::BuiltinType::kIdentityRef ||
      type.builtin == semantic::BuiltinType::kLeafRef ||
      type.builtin == semantic::BuiltinType::kInstanceIdentifier) return false;
  return semantic::ValueMatchesType(type, value);
}
std::string PathComponent(const QualifiedXmlName& name) {
  return "/{" + name.namespace_uri + "}" + name.local_name;
}
ValidationFinding Finding(ValidationCode code, FindingState state,
                          std::string message, std::string path,
                          std::string tag = "invalid-value",
                          std::string module_name = {}) {
  ValidationFinding result;
  result.code = code;
  result.state = state;
  result.message = std::move(message);
  result.module_name = std::move(module_name);
  result.instance_path = std::move(path);
  result.netconf_error_tag = std::move(tag);
  return result;
}

SourceLocation XmlLocation(std::string_view source, std::size_t offset) {
  SourceLocation location{offset, 1, 1};
  for (std::size_t index = 0; index < std::min(offset, source.size()); ++index) {
    const unsigned char character = static_cast<unsigned char>(source[index]);
    if (character == '\n') {
      ++location.line;
      location.column = 1;
    } else if ((character & 0xC0U) != 0x80U) {
      ++location.column;
    }
  }
  return location;
}

SourceRange XmlRange(std::string_view source, const pugi::xml_node& node) {
  const std::ptrdiff_t raw_offset = node.offset_debug();
  const std::size_t begin = raw_offset < 0 ? 0U : static_cast<std::size_t>(raw_offset);
  const std::size_t end = std::min(source.size(), begin + std::string_view(node.name()).size() + 1U);
  return {XmlLocation(source, begin), XmlLocation(source, end)};
}
void CollectVisible(const RuntimeSchema& schema,
                    const std::vector<RuntimeSchemaNodeId>& candidates,
                    std::vector<RuntimeSchemaNodeId>* result) {
  for (RuntimeSchemaNodeId id : candidates) {
    const RuntimeSchemaNode& node = schema.Get(id);
    if (!node.supported) continue;
    if (IsDataNode(node.kind)) result->push_back(id);
    else if (IsTransparent(node.kind)) CollectVisible(schema, node.children, result);
  }
}
std::optional<RuntimeSchemaNodeId> FindVisible(
    const RuntimeSchema& schema,
    const std::vector<RuntimeSchemaNodeId>& candidates,
    const QualifiedXmlName& name) {
  std::vector<RuntimeSchemaNodeId> visible;
  CollectVisible(schema, candidates, &visible);
  const auto found = std::ranges::find_if(visible, [&](RuntimeSchemaNodeId id) {
    return schema.Get(id).name == name;
  });
  return found == visible.end() ? std::nullopt
                                : std::optional<RuntimeSchemaNodeId>(*found);
}
std::pair<std::string, std::string> SplitName(std::string_view name) {
  const std::size_t colon = name.find(':');
  if (colon == std::string_view::npos) return {"", std::string(name)};
  return {std::string(name.substr(0, colon)), std::string(name.substr(colon + 1))};
}
using NamespaceMap = std::unordered_map<std::string, std::string>;
NamespaceMap ExtendNamespaces(const pugi::xml_node& node,
                              const NamespaceMap& inherited) {
  NamespaceMap result = inherited;
  for (const pugi::xml_attribute attribute : node.attributes()) {
    const std::string_view name = attribute.name();
    if (name == "xmlns") result[""] = attribute.value();
    if (name.starts_with("xmlns:")) result[std::string(name.substr(6))] = attribute.value();
  }
  return result;
}
std::optional<QualifiedXmlName> ExpandName(const pugi::xml_node& node,
                                           const NamespaceMap& namespaces) {
  auto [prefix, local] = SplitName(node.name());
  const auto found = namespaces.find(prefix);
  if (found == namespaces.end()) return std::nullopt;
  return QualifiedXmlName{found->second, std::move(local)};
}
bool HasNonWhitespaceText(const pugi::xml_node& node) {
  for (const pugi::xml_node child : node.children()) {
    if (child.type() != pugi::node_pcdata && child.type() != pugi::node_cdata) continue;
    const std::string_view value = child.value();
    if (std::ranges::any_of(value, [](unsigned char c) { return !std::isspace(c); })) return true;
  }
  return false;
}
std::string TextValue(const pugi::xml_node& node) {
  std::string value;
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) value += child.value();
  }
  return value;
}
bool HasElementChildren(const pugi::xml_node& node) {
  return std::ranges::any_of(node.children(), [](const pugi::xml_node& child) {
    return child.type() == pugi::node_element;
  });
}

}  // namespace

class XmlBinder {
 public:
  XmlBinder(const RuntimeSchema& schema, std::string_view source,
            const ConfigParseOptions& options)
      : schema_(schema), source_(source), options_(options) {}
  ConfigParseResult Bind(const pugi::xml_node& xml_root) {
    result_.source_ = std::string(source_);
    NamespaceMap namespaces = ExtendNamespaces(xml_root, {});
    const auto root_name = ExpandName(xml_root, namespaces);
    if (!root_name) {
      Error(ValidationCode::kUnknownDataNode,
            "XML element uses an undeclared namespace prefix", "");
      return Finish();
    }
    const bool wrapper = root_name->namespace_uri == kNetconfNamespace &&
                         (root_name->local_name == "config" || root_name->local_name == "data");
    if (wrapper) {
      for (const pugi::xml_node child : xml_root.children()) {
        if (child.type() == pugi::node_element)
          BindNode(child, namespaces, std::nullopt, options_.attachment_parent);
      }
    } else {
      BindNode(xml_root, {}, std::nullopt, options_.attachment_parent);
    }
    return Finish();
  }

 private:
  void BindNode(const pugi::xml_node& xml, const NamespaceMap& inherited,
                std::optional<ConfigNodeId> parent,
                std::optional<RuntimeSchemaNodeId> schema_parent) {
    NamespaceMap namespaces = ExtendNamespaces(xml, inherited);
    const auto name = ExpandName(xml, namespaces);
    const std::string parent_path = parent ? paths_.at(*parent) : std::string();
    if (!name) {
      Error(ValidationCode::kUnknownDataNode,
            "XML element uses an undeclared namespace prefix", parent_path);
      return;
    }
    const std::string path = parent_path + PathComponent(*name);
    for (const pugi::xml_attribute attribute : xml.attributes()) {
      const std::string_view attribute_name = attribute.name();
      if (attribute_name == "xmlns" || attribute_name.starts_with("xmlns:")) continue;
      const std::size_t colon = attribute_name.find(':');
      const std::string_view prefix = colon == std::string_view::npos
          ? std::string_view() : attribute_name.substr(0, colon);
      const auto namespace_found = namespaces.find(std::string(prefix));
      constexpr std::string_view kOriginNamespace =
          "urn:ietf:params:xml:ns:yang:ietf-origin";
      const std::string_view local = colon == std::string_view::npos
          ? attribute_name : attribute_name.substr(colon + 1);
      std::string_view origin_value = attribute.value();
      while (!origin_value.empty() && std::isspace(
                 static_cast<unsigned char>(origin_value.front())))
        origin_value.remove_prefix(1);
      while (!origin_value.empty() && std::isspace(
                 static_cast<unsigned char>(origin_value.back())))
        origin_value.remove_suffix(1);
      const std::size_t value_colon = origin_value.find(':');
      const std::string value_prefix = value_colon == std::string_view::npos
          ? std::string() : std::string(origin_value.substr(0, value_colon));
      const auto value_namespace = namespaces.find(value_prefix);
      const std::optional<QualifiedXmlName> origin =
          origin_value.empty() || value_namespace == namespaces.end() ||
              (value_colon != std::string_view::npos &&
               (value_colon == 0 || value_colon + 1 == origin_value.size() ||
                origin_value.find(':', value_colon + 1) !=
                    std::string_view::npos))
          ? std::nullopt
          : std::optional<QualifiedXmlName>({
                value_namespace->second,
                std::string(value_colon == std::string_view::npos
                                ? origin_value
                                : origin_value.substr(value_colon + 1))});
      if (options_.allow_origin_metadata && colon != std::string_view::npos &&
          namespace_found != namespaces.end() &&
          namespace_found->second == kOriginNamespace && local == "origin" &&
          origin && schema_.IdentityIsDerivedFrom(
                        *origin, {std::string(kOriginNamespace), "origin"})) {
        continue;
      }
      Error(ValidationCode::kInvalidNodeShape,
            "attribute is not permitted or is invalid instance metadata", path,
            "unknown-attribute");
    }
    const auto schema_id = schema_parent ? schema_.FindChild(*schema_parent, *name)
                                         : schema_.FindRoot(*name);
    if (!schema_id) {
      Error(ValidationCode::kUnknownDataNode,
            "no configuration schema node matches the expanded XML name", path,
            "unknown-element");
      return;
    }
    const RuntimeSchemaNode& schema_node = schema_.Get(*schema_id);
    ConfigNode node;
    node.id = static_cast<ConfigNodeId>(result_.nodes_.size());
    node.schema = *schema_id;
    node.parent = parent;
    node.name = *name;
    node.child_coverage = options_.coverage;
    node.source_range = XmlRange(source_, xml);
    if (IsScalar(schema_node.kind)) {
      for (const auto& [prefix, namespace_uri] : namespaces) {
        node.value_namespaces.push_back({prefix, namespace_uri});
      }
      std::ranges::sort(node.value_namespaces, {},
                        &XmlNamespaceBinding::prefix);
    }
    if (IsScalar(schema_node.kind)) {
      if (HasElementChildren(xml)) {
        Error(ValidationCode::kInvalidNodeShape,
              "a leaf or leaf-list cannot contain child elements", path);
      } else {
        node.value = TextValue(xml);
      }
    } else if (schema_node.kind != SchemaNodeKind::kAnydata &&
               schema_node.kind != SchemaNodeKind::kAnyxml && HasNonWhitespaceText(xml)) {
      Error(ValidationCode::kInvalidNodeShape,
            "a container or list cannot contain character data", path);
    }
    const ConfigNodeId id = node.id;
    result_.nodes_.push_back(std::move(node));
    paths_.push_back(path);
    if (parent) result_.nodes_.at(*parent).children.push_back(id);
    else result_.roots_.push_back(id);
    if (schema_node.kind == SchemaNodeKind::kAnydata ||
        schema_node.kind == SchemaNodeKind::kAnyxml || IsScalar(schema_node.kind)) return;
    for (const pugi::xml_node child : xml.children()) {
      if (child.type() == pugi::node_element) BindNode(child, namespaces, id, *schema_id);
    }
  }
  void Error(ValidationCode code, std::string message, std::string path,
             std::string tag = "invalid-value") {
    findings_.push_back(Finding(code, FindingState::kInvalid, std::move(message),
                                std::move(path), std::move(tag)));
  }
  ConfigParseResult Finish() {
    ConfigParseResult output;
    output.findings = std::move(findings_);
    if (output.findings.empty()) output.document = std::move(result_);
    return output;
  }
  const RuntimeSchema& schema_;
  std::string_view source_;
  const ConfigParseOptions& options_;
  ConfigDocument result_;
  std::vector<std::string> paths_;
  std::vector<ValidationFinding> findings_;
};

namespace {
std::optional<RuntimeSchemaNodeId> DirectCaseFor(
    const RuntimeSchema& schema, RuntimeSchemaNodeId choice,
    RuntimeSchemaNodeId data_node) {
  std::function<bool(RuntimeSchemaNodeId)> contains = [&](RuntimeSchemaNodeId id) {
    if (id == data_node) return true;
    return std::ranges::any_of(schema.Get(id).children, contains);
  };
  for (RuntimeSchemaNodeId child : schema.Get(choice).children) {
    if (schema.Get(child).kind == SchemaNodeKind::kCase && contains(child)) return child;
  }
  return std::nullopt;
}

std::optional<ConfigNodeId> FindConfigDescendant(
    const ConfigDocument& document, ConfigNodeId root,
    RuntimeSchemaNodeId target) {
  for (ConfigNodeId child : document.Get(root).children) {
    if (document.Get(child).schema == target) return child;
    if (const auto nested = FindConfigDescendant(document, child, target)) return nested;
  }
  return std::nullopt;
}

std::optional<ConfigNodeId> FindRelatedConfigNode(
    const ConfigDocument& document, ConfigNodeId origin,
    RuntimeSchemaNodeId target) {
  std::optional<ConfigNodeId> current = origin;
  while (current) {
    if (document.Get(*current).schema == target) return current;
    if (const auto descendant = FindConfigDescendant(document, *current, target))
      return descendant;
    current = document.Get(*current).parent;
  }
  return std::nullopt;
}

std::optional<ConfigNodeId> FindConfigAncestor(
    const ConfigDocument& document, ConfigNodeId origin,
    RuntimeSchemaNodeId target) {
  std::optional<ConfigNodeId> current = origin;
  while (current) {
    if (document.Get(*current).schema == target) return current;
    current = document.Get(*current).parent;
  }
  return std::nullopt;
}

std::optional<std::string> UniqueComponentValue(
    const RuntimeSchema& schema, const ConfigDocument& document,
    ConfigNodeId list_entry, RuntimeSchemaNodeId target) {
  if (const auto explicit_node = FindConfigDescendant(document, list_entry, target)) {
    return document.Get(*explicit_node).value;
  }
  const RuntimeSchemaNode& leaf = schema.Get(target);
  if (!leaf.default_value) return std::nullopt;
  std::optional<RuntimeSchemaNodeId> ancestor = leaf.parent;
  while (ancestor && *ancestor != document.Get(list_entry).schema) {
    const RuntimeSchemaNode& node = schema.Get(*ancestor);
    if (node.kind == SchemaNodeKind::kChoice || node.kind == SchemaNodeKind::kCase) {
      return std::nullopt;
    }
    if (node.kind == SchemaNodeKind::kContainer && node.presence_container &&
        !FindConfigDescendant(document, list_entry, *ancestor)) {
      return std::nullopt;
    }
    ancestor = node.parent;
  }
  return leaf.default_value;
}

bool ValidIdentifier(std::string_view value) {
  if (value.empty()) return false;
  const auto valid_start = [](unsigned char c) {
    return std::isalpha(c) != 0 || c == '_';
  };
  if (!valid_start(static_cast<unsigned char>(value.front()))) return false;
  return std::ranges::all_of(value.substr(1), [&](unsigned char c) {
    return valid_start(c) || std::isdigit(c) != 0 || c == '-' || c == '.';
  });
}

std::optional<std::string> NamespaceForPrefix(const ConfigNode& node,
                                              std::string_view prefix) {
  const auto found = std::ranges::find(node.value_namespaces, prefix,
                                       &XmlNamespaceBinding::prefix);
  return found == node.value_namespaces.end()
             ? std::nullopt
             : std::optional<std::string>(found->namespace_uri);
}

std::optional<QualifiedXmlName> ParseValueQName(const ConfigNode& context,
                                                std::string_view value) {
  const std::size_t colon = value.find(':');
  if (colon == std::string_view::npos) {
    if (!ValidIdentifier(value)) return std::nullopt;
    return QualifiedXmlName{context.name.namespace_uri, std::string(value)};
  }
  if (value.find(':', colon + 1) != std::string_view::npos) return std::nullopt;
  const std::string_view prefix = value.substr(0, colon);
  const std::string_view local = value.substr(colon + 1);
  if (!ValidIdentifier(prefix) || !ValidIdentifier(local)) return std::nullopt;
  const auto namespace_uri = NamespaceForPrefix(context, prefix);
  if (!namespace_uri) return std::nullopt;
  return QualifiedXmlName{*namespace_uri, std::string(local)};
}

struct InstanceSegment {
  QualifiedXmlName name;
  std::vector<std::string> predicates;
};

std::optional<std::vector<std::string_view>> SplitInstancePath(
    std::string_view value) {
  if (value.empty() || value.front() != '/') return std::nullopt;
  value.remove_prefix(1);
  std::vector<std::string_view> result;
  std::size_t start = 0;
  int brackets = 0;
  char quote = 0;
  for (std::size_t index = 0; index <= value.size(); ++index) {
    const char character = index == value.size() ? '/' : value[index];
    if (quote != 0) {
      if (character == quote) quote = 0;
      continue;
    }
    if (character == '\'' || character == '"') quote = character;
    else if (character == '[') ++brackets;
    else if (character == ']') --brackets;
    else if (character == '/' && brackets == 0) {
      if (index == start) return std::nullopt;
      result.push_back(value.substr(start, index - start));
      start = index + 1;
    }
    if (brackets < 0) return std::nullopt;
  }
  if (quote != 0 || brackets != 0) return std::nullopt;
  return result;
}

std::optional<InstanceSegment> ParseInstanceSegment(const ConfigNode& context,
                                                    std::string_view text) {
  const std::size_t bracket = text.find('[');
  const auto name = ParseValueQName(context, text.substr(0, bracket));
  if (!name) return std::nullopt;
  InstanceSegment result{*name, {}};
  std::size_t offset = bracket;
  while (offset != std::string_view::npos) {
    if (text[offset] != '[') return std::nullopt;
    char quote = 0;
    std::size_t close = std::string_view::npos;
    for (std::size_t index = offset + 1; index < text.size(); ++index) {
      if (quote != 0) {
        if (text[index] == quote) quote = 0;
      } else if (text[index] == '\'' || text[index] == '"') {
        quote = text[index];
      } else if (text[index] == ']') {
        close = index;
        break;
      }
    }
    if (close == std::string_view::npos) return std::nullopt;
    result.predicates.emplace_back(text.substr(offset + 1, close - offset - 1));
    offset = close + 1;
    if (offset == text.size()) break;
    if (text[offset] != '[') return std::nullopt;
  }
  return result;
}

std::optional<std::string_view> QuotedPredicateValue(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  if (value.size() < 2 || (value.front() != '\'' && value.front() != '"') ||
      value.back() != value.front()) return std::nullopt;
  return value.substr(1, value.size() - 2);
}

struct InstanceLookupResult {
  bool syntax_valid = false;
  bool found = false;
};

InstanceLookupResult ResolveInstanceIdentifier(const RuntimeSchema& schema,
                                               const ConfigDocument& document,
                                               const ConfigNode& context,
                                               std::string_view value) {
  const auto parts = SplitInstancePath(value);
  if (!parts) return {};
  std::vector<ConfigNodeId> current;
  for (std::size_t step = 0; step < parts->size(); ++step) {
    const auto segment = ParseInstanceSegment(context, parts->at(step));
    if (!segment) return {};
    std::vector<ConfigNodeId> candidates;
    const auto consider = [&](ConfigNodeId id) {
      if (document.Get(id).name == segment->name) candidates.push_back(id);
    };
    if (step == 0) {
      for (ConfigNodeId root : document.roots()) consider(root);
    } else {
      for (ConfigNodeId parent : current)
        for (ConfigNodeId child : document.Get(parent).children) consider(child);
    }
    std::set<RuntimeSchemaNodeId> predicate_keys;
    for (const std::string& predicate_text : segment->predicates) {
      std::string_view predicate = predicate_text;
      while (!predicate.empty() &&
             std::isspace(static_cast<unsigned char>(predicate.front())))
        predicate.remove_prefix(1);
      while (!predicate.empty() &&
             std::isspace(static_cast<unsigned char>(predicate.back())))
        predicate.remove_suffix(1);
      if (std::ranges::all_of(predicate, [](unsigned char c) {
            return std::isdigit(c) != 0;
          })) {
        if (candidates.empty() ||
            schema.Get(document.Get(candidates.front()).schema).kind !=
                SchemaNodeKind::kLeafList) return {};
        std::size_t position = 0;
        for (char c : predicate) position = position * 10 + static_cast<std::size_t>(c - '0');
        if (position == 0 || position > candidates.size()) candidates.clear();
        else candidates = {candidates.at(position - 1)};
        continue;
      }
      const std::size_t equals = predicate.find('=');
      if (equals == std::string_view::npos ||
          predicate.find('=', equals + 1) != std::string_view::npos) return {};
      std::string_view left = predicate.substr(0, equals);
      while (!left.empty() &&
             std::isspace(static_cast<unsigned char>(left.front())))
        left.remove_prefix(1);
      while (!left.empty() &&
             std::isspace(static_cast<unsigned char>(left.back())))
        left.remove_suffix(1);
      const auto expected = QuotedPredicateValue(predicate.substr(equals + 1));
      if (!expected) return {};
      if (left == ".") {
        if (candidates.empty() ||
            schema.Get(document.Get(candidates.front()).schema).kind !=
                SchemaNodeKind::kLeafList) return {};
        std::erase_if(candidates, [&](ConfigNodeId id) {
          return document.Get(id).value != *expected;
        });
        continue;
      }
      const auto key_name = ParseValueQName(context, left);
      if (!key_name) return {};
      if (candidates.empty()) return {true, false};
      const RuntimeSchemaNode& candidate_schema =
          schema.Get(document.Get(candidates.front()).schema);
      if (candidate_schema.kind != SchemaNodeKind::kList) return {};
      const auto key_schema = std::ranges::find_if(
          candidate_schema.keys, [&](RuntimeSchemaNodeId key) {
            return schema.Get(key).name == *key_name;
          });
      if (key_schema == candidate_schema.keys.end() ||
          !predicate_keys.insert(*key_schema).second) return {};
      std::erase_if(candidates, [&](ConfigNodeId id) {
        return !std::ranges::any_of(document.Get(id).children, [&](ConfigNodeId child) {
          const ConfigNode& key = document.Get(child);
          return key.name == *key_name && key.value == *expected;
        });
      });
    }
    if (!candidates.empty()) {
      const RuntimeSchemaNode& candidate_schema =
          schema.Get(document.Get(candidates.front()).schema);
      if (candidate_schema.kind == SchemaNodeKind::kList &&
          predicate_keys.size() != candidate_schema.keys.size()) return {};
    }
    current = std::move(candidates);
    if (current.empty()) return {true, false};
  }
  return {true, !current.empty()};
}

std::string_view TrimView(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
    value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
    value.remove_suffix(1);
  return value;
}

enum class XPathRuntimeKind { kBoolean, kNumber, kString, kNodes, kInvalid };
struct XPathRuntimeValue {
  XPathRuntimeKind kind = XPathRuntimeKind::kInvalid;
  bool indeterminate = false;
  bool boolean = false;
  double number = 0.0;
  std::string string;
  std::vector<EffectiveNodeId> nodes;
};

class RuntimeXPathEvaluator {
 public:
  RuntimeXPathEvaluator(const RuntimeSchema& schema,
                        const EffectiveDataView& view,
                        const ConfigDocument& document,
                        const RuntimeXPathConstraint& constraint,
                        bool complete, EffectiveNodeId current)
      : schema_(schema), view_(view), document_(document), constraint_(constraint),
        complete_(complete), current_(current) {}

  XPathRuntimeValue Evaluate(std::string_view expression,
                             EffectiveNodeId context,
                             std::size_t context_position = 1,
                             std::size_t context_size = 1) const {
    expression = StripParentheses(TrimView(expression));
    for (const std::string_view operation : {" or ", " and "}) {
      if (const auto position = FindTopLevel(expression, operation)) {
        const auto left = Evaluate(expression.substr(0, *position), context,
                                   context_position, context_size);
        const auto right = Evaluate(
            expression.substr(*position + operation.size()), context,
            context_position, context_size);
        const bool left_bool = Boolean(left);
        const bool right_bool = Boolean(right);
        const bool value = operation == " or " ? left_bool || right_bool
                                                 : left_bool && right_bool;
        const bool determined = operation == " or "
            ? (value && ((!left.indeterminate && left_bool) ||
                         (!right.indeterminate && right_bool)))
            : (!value && ((!left.indeterminate && !left_bool) ||
                          (!right.indeterminate && !right_bool)));
        return BooleanValue(value,
                            !determined && (left.indeterminate || right.indeterminate));
      }
    }
    for (const std::string_view operation : {"!=", "<=", ">=", "=", "<", ">"}) {
      if (const auto position = FindTopLevel(expression, operation)) {
        const auto left = Evaluate(expression.substr(0, *position), context,
                                   context_position, context_size);
        const auto right = Evaluate(
            expression.substr(*position + operation.size()), context,
            context_position, context_size);
        return Compare(left, right, operation);
      }
    }
    if (const auto position = FindTopLevel(expression, " | ")) {
      auto left = Evaluate(expression.substr(0, *position), context,
                           context_position, context_size);
      auto right = Evaluate(expression.substr(*position + 3), context,
                            context_position, context_size);
      if (left.kind != XPathRuntimeKind::kNodes ||
          right.kind != XPathRuntimeKind::kNodes) return Unsupported();
      left.nodes.insert(left.nodes.end(), right.nodes.begin(), right.nodes.end());
      std::ranges::sort(left.nodes);
      left.nodes.erase(std::ranges::unique(left.nodes).begin(), left.nodes.end());
      left.indeterminate = left.indeterminate || right.indeterminate;
      return left;
    }
    for (const std::string_view operation : {" + ", " - "}) {
      if (const auto position = FindTopLevel(expression, operation)) {
        const auto left = Evaluate(expression.substr(0, *position), context,
                                   context_position, context_size);
        const auto right = Evaluate(expression.substr(*position + 3), context,
                                    context_position, context_size);
        return NumberValue(operation == " + " ? Number(left) + Number(right)
                                                : Number(left) - Number(right),
                           left.indeterminate || right.indeterminate);
      }
    }
    for (const std::string_view operation : {" * ", " div ", " mod "}) {
      if (const auto position = FindTopLevel(expression, operation)) {
        const auto left = Evaluate(expression.substr(0, *position), context,
                                   context_position, context_size);
        const auto right = Evaluate(
            expression.substr(*position + operation.size()), context,
            context_position, context_size);
        const double lhs = Number(left);
        const double rhs = Number(right);
        double result = lhs * rhs;
        if (operation == " div ") result = lhs / rhs;
        if (operation == " mod ") result = std::fmod(lhs, rhs);
        return NumberValue(result, left.indeterminate || right.indeterminate);
      }
    }
    if (expression.starts_with('-')) {
      const auto operand = Evaluate(expression.substr(1), context,
                                    context_position, context_size);
      return NumberValue(-Number(operand), operand.indeterminate);
    }
    if (expression.size() >= 2 &&
        ((expression.front() == '\'' && expression.back() == '\'') ||
         (expression.front() == '"' && expression.back() == '"'))) {
      return StringValue(std::string(expression.substr(1, expression.size() - 2)));
    }
    if (expression == "true()") return BooleanValue(true);
    if (expression == "false()") return BooleanValue(false);
    if (const auto function = ParseFunction(expression)) {
      return EvaluateFunction(function->first, function->second, context,
                              context_position, context_size);
    }
    char* end = nullptr;
    const std::string owned(expression);
    const double number = std::strtod(owned.c_str(), &end);
    if (!owned.empty() && end == owned.c_str() + owned.size()) return NumberValue(number);
    return EvaluatePath(expression, context);
  }

 private:
  static XPathRuntimeValue BooleanValue(bool value, bool indeterminate = false) {
    XPathRuntimeValue result;
    result.kind = XPathRuntimeKind::kBoolean;
    result.boolean = value;
    result.indeterminate = indeterminate;
    return result;
  }
  static XPathRuntimeValue NumberValue(double value, bool indeterminate = false) {
    XPathRuntimeValue result;
    result.kind = XPathRuntimeKind::kNumber;
    result.number = value;
    result.indeterminate = indeterminate;
    return result;
  }
  static XPathRuntimeValue StringValue(std::string value,
                                       bool indeterminate = false) {
    XPathRuntimeValue result;
    result.kind = XPathRuntimeKind::kString;
    result.string = std::move(value);
    result.indeterminate = indeterminate;
    return result;
  }
  static XPathRuntimeValue NodesValue(std::vector<EffectiveNodeId> nodes,
                                      bool indeterminate = false) {
    XPathRuntimeValue result;
    result.kind = XPathRuntimeKind::kNodes;
    result.nodes = std::move(nodes);
    result.indeterminate = indeterminate;
    return result;
  }
  static XPathRuntimeValue Unsupported() {
    XPathRuntimeValue result;
    result.indeterminate = true;
    return result;
  }
  static std::string_view StripParentheses(std::string_view value) {
    while (value.size() >= 2 && value.front() == '(' && value.back() == ')') {
      int depth = 0;
      bool covers_all = true;
      char quote = 0;
      for (std::size_t index = 0; index < value.size(); ++index) {
        if (quote != 0) {
          if (value[index] == quote) quote = 0;
          continue;
        }
        if (value[index] == '\'' || value[index] == '"') quote = value[index];
        else if (value[index] == '(') ++depth;
        else if (value[index] == ')' && --depth == 0 && index + 1 != value.size()) {
          covers_all = false;
          break;
        }
      }
      if (!covers_all || depth != 0) break;
      value = TrimView(value.substr(1, value.size() - 2));
    }
    return value;
  }
  static std::optional<std::size_t> FindTopLevel(
      std::string_view expression, std::string_view operation) {
    int parentheses = 0;
    int brackets = 0;
    char quote = 0;
    for (std::size_t index = 0; index + operation.size() <= expression.size(); ++index) {
      const char character = expression[index];
      if (quote != 0) {
        if (character == quote) quote = 0;
        continue;
      }
      if (character == '\'' || character == '"') quote = character;
      else if (character == '(') ++parentheses;
      else if (character == ')') --parentheses;
      else if (character == '[') ++brackets;
      else if (character == ']') --brackets;
      else if (parentheses == 0 && brackets == 0 &&
               expression.substr(index, operation.size()) == operation) return index;
    }
    return std::nullopt;
  }
  static std::optional<std::pair<std::string_view, std::string_view>> ParseFunction(
      std::string_view expression) {
    const std::size_t open = expression.find('(');
    if (open == std::string_view::npos || expression.back() != ')' ||
        !ValidIdentifier(expression.substr(0, open))) return std::nullopt;
    int depth = 0;
    char quote = 0;
    for (std::size_t index = open; index < expression.size(); ++index) {
      const char character = expression[index];
      if (quote != 0) {
        if (character == quote) quote = 0;
      } else if (character == '\'' || character == '"') {
        quote = character;
      } else if (character == '(') {
        ++depth;
      } else if (character == ')' && --depth == 0 &&
                 index + 1 != expression.size()) {
        return std::nullopt;
      }
    }
    if (depth != 0 || quote != 0) return std::nullopt;
    return std::pair(expression.substr(0, open),
                     expression.substr(open + 1, expression.size() - open - 2));
  }
  static std::vector<std::string_view> SplitArguments(std::string_view arguments) {
    std::vector<std::string_view> result;
    int parentheses = 0;
    int brackets = 0;
    char quote = 0;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= arguments.size(); ++index) {
      const char character = index == arguments.size() ? ',' : arguments[index];
      if (quote != 0) {
        if (character == quote) quote = 0;
      } else if (character == '\'' || character == '"') quote = character;
      else if (character == '(') ++parentheses;
      else if (character == ')') --parentheses;
      else if (character == '[') ++brackets;
      else if (character == ']') --brackets;
      else if (character == ',' && parentheses == 0 && brackets == 0) {
        result.push_back(TrimView(arguments.substr(start, index - start)));
        start = index + 1;
      }
    }
    if (arguments.empty()) result.clear();
    return result;
  }
  std::optional<QualifiedXmlName> ConstraintName(std::string_view value) const {
    const std::size_t colon = value.find(':');
    const std::string_view prefix = colon == std::string_view::npos
                                        ? std::string_view()
                                        : value.substr(0, colon);
    const std::string_view local = colon == std::string_view::npos
                                       ? value
                                       : value.substr(colon + 1);
    if (!ValidIdentifier(local)) return std::nullopt;
    const auto found = std::ranges::find(constraint_.namespaces, prefix,
                                         &XmlNamespaceBinding::prefix);
    if (found == constraint_.namespaces.end()) return std::nullopt;
    return QualifiedXmlName{found->namespace_uri, std::string(local)};
  }
  std::optional<EffectiveNodeId> Parent(EffectiveNodeId id) const {
    return view_.Get(id).parent;
  }
  std::vector<EffectiveNodeId> Children(EffectiveNodeId id) const {
    return view_.Get(id).children;
  }
  XPathRuntimeValue EvaluatePath(std::string_view expression,
                                 EffectiveNodeId context) const {
    bool from_current = expression.starts_with("current()");
    if (from_current) expression.remove_prefix(9);
    const bool absolute = !expression.empty() && expression.front() == '/';
    bool descendant_separator = expression.starts_with("//");
    if (descendant_separator) expression.remove_prefix(2);
    else if (absolute || (from_current && expression.starts_with('/'))) {
      expression.remove_prefix(1);
    }
    std::vector<EffectiveNodeId> nodes;
    if (absolute) nodes = view_.roots();
    else nodes = {from_current ? current_ : context};
    bool first_absolute_step = absolute;
    std::size_t start = 0;
    int brackets = 0;
    char quote = 0;
    struct RuntimePathStep {
      std::string_view text;
      bool descendants = false;
    };
    std::vector<RuntimePathStep> steps;
    for (std::size_t index = 0; index <= expression.size(); ++index) {
      const char character = index == expression.size() ? '/' : expression[index];
      if (quote != 0) {
        if (character == quote) quote = 0;
      } else if (character == '\'' || character == '"') quote = character;
      else if (character == '[') ++brackets;
      else if (character == ']') --brackets;
      else if (character == '/' && brackets == 0) {
        if (index == start) {
          if (index == expression.size() || descendant_separator) return {};
          descendant_separator = true;
          start = index + 1;
        } else {
          steps.push_back(
              {expression.substr(start, index - start), descendant_separator});
          descendant_separator = false;
          start = index + 1;
        }
      }
    }
    for (const RuntimePathStep& runtime_step : steps) {
      const std::string_view step = runtime_step.text;
      if (step == ".") continue;
      if (step == "..") {
        std::vector<EffectiveNodeId> parents;
        for (EffectiveNodeId node : nodes) {
          if (const auto parent = Parent(node)) parents.push_back(*parent);
        }
        nodes = std::move(parents);
        first_absolute_step = false;
        continue;
      }
      const std::size_t predicate_open = step.find('[');
      const std::string_view name_text = step.substr(0, predicate_open);
      const bool wildcard = name_text == "*";
      const auto name = wildcard ? std::optional<QualifiedXmlName>()
                                 : ConstraintName(name_text);
      if (!wildcard && !name) return {};
      std::vector<EffectiveNodeId> selected;
      const auto select = [&](EffectiveNodeId candidate) {
        if (wildcard || schema_.Get(view_.Get(candidate).schema).name == *name)
          selected.push_back(candidate);
      };
      if (runtime_step.descendants) {
        const auto visit = [&](const auto& self, EffectiveNodeId candidate) -> void {
          select(candidate);
          for (EffectiveNodeId child : Children(candidate)) self(self, child);
        };
        for (EffectiveNodeId node : nodes) {
          if (first_absolute_step) visit(visit, node);
          else for (EffectiveNodeId child : Children(node)) visit(visit, child);
        }
      } else if (first_absolute_step) {
        for (EffectiveNodeId root : nodes) select(root);
      } else {
        for (EffectiveNodeId node : nodes)
          for (EffectiveNodeId child : Children(node)) select(child);
      }
      first_absolute_step = false;
      std::size_t offset = predicate_open;
      while (offset != std::string_view::npos) {
        int depth = 1;
        char predicate_quote = 0;
        std::size_t close = offset + 1;
        for (; close < step.size(); ++close) {
          const char character = step[close];
          if (predicate_quote != 0) {
            if (character == predicate_quote) predicate_quote = 0;
          } else if (character == '\'' || character == '"') predicate_quote = character;
          else if (character == '[') ++depth;
          else if (character == ']' && --depth == 0) break;
        }
        if (close == step.size()) return {};
        const std::string_view predicate = step.substr(offset + 1, close - offset - 1);
        std::vector<EffectiveNodeId> filtered;
        bool predicate_indeterminate = false;
        for (std::size_t index = 0; index < selected.size(); ++index) {
          const XPathRuntimeValue value =
              Evaluate(predicate, selected[index], index + 1, selected.size());
          const bool keep = value.kind == XPathRuntimeKind::kNumber
                                ? value.number == static_cast<double>(index + 1)
                                : Boolean(value);
          if (keep) filtered.push_back(selected[index]);
          if (value.indeterminate && !keep) predicate_indeterminate = true;
        }
        selected = std::move(filtered);
        if (predicate_indeterminate && selected.empty()) {
          return NodesValue({}, true);
        }
        offset = close + 1 == step.size() ? std::string_view::npos : close + 1;
      }
      nodes = std::move(selected);
    }
    const bool indeterminate = !complete_ && nodes.empty();
    return NodesValue(std::move(nodes), indeterminate);
  }
  std::string String(const XPathRuntimeValue& value) const {
    if (value.kind == XPathRuntimeKind::kString) return value.string;
    if (value.kind == XPathRuntimeKind::kBoolean) return value.boolean ? "true" : "false";
    if (value.kind == XPathRuntimeKind::kNumber) return fmt::format("{}", value.number);
    if (value.kind == XPathRuntimeKind::kNodes && !value.nodes.empty())
      return view_.Get(value.nodes.front()).value.value_or("");
    return {};
  }
  bool Boolean(const XPathRuntimeValue& value) const {
    if (value.kind == XPathRuntimeKind::kBoolean) return value.boolean;
    if (value.kind == XPathRuntimeKind::kNumber) return value.number != 0.0;
    if (value.kind == XPathRuntimeKind::kString) return !value.string.empty();
    if (value.kind == XPathRuntimeKind::kNodes) return !value.nodes.empty();
    return false;
  }
  double Number(const XPathRuntimeValue& value) const {
    if (value.kind == XPathRuntimeKind::kNumber) return value.number;
    const std::string text = String(value);
    char* end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    return end == text.c_str() + text.size() ? number
                                             : std::numeric_limits<double>::quiet_NaN();
  }
  XPathRuntimeValue Compare(const XPathRuntimeValue& left,
                            const XPathRuntimeValue& right,
                            std::string_view operation) const {
    bool result = false;
    const auto strings = [&](const XPathRuntimeValue& value) {
      std::vector<std::string> values;
      if (value.kind == XPathRuntimeKind::kNodes) {
        for (EffectiveNodeId node : value.nodes)
          values.push_back(view_.Get(node).value.value_or(""));
      } else values.push_back(String(value));
      return values;
    };
    if ((operation == "=" || operation == "!=") &&
        (left.kind == XPathRuntimeKind::kBoolean ||
         right.kind == XPathRuntimeKind::kBoolean)) {
      result = Boolean(left) == Boolean(right);
      if (operation == "!=") result = !result;
    } else if (operation == "=" || operation == "!=") {
      const bool numeric = left.kind == XPathRuntimeKind::kNumber ||
                           right.kind == XPathRuntimeKind::kNumber;
      for (const std::string& left_value : strings(left)) {
        for (const std::string& right_value : strings(right)) {
          bool pair_matches = left_value == right_value;
          if (numeric) {
            char* left_end = nullptr;
            char* right_end = nullptr;
            const double left_number = std::strtod(left_value.c_str(), &left_end);
            const double right_number = std::strtod(right_value.c_str(), &right_end);
            pair_matches = left_end == left_value.c_str() + left_value.size() &&
                           right_end == right_value.c_str() + right_value.size() &&
                           left_number == right_number;
          }
          if ((operation == "=" && pair_matches) ||
              (operation == "!=" && !pair_matches)) {
            result = true;
          }
        }
      }
    } else {
      const auto numbers = [&](const XPathRuntimeValue& value) {
        std::vector<double> values;
        if (value.kind == XPathRuntimeKind::kNodes) {
          for (EffectiveNodeId node : value.nodes) {
            values.push_back(Number(StringValue(
                view_.Get(node).value.value_or(""))));
          }
        } else {
          values.push_back(Number(value));
        }
        return values;
      };
      for (double left_number : numbers(left)) {
        for (double right_number : numbers(right)) {
          if (operation == "<") result = result || left_number < right_number;
          else if (operation == "<=") result = result || left_number <= right_number;
          else if (operation == ">") result = result || left_number > right_number;
          else result = result || left_number >= right_number;
        }
      }
    }
    return BooleanValue(result, left.indeterminate || right.indeterminate);
  }
  XPathRuntimeValue EvaluateFunction(std::string_view name,
                                     std::string_view arguments,
                                     EffectiveNodeId context,
                                     std::size_t context_position,
                                     std::size_t context_size) const {
    const auto argument_texts = SplitArguments(arguments);
    std::vector<XPathRuntimeValue> values;
    for (std::string_view argument : argument_texts) {
      values.push_back(
          Evaluate(argument, context, context_position, context_size));
    }
    const bool indeterminate = std::ranges::any_of(
        values, &XPathRuntimeValue::indeterminate);
    if (name == "current") return NodesValue({current_});
    if (name == "position") {
      return NumberValue(static_cast<double>(context_position));
    }
    if (name == "last") return NumberValue(static_cast<double>(context_size));
    if (name == "not") return BooleanValue(!Boolean(values.at(0)), indeterminate);
    if (name == "boolean") return BooleanValue(Boolean(values.at(0)), indeterminate);
    if (name == "string") return StringValue(values.empty() ? view_.Get(context).value.value_or("")
                                                              : String(values.at(0)), indeterminate);
    if (name == "number") return NumberValue(values.empty() ? 0.0 : Number(values.at(0)), indeterminate);
    if (name == "count") return NumberValue(static_cast<double>(values.at(0).nodes.size()), indeterminate);
    if (name == "contains") return BooleanValue(String(values.at(0)).find(String(values.at(1))) != std::string::npos, indeterminate);
    if (name == "starts-with") return BooleanValue(String(values.at(0)).starts_with(String(values.at(1))), indeterminate);
    if (name == "string-length") return NumberValue(static_cast<double>(String(values.empty() ? NodesValue({context}) : values.at(0)).size()), indeterminate);
    if (name == "normalize-space") {
      const std::string input = String(values.empty() ? NodesValue({context}) : values.at(0));
      std::string normalized;
      bool space = false;
      for (char raw_character : input) {
        const unsigned char character =
            static_cast<unsigned char>(raw_character);
        if (std::isspace(character)) space = !normalized.empty();
        else { if (space) normalized.push_back(' '); normalized.push_back(static_cast<char>(character)); space = false; }
      }
      return StringValue(std::move(normalized), indeterminate);
    }
    if (name == "concat") {
      std::string joined;
      for (const auto& value : values) joined += String(value);
      return StringValue(std::move(joined), indeterminate);
    }
    if (name == "substring-before") {
      const std::string input = String(values.at(0));
      const std::string needle = String(values.at(1));
      const std::size_t position = input.find(needle);
      return StringValue(position == std::string::npos ? std::string()
                                                       : input.substr(0, position),
                         indeterminate);
    }
    if (name == "substring-after") {
      const std::string input = String(values.at(0));
      const std::string needle = String(values.at(1));
      const std::size_t position = input.find(needle);
      return StringValue(position == std::string::npos
                             ? std::string()
                             : input.substr(position + needle.size()),
                         indeterminate);
    }
    if (name == "substring") {
      const std::string input = String(values.at(0));
      const long start = static_cast<long>(std::round(Number(values.at(1)))) - 1L;
      if (start < 0 || static_cast<std::size_t>(start) >= input.size())
        return StringValue({}, indeterminate);
      if (values.size() == 2)
        return StringValue(input.substr(static_cast<std::size_t>(start)), indeterminate);
      const long length = static_cast<long>(std::round(Number(values.at(2))));
      return StringValue(length <= 0 ? std::string()
                                    : input.substr(static_cast<std::size_t>(start),
                                                   static_cast<std::size_t>(length)),
                         indeterminate);
    }
    if (name == "translate") {
      std::string output = String(values.at(0));
      const std::string from = String(values.at(1));
      const std::string to = String(values.at(2));
      std::string translated;
      for (char character : output) {
        const std::size_t position = from.find(character);
        if (position == std::string::npos) translated.push_back(character);
        else if (position < to.size()) translated.push_back(to[position]);
      }
      return StringValue(std::move(translated), indeterminate);
    }
    if (name == "sum") {
      double total = 0.0;
      for (EffectiveNodeId node : values.at(0).nodes)
        total += Number(StringValue(view_.Get(node).value.value_or("")));
      return NumberValue(total, indeterminate);
    }
    if (name == "floor") return NumberValue(std::floor(Number(values.at(0))), indeterminate);
    if (name == "ceiling") return NumberValue(std::ceil(Number(values.at(0))), indeterminate);
    if (name == "round") return NumberValue(std::round(Number(values.at(0))), indeterminate);
    if ((name == "local-name" || name == "namespace-uri" || name == "name") &&
        (values.empty() || values.at(0).kind == XPathRuntimeKind::kNodes)) {
      const EffectiveNodeId node = values.empty() ? context :
          (values.at(0).nodes.empty() ? context : values.at(0).nodes.front());
      const QualifiedXmlName& qualified = schema_.Get(view_.Get(node).schema).name;
      if (name == "local-name") return StringValue(qualified.local_name, indeterminate);
      if (name == "namespace-uri") return StringValue(qualified.namespace_uri, indeterminate);
      return StringValue(qualified.local_name, indeterminate);
    }
    if (name == "re-match") {
      const auto expression = XmlSchemaRegex::Compile(String(values.at(1)));
      if (!expression) return Unsupported();
      return BooleanValue(expression->Matches(String(values.at(0))),
                          indeterminate);
    }
    if (name == "bit-is-set") {
      const std::string bits = String(values.at(0));
      std::istringstream input(bits);
      std::string bit;
      while (input >> bit)
        if (bit == String(values.at(1))) return BooleanValue(true, indeterminate);
      return BooleanValue(false, indeterminate);
    }
    if (name == "enum-value" && !values.at(0).nodes.empty()) {
      const EffectiveNode& node = view_.Get(values.at(0).nodes.front());
      const RuntimeSchemaNode& schema_node = schema_.Get(node.schema);
      if (schema_node.type) {
        const auto found = schema_node.type->enum_values.find(node.value.value_or(""));
        if (found != schema_node.type->enum_values.end())
          return NumberValue(static_cast<double>(found->second), indeterminate);
      }
      return NumberValue(std::numeric_limits<double>::quiet_NaN(), indeterminate);
    }
    if ((name == "derived-from" || name == "derived-from-or-self") &&
        !values.at(0).nodes.empty()) {
      const EffectiveNode& node = view_.Get(values.at(0).nodes.front());
      if (!node.explicit_node || !node.value) return BooleanValue(false, indeterminate);
      const auto actual = ParseValueQName(document_.Get(*node.explicit_node), *node.value);
      const auto base = ConstraintName(String(values.at(1)));
      if (!actual || !base) return BooleanValue(false, indeterminate);
      const RuntimeSchemaNode& schema_node = schema_.Get(node.schema);
      const bool in_value_space = std::ranges::find(schema_node.identity_values, *actual) !=
                                  schema_node.identity_values.end();
      const bool same = *actual == *base;
      return BooleanValue(in_value_space &&
          (name == "derived-from-or-self" || !same), indeterminate);
    }
    if (name == "deref" && !values.at(0).nodes.empty()) {
      const EffectiveNode& reference = view_.Get(values.at(0).nodes.front());
      const RuntimeSchemaNode& reference_schema = schema_.Get(reference.schema);
      if (!reference_schema.leafref_target || !reference.value) return NodesValue({});
      std::vector<EffectiveNodeId> targets;
      for (EffectiveNodeId id = 0; id < view_.size(); ++id) {
        if (view_.Get(id).schema == *reference_schema.leafref_target &&
            view_.Get(id).value == reference.value) targets.push_back(id);
      }
      return NodesValue(std::move(targets), indeterminate);
    }
    return Unsupported();
  }

  const RuntimeSchema& schema_;
  const EffectiveDataView& view_;
  const ConfigDocument& document_;
  const RuntimeXPathConstraint& constraint_;
  bool complete_;
  EffectiveNodeId current_;
};

std::optional<EffectiveNodeId> FindEffectiveDescendant(
    const EffectiveDataView& view, EffectiveNodeId root,
    RuntimeSchemaNodeId schema) {
  for (EffectiveNodeId child : view.Get(root).children) {
    if (view.Get(child).schema == schema) return child;
    if (const auto nested = FindEffectiveDescendant(view, child, schema))
      return nested;
  }
  return std::nullopt;
}

std::optional<EffectiveNodeId> FindRelatedEffectiveNode(
    const EffectiveDataView& view, EffectiveNodeId origin,
    RuntimeSchemaNodeId schema) {
  std::optional<EffectiveNodeId> current = origin;
  while (current) {
    if (view.Get(*current).schema == schema) return current;
    if (const auto nested = FindEffectiveDescendant(view, *current, schema))
      return nested;
    current = view.Get(*current).parent;
  }
  return std::nullopt;
}
}  // namespace

const RuntimeSchemaNode& RuntimeSchema::Get(RuntimeSchemaNodeId id) const { return nodes_.at(id); }
std::optional<RuntimeSchemaNodeId> RuntimeSchema::FindRoot(const QualifiedXmlName& name) const {
  return FindVisible(*this, roots_, name);
}
std::optional<RuntimeSchemaNodeId> RuntimeSchema::FindChild(
    RuntimeSchemaNodeId parent, const QualifiedXmlName& name) const {
  return FindVisible(*this, Get(parent).children, name);
}
std::optional<RuntimeSchemaNodeId> RuntimeSchema::FindTopLevelOperation(
    const QualifiedXmlName& name, semantic::SchemaNodeKind kind) const {
  const auto found = std::ranges::find_if(nodes_, [&](const RuntimeSchemaNode& node) {
    return node.supported && !node.parent && node.kind == kind &&
           node.name == name;
  });
  return found == nodes_.end() ? std::nullopt
                               : std::optional<RuntimeSchemaNodeId>(found->id);
}
std::optional<RuntimeSchemaNodeId> RuntimeSchema::FindChildOperation(
    RuntimeSchemaNodeId parent, const QualifiedXmlName& name,
    semantic::SchemaNodeKind kind) const {
  std::function<std::optional<RuntimeSchemaNodeId>(RuntimeSchemaNodeId)> find;
  find = [&](RuntimeSchemaNodeId candidate)
      -> std::optional<RuntimeSchemaNodeId> {
    const RuntimeSchemaNode& node = Get(candidate);
    if (!node.supported) return std::nullopt;
    if (node.kind == kind && node.name == name) return candidate;
    if (IsTransparent(node.kind)) {
      for (RuntimeSchemaNodeId child : node.children)
        if (auto found = find(child)) return found;
    }
    return std::nullopt;
  };
  for (RuntimeSchemaNodeId child : Get(parent).children)
    if (auto found = find(child)) return found;
  return std::nullopt;
}
std::vector<RuntimeSchemaNodeId> RuntimeSchema::ResolveInstancePath(
    std::string_view path) const {
  std::vector<RuntimeSchemaNodeId> result;
  std::size_t position = 0;
  while (position < path.size()) {
    if (path[position] != '/' || position + 2 >= path.size() ||
        path[position + 1] != '{') return {};
    const std::size_t namespace_end = path.find('}', position + 2);
    if (namespace_end == std::string_view::npos) return {};
    const std::size_t local_begin = namespace_end + 1;
    std::size_t local_end = path.find_first_of("[/", local_begin);
    if (local_end == std::string_view::npos) local_end = path.size();
    if (local_begin == local_end) return {};
    const QualifiedXmlName name{
        std::string(path.substr(position + 2,
                                namespace_end - position - 2)),
        std::string(path.substr(local_begin, local_end - local_begin))};
    std::optional<RuntimeSchemaNodeId> node = result.empty()
        ? FindRoot(name) : FindChild(result.back(), name);
    if (!node && !result.empty()) {
      node = FindChildOperation(result.back(), name,
                                semantic::SchemaNodeKind::kAction);
      if (!node)
        node = FindChildOperation(result.back(), name,
                                  semantic::SchemaNodeKind::kNotification);
    }
    if (!node) return {};
    result.push_back(*node);
    position = local_end;
    while (position < path.size() && path[position] == '[') {
      char quote = 0;
      std::size_t close = position + 1;
      for (; close < path.size(); ++close) {
        const char character = path[close];
        if ((character == '\'' || character == '"')) {
          quote = quote == 0 ? character : (quote == character ? 0 : quote);
        } else if (character == ']' && quote == 0) {
          break;
        }
      }
      if (close == path.size()) return {};
      position = close + 1;
    }
  }
  return result;
}
std::vector<RuntimeSchemaNodeId> RuntimeSchema::OperationDataChildren(
    RuntimeSchemaNodeId operation, semantic::SchemaNodeKind io_kind) const {
  std::vector<RuntimeSchemaNodeId> result;
  for (RuntimeSchemaNodeId child : Get(operation).children) {
    const RuntimeSchemaNode& node = Get(child);
    if (node.supported && node.kind == io_kind)
      CollectVisible(*this, node.children, &result);
  }
  return result;
}
std::vector<RuntimeSchemaNodeId> RuntimeSchema::DataChildren(
    RuntimeSchemaNodeId parent) const {
  std::vector<RuntimeSchemaNodeId> result;
  CollectVisible(*this, Get(parent).children, &result);
  return result;
}

bool RuntimeSchema::IdentityIsDerivedFrom(
    const QualifiedXmlName& identity, const QualifiedXmlName& base) const {
  return identity == base ||
         std::ranges::find(identity_derivations_,
                           std::pair(identity, base)) !=
             identity_derivations_.end();
}

RuntimeSchema RuntimeSchemaBuilder::FromCompilation(const Compilation& compilation) {
  RuntimeSchema result;
  const auto has_nacm_annotation = [&](const semantic::SchemaNode& node,
                                       std::string_view name) {
    if (!node.source_module || node.declaration == kInvalidStatementId)
      return false;
    const Statement& declaration =
        node.source_module->syntax->Get(node.declaration);
    return std::ranges::any_of(
        compilation.extensions.instances(),
        [&](const semantic::ExtensionInstance& instance) {
          return instance.definition.name.module == "ietf-netconf-acm" &&
                 instance.definition.name.name == name &&
                 instance.source_module.get() == node.source_module.get() &&
                 std::ranges::find(declaration.children, instance.statement) !=
                     declaration.children.end();
        });
  };
  struct PendingUnionLeafref {
    RuntimeSchemaNodeId node = kInvalidRuntimeSchemaNodeId;
    std::size_t alternative = 0;
    const ResolvedModule* source = nullptr;
    std::string path;
  };
  std::vector<PendingUnionLeafref> pending_union_leafrefs;
  std::unordered_map<std::string, std::string> namespaces;
  std::vector<const ResolvedModule*> modules = compilation.schemas.modules();
  std::ranges::sort(modules, {}, [](const ResolvedModule* module) {
    return std::pair(module->name, module->revision.value_or(""));
  });
  for (const ResolvedModule* module : modules)
    namespaces[module->name] = module->namespace_uri;
  std::unordered_map<const ResolvedModule*, std::vector<RuntimeSchemaNodeId>> ids;
  for (const ResolvedModule* module : modules) {
    const semantic::SchemaTree* tree = compilation.schemas.Find(*module);
    if (!tree) continue;
    auto& module_ids = ids[module];
    for (std::size_t index = 0; index < tree->size(); ++index) {
      module_ids.push_back(static_cast<RuntimeSchemaNodeId>(result.nodes_.size()));
      result.nodes_.push_back({});
    }
  }
  for (const ResolvedModule* module : modules) {
    const semantic::SchemaTree* tree = compilation.schemas.Find(*module);
    if (!tree) continue;
    const auto& module_ids = ids.at(module);
    for (std::size_t index = 0; index < tree->size(); ++index) {
      const semantic::SchemaNode& source = tree->Get(static_cast<semantic::SchemaNodeId>(index));
      RuntimeSchemaNode& node = result.nodes_.at(module_ids[index]);
      node.id = module_ids[index];
      if (source.parent) node.parent = module_ids.at(*source.parent);
      node.kind = source.kind;
      node.name = {namespaces[source.name.module], source.name.local_name};
      node.module_name = source.name.module;
      node.config = source.effective_config;
      node.mandatory = source.mandatory;
      node.presence_container = source.presence_container;
      node.supported = source.supported;
      node.nacm_default_deny_all =
          has_nacm_annotation(source, "default-deny-all");
      node.nacm_default_deny_write =
          has_nacm_annotation(source, "default-deny-write");
      if (node.parent) {
        const RuntimeSchemaNode& parent = result.Get(*node.parent);
        node.nacm_default_deny_all = node.nacm_default_deny_all ||
                                     parent.nacm_default_deny_all;
        node.nacm_default_deny_write = node.nacm_default_deny_write ||
                                       parent.nacm_default_deny_write;
      }
      node.min_elements = source.min_elements;
      node.max_elements = source.max_elements;
      node.max_elements_unbounded = source.max_elements_unbounded;
      node.ordered_by_user = source.ordered_by_user;
      node.type = source.type;
      if (source.type) {
        node.require_instance = source.type->require_instance;
        if (source.type->builtin == semantic::BuiltinType::kIdentityRef) {
          node.reference_kind = RuntimeReferenceKind::kIdentityRef;
          for (const auto& base : source.type->resolved_identity_bases) {
            std::vector<semantic::QualifiedSymbolName> identities{
                {base.module, base.name}};
            const auto derived = compilation.identities.identities.DerivedFrom(
                {base.module, base.name});
            identities.insert(identities.end(), derived.begin(), derived.end());
            const auto add_identity = [&](const semantic::QualifiedSymbolName&
                                              identity) {
              const auto namespace_found = namespaces.find(identity.module);
              if (namespace_found != namespaces.end()) {
                node.identity_values.push_back(
                    {namespace_found->second, identity.name});
              }
            };
            for (const auto& identity : identities) add_identity(identity);
            for (const auto& identity : identities) {
              for (const auto& possible_base : identities) {
                if (!compilation.identities.identities.IsDerivedFrom(
                        identity, possible_base))
                  continue;
                const auto identity_namespace =
                    namespaces.find(identity.module);
                const auto base_namespace =
                    namespaces.find(possible_base.module);
                if (identity_namespace != namespaces.end() &&
                    base_namespace != namespaces.end()) {
                  result.identity_derivations_.push_back(
                      {{identity_namespace->second, identity.name},
                       {base_namespace->second, possible_base.name}});
                }
              }
            }
          }
          std::ranges::sort(node.identity_values, {},
                            [](const QualifiedXmlName& value) {
                              return std::pair(value.namespace_uri,
                                               value.local_name);
                            });
          node.identity_values.erase(
              std::ranges::unique(node.identity_values).begin(),
              node.identity_values.end());
        } else if (source.type->builtin == semantic::BuiltinType::kLeafRef) {
          node.reference_kind = RuntimeReferenceKind::kLeafRef;
          const auto target = compilation.leafrefs.Target(
              {module, static_cast<semantic::SchemaNodeId>(index)});
          if (target && ids.contains(target->tree_module)) {
            node.leafref_target = ids.at(target->tree_module).at(target->node);
          }
          for (const auto& predicate : compilation.leafrefs.Predicates(
                   {module, static_cast<semantic::SchemaNodeId>(index)})) {
            if (!ids.contains(predicate.list.tree_module) ||
                !ids.contains(predicate.key.tree_module) ||
                !ids.contains(predicate.source.tree_module)) continue;
            node.leafref_predicates.push_back({
                ids.at(predicate.list.tree_module).at(predicate.list.node),
                ids.at(predicate.key.tree_module).at(predicate.key.node),
                ids.at(predicate.source.tree_module).at(predicate.source.node)});
          }
          node.type = compilation.leafrefs.EffectiveType(
              compilation.schemas,
              {module, static_cast<semantic::SchemaNodeId>(index)});
        } else if (source.type->builtin ==
                   semantic::BuiltinType::kInstanceIdentifier) {
          node.reference_kind = RuntimeReferenceKind::kInstanceIdentifier;
        } else if (source.type->builtin == semantic::BuiltinType::kUnion) {
          std::function<void(const std::shared_ptr<const semantic::ResolvedType>&)>
              collect_references;
          collect_references = [&](const auto& member) {
            if (member->builtin == semantic::BuiltinType::kUnion) {
              for (const auto& nested : member->union_members)
                collect_references(nested);
              return;
            }
            RuntimeReferenceAlternative alternative;
            if (member->builtin == semantic::BuiltinType::kIdentityRef) {
              alternative.kind = RuntimeReferenceKind::kIdentityRef;
              for (const auto& base : member->resolved_identity_bases) {
                const auto add = [&](const semantic::QualifiedSymbolName& identity) {
                  const auto found = namespaces.find(identity.module);
                  if (found != namespaces.end())
                    alternative.identity_values.push_back(
                        {found->second, identity.name});
                };
                add({base.module, base.name});
                for (const auto& identity :
                     compilation.identities.identities.DerivedFrom(
                         {base.module, base.name})) add(identity);
              }
            } else if (member->builtin ==
                       semantic::BuiltinType::kInstanceIdentifier) {
              alternative.kind = RuntimeReferenceKind::kInstanceIdentifier;
              alternative.require_instance = member->require_instance;
            } else if (member->builtin == semantic::BuiltinType::kLeafRef &&
                       member->leafref_path && source.source_module) {
              alternative.kind = RuntimeReferenceKind::kLeafRef;
              alternative.require_instance = member->require_instance;
            }
            if (alternative.kind != RuntimeReferenceKind::kNone) {
              node.union_references.push_back(std::move(alternative));
              if (member->builtin == semantic::BuiltinType::kLeafRef) {
                pending_union_leafrefs.push_back(
                    {node.id, node.union_references.size() - 1,
                     source.source_module.get(), *member->leafref_path});
              }
            }
          };
          for (const auto& member : source.type->union_members)
            collect_references(member);
        }
      }
      node.default_value = source.default_value;
      node.default_values = source.default_values;
      for (semantic::SchemaNodeId child : source.children) node.children.push_back(module_ids.at(child));
      for (semantic::SchemaNodeId key : source.key) node.keys.push_back(module_ids.at(key));
      for (const auto& unique : source.unique) {
        std::vector<RuntimeSchemaNodeId> targets;
        for (semantic::SchemaNodeId target : unique) targets.push_back(module_ids.at(target));
        node.unique.push_back(std::move(targets));
      }
    }
    for (semantic::SchemaNodeId root : tree->roots()) {
      const RuntimeSchemaNode& node = result.Get(module_ids.at(root));
      if (node.supported && (IsDataNode(node.kind) || IsTransparent(node.kind))) result.roots_.push_back(node.id);
    }
  }
  for (const PendingUnionLeafref& pending : pending_union_leafrefs) {
    std::string_view path = TrimView(pending.path);
    const bool absolute = !path.empty() && path.front() == '/';
    if (absolute) path.remove_prefix(1);
    std::optional<RuntimeSchemaNodeId> current = pending.node;
    while (!absolute && path.starts_with("../")) {
      current = current ? result.Get(*current).parent : std::nullopt;
      path.remove_prefix(3);
    }
    bool valid_path = absolute || current.has_value();
    bool first = true;
    while (valid_path && !path.empty()) {
      const std::size_t slash = path.find('/');
      std::string_view segment = path.substr(0, slash);
      if (const std::size_t predicate = segment.find('[');
          predicate != std::string_view::npos) {
        valid_path = false;
        break;
      }
      const std::size_t colon = segment.find(':');
      const std::string_view prefix = colon == std::string_view::npos
                                          ? std::string_view()
                                          : segment.substr(0, colon);
      const std::string_view local = colon == std::string_view::npos
                                         ? segment
                                         : segment.substr(colon + 1);
      std::optional<std::string> namespace_uri;
      if (prefix.empty() || prefix == pending.source->prefix) {
        const std::string module =
            pending.source->belongs_to.value_or(pending.source->name);
        if (namespaces.contains(module)) namespace_uri = namespaces.at(module);
      } else if (const auto imported = pending.source->imports.find(std::string(prefix));
                 imported != pending.source->imports.end()) {
        namespace_uri = imported->second->namespace_uri;
      }
      if (!namespace_uri || !ValidIdentifier(local)) {
        valid_path = false;
        break;
      }
      const QualifiedXmlName name{*namespace_uri, std::string(local)};
      current = absolute && first ? result.FindRoot(name)
                                  : current ? result.FindChild(*current, name)
                                            : std::nullopt;
      if (!current) valid_path = false;
      first = false;
      if (slash == std::string_view::npos) break;
      path.remove_prefix(slash + 1);
    }
    if (valid_path && current) {
      RuntimeReferenceAlternative& alternative =
          result.nodes_.at(pending.node).union_references.at(pending.alternative);
      alternative.leafref_target = current;
      alternative.scalar_type = result.Get(*current).type;
    }
  }
  for (const semantic::ValidatedXPath& constraint :
       compilation.xpath.expressions()) {
    if (!ids.contains(constraint.constrained_node.tree_module) ||
        !ids.contains(constraint.context.tree_module) ||
        constraint.source_module == nullptr) continue;
    RuntimeXPathConstraint lowered;
    lowered.kind = constraint.kind;
    lowered.context_schema =
        ids.at(constraint.context.tree_module).at(constraint.context.node);
    lowered.expression = constraint.expression;
    lowered.error_message = constraint.error_message;
    lowered.error_app_tag = constraint.error_app_tag;
    const ResolvedModule& source = *constraint.source_module;
    const std::string own_module = source.belongs_to.value_or(source.name);
    if (namespaces.contains(own_module)) {
      lowered.namespaces.push_back({source.prefix, namespaces.at(own_module)});
      lowered.namespaces.push_back({"", namespaces.at(own_module)});
    }
    for (const auto& [prefix, imported] : source.imports) {
      lowered.namespaces.push_back({prefix, imported->namespace_uri});
    }
    std::ranges::sort(lowered.namespaces, {}, &XmlNamespaceBinding::prefix);
    result.nodes_
        .at(ids.at(constraint.constrained_node.tree_module)
                .at(constraint.constrained_node.node))
        .xpath_constraints.push_back(std::move(lowered));
  }
  std::ranges::sort(result.identity_derivations_, {}, [](const auto& value) {
    return std::tuple(value.first.namespace_uri, value.first.local_name,
                      value.second.namespace_uri, value.second.local_name);
  });
  result.identity_derivations_.erase(
      std::ranges::unique(result.identity_derivations_).begin(),
      result.identity_derivations_.end());
  return result;
}

std::optional<RuntimeSchema> RuntimeSchemaBuilder::FromYin(
    const pugi::xml_document& yin, ModuleSourceRepository& repository,
    DiagnosticSink& diagnostics) {
  constexpr std::string_view kYinNamespace =
      "urn:ietf:params:xml:ns:yang:yin:1";
  const pugi::xml_node root = yin.document_element();
  if (!root) {
    diagnostics.Report({DiagnosticCode::kInvalidYinInput,
                        DiagnosticSeverity::kError,
                        "YIN document has no root element", {}});
    return std::nullopt;
  }
  std::string resource_error;
  if (!XmlWithinResourceLimits(yin, {}, DefaultResourceLimits(),
                               &resource_error)) {
    diagnostics.Report({DiagnosticCode::kResourceLimitExceeded,
                        DiagnosticSeverity::kError, resource_error, {}});
    return std::nullopt;
  }
  const auto lexical_local = [](std::string_view name) {
    const std::size_t colon = name.find(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
  };
  NamespaceMap root_namespaces = ExtendNamespaces(root, {});
  const auto expanded_root = ExpandName(root, root_namespaces);
  if (!expanded_root || expanded_root->namespace_uri != kYinNamespace ||
      (expanded_root->local_name != "module" &&
       expanded_root->local_name != "submodule")) {
    diagnostics.Report({DiagnosticCode::kInvalidYinInput,
                        DiagnosticSeverity::kError,
                        "YIN root must be module or submodule in the YIN namespace",
                        {}});
    return std::nullopt;
  }
  std::optional<std::string> nacm_prefix;
  for (const pugi::xml_node child : root.children()) {
    if (lexical_local(child.name()) != "import" ||
        std::string_view(child.attribute("module").value()) !=
            "ietf-netconf-acm") {
      continue;
    }
    for (const pugi::xml_node import_child : child.children()) {
      if (lexical_local(import_child.name()) == "prefix" &&
          import_child.attribute("value")) {
        nacm_prefix = import_child.attribute("value").value();
      }
    }
  }
  const auto quote = [](std::string_view value) {
    std::string result = "\"";
    for (char character : value) {
      if (character == '\\' || character == '"') result.push_back('\\');
      if (character == '\n') result += "\\n";
      else if (character == '\t') result += "\\t";
      else if (character != '\r') result.push_back(character);
    }
    result.push_back('"');
    return result;
  };
  bool valid = true;
  std::function<std::string(const pugi::xml_node&)> convert;
  convert = [&](const pugi::xml_node& element) {
    const std::string_view lexical_name = element.name();
    if (lexical_name.find(':') != std::string_view::npos) {
      const NamespaceMap namespaces = ExtendNamespaces(element, root_namespaces);
      const auto expanded = ExpandName(element, namespaces);
      constexpr std::string_view kNacmNamespace =
          "urn:ietf:params:xml:ns:yang:ietf-netconf-acm";
      if (!expanded || expanded->namespace_uri != kNacmNamespace ||
          (expanded->local_name != "default-deny-all" &&
           expanded->local_name != "default-deny-write") || !nacm_prefix) {
        valid = false;
        diagnostics.Report({DiagnosticCode::kInvalidYinInput,
                            DiagnosticSeverity::kError,
                            "YIN extension statement is not supported by this adapter",
                            {}});
        return std::string();
      }
      return *nacm_prefix + ":" + expanded->local_name + ";\n";
    }
    const std::string keyword(lexical_local(lexical_name));
    const auto spec = SchemaRegistry::Find(keyword);
    if (!spec) {
      valid = false;
      diagnostics.Report({DiagnosticCode::kInvalidYinInput,
                          DiagnosticSeverity::kError,
                          "unknown YIN statement '" + keyword + "'", {}});
      return std::string();
    }
    std::string output = keyword;
    std::optional<pugi::xml_node> argument_element;
    if (spec->argument == ArgumentRequirement::kRequired) {
      std::optional<std::string> argument;
      if (spec->yin_element) {
        for (const pugi::xml_node child : element.children()) {
          if (child.type() == pugi::node_element &&
              lexical_local(child.name()) == spec->yin_argument) {
            argument_element = child;
            argument = child.text().get();
            break;
          }
        }
      } else if (const pugi::xml_attribute attribute =
                     element.attribute(std::string(spec->yin_argument).c_str())) {
        argument = attribute.value();
      }
      if (!argument) {
        valid = false;
        diagnostics.Report({DiagnosticCode::kInvalidYinInput,
                            DiagnosticSeverity::kError,
                            "YIN statement '" + keyword + "' is missing argument '" +
                                std::string(spec->yin_argument) + "'",
                            {}});
      } else {
        output += " " + quote(*argument);
      }
    }
    std::vector<pugi::xml_node> children;
    for (const pugi::xml_node child : element.children()) {
      if (child.type() == pugi::node_element &&
          (!argument_element || child != *argument_element)) children.push_back(child);
    }
    if (children.empty()) return output + ";\n";
    output += " {\n";
    for (const pugi::xml_node child : children) output += convert(child);
    output += "}\n";
    return output;
  };
  const std::string yang_source = convert(root);
  if (!valid) return std::nullopt;
  const std::string source_name =
      std::string(root.attribute("name").value()) + ".yin.yang";
  auto source = SourceFile::Create(source_name, yang_source, diagnostics);
  if (!source) return std::nullopt;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(std::move(source));
  if (!compilation) return std::nullopt;
  return FromCompilation(*compilation);
}

const ConfigNode& ConfigDocument::Get(ConfigNodeId id) const { return nodes_.at(id); }

std::string ConfigDocument::ToXml(bool netconf_wrapper) const {
  pugi::xml_document xml;
  pugi::xml_node parent = xml;
  std::string parent_namespace;
  if (netconf_wrapper) {
    parent = xml.append_child("config");
    parent.append_attribute("xmlns") = kNetconfNamespace.data();
    parent_namespace = std::string(kNetconfNamespace);
  }
  std::function<void(ConfigNodeId, pugi::xml_node, std::string_view)> append;
  append = [&](ConfigNodeId id, pugi::xml_node xml_parent,
               std::string_view inherited_namespace) {
    const ConfigNode& node = Get(id);
    pugi::xml_node element = xml_parent.append_child(node.name.local_name.c_str());
    if (node.name.namespace_uri != inherited_namespace)
      element.append_attribute("xmlns") = node.name.namespace_uri.c_str();
    for (const XmlNamespaceBinding& binding : node.value_namespaces) {
      if (binding.prefix.empty() || binding.prefix == "xml" ||
          binding.prefix == "xmlns" || !node.value ||
          node.value->find(binding.prefix + ":") == std::string::npos)
        continue;
      const std::string attribute = "xmlns:" + binding.prefix;
      element.append_attribute(attribute.c_str()) = binding.namespace_uri.c_str();
    }
    if (node.value) element.text().set(node.value->c_str());
    for (ConfigNodeId child : node.children)
      append(child, element, node.name.namespace_uri);
  };
  for (ConfigNodeId root : roots_) append(root, parent, parent_namespace);
  std::ostringstream output;
  xml.print(output, "  ", pugi::format_default, pugi::encoding_utf8);
  return output.str();
}

ConfigDocument ConfigDocument::WithChildCoverage(ConfigNodeId node,
                                                 Coverage coverage) const {
  ConfigDocument result = *this;
  result.nodes_.at(node).child_coverage = coverage;
  return result;
}

ConfigDocument ConfigDocument::WithCollectionCoverage(
    std::optional<ConfigNodeId> parent, RuntimeSchemaNodeId child_schema,
    Coverage coverage) const {
  ConfigDocument result = *this;
  const auto found = std::ranges::find_if(
      result.collection_coverage_, [&](const CollectionCoverageEntry& entry) {
        return entry.parent == parent && entry.schema == child_schema;
      });
  if (found == result.collection_coverage_.end())
    result.collection_coverage_.push_back({parent, child_schema, coverage});
  else
    found->coverage = coverage;
  return result;
}

std::optional<Coverage> ConfigDocument::CollectionCoverage(
    std::optional<ConfigNodeId> parent,
    RuntimeSchemaNodeId child_schema) const {
  const auto found = std::ranges::find_if(
      collection_coverage_, [&](const CollectionCoverageEntry& entry) {
        return entry.parent == parent && entry.schema == child_schema;
      });
  return found == collection_coverage_.end()
             ? std::nullopt
             : std::optional<Coverage>(found->coverage);
}

const EffectiveNode& EffectiveDataView::Get(EffectiveNodeId id) const {
  return nodes_.at(id);
}

EffectiveDataView EffectiveDataView::Build(const RuntimeSchema& schema,
                                           const ConfigDocument& document,
                                           bool allow_state_data) {
  EffectiveDataView result;
  const auto schema_contains = [&](RuntimeSchemaNodeId ancestor,
                                   RuntimeSchemaNodeId target) {
    std::function<bool(RuntimeSchemaNodeId)> visit = [&](RuntimeSchemaNodeId id) {
      return id == target ||
             std::ranges::any_of(schema.Get(id).children, visit);
    };
    return visit(ancestor);
  };
  const auto add = [&](RuntimeSchemaNodeId schema_id,
                       std::optional<EffectiveNodeId> parent,
                       std::optional<ConfigNodeId> explicit_node,
                       std::optional<std::string> value) {
    const EffectiveNodeId id =
        static_cast<EffectiveNodeId>(result.nodes_.size());
    result.nodes_.push_back(
        {id, schema_id, parent, explicit_node, std::move(value), {}});
    if (parent) result.nodes_.at(*parent).children.push_back(id);
    else result.roots_.push_back(id);
    return id;
  };
  std::function<void(const std::vector<RuntimeSchemaNodeId>&,
                     const std::vector<ConfigNodeId>&,
                     std::optional<EffectiveNodeId>)>
      populate;
  populate = [&](const std::vector<RuntimeSchemaNodeId>& schema_children,
                 const std::vector<ConfigNodeId>& config_children,
                 std::optional<EffectiveNodeId> parent) {
    for (RuntimeSchemaNodeId schema_id : schema_children) {
      const RuntimeSchemaNode& schema_node = schema.Get(schema_id);
      if (!schema_node.supported ||
          (!schema_node.config && !allow_state_data))
        continue;
      if (schema_node.kind == SchemaNodeKind::kChoice) {
        std::optional<RuntimeSchemaNodeId> selected_case;
        for (RuntimeSchemaNodeId candidate : schema_node.children) {
          if (std::ranges::any_of(config_children, [&](ConfigNodeId child) {
                return schema_contains(candidate, document.Get(child).schema);
              })) {
            selected_case = candidate;
            break;
          }
        }
        if (!selected_case && schema_node.default_value) {
          const auto found = std::ranges::find_if(
              schema_node.children, [&](RuntimeSchemaNodeId candidate) {
                return schema.Get(candidate).name.local_name ==
                       *schema_node.default_value;
              });
          if (found != schema_node.children.end()) selected_case = *found;
        }
        if (selected_case) {
          populate(schema.Get(*selected_case).children, config_children, parent);
        }
        continue;
      }
      if (schema_node.kind == SchemaNodeKind::kCase) {
        populate(schema_node.children, config_children, parent);
        continue;
      }
      std::vector<ConfigNodeId> explicit_instances;
      for (ConfigNodeId child : config_children) {
        if (document.Get(child).schema == schema_id) explicit_instances.push_back(child);
      }
      for (ConfigNodeId explicit_id : explicit_instances) {
        const ConfigNode& explicit_node = document.Get(explicit_id);
        const EffectiveNodeId effective_id =
            add(schema_id, parent, explicit_id, explicit_node.value);
        populate(schema_node.children, explicit_node.children, effective_id);
      }
      if (!explicit_instances.empty()) continue;
      if (schema_node.kind == SchemaNodeKind::kLeaf &&
          schema_node.default_value) {
        add(schema_id, parent, std::nullopt, schema_node.default_value);
      } else if (schema_node.kind == SchemaNodeKind::kLeafList) {
        for (const std::string& value : schema_node.default_values) {
          add(schema_id, parent, std::nullopt, value);
        }
      } else if (schema_node.kind == SchemaNodeKind::kContainer &&
                 !schema_node.presence_container) {
        const std::size_t original_size = result.nodes_.size();
        const std::size_t original_roots = result.roots_.size();
        const EffectiveNodeId container =
            add(schema_id, parent, std::nullopt, std::nullopt);
        populate(schema_node.children, {}, container);
        if (result.Get(container).children.empty()) {
          if (parent) result.nodes_.at(*parent).children.pop_back();
          else result.roots_.resize(original_roots);
          result.nodes_.resize(original_size);
        }
      }
    }
  };
  populate(schema.roots(), document.roots(), std::nullopt);
  return result;
}

ConfigParseResult ParseDatastoreXml(const RuntimeSchema& schema, std::string_view xml,
                                    const ConfigParseOptions& options) {
  pugi::xml_document document;
  const UntrustedXmlResult parsed = ParseUntrustedXml(xml, &document);
  if (!parsed.ok) {
    ConfigParseResult result;
    result.findings.push_back(Finding(
        parsed.resource_limit ? ValidationCode::kResourceLimitExceeded
                              : ValidationCode::kMalformedXml,
        FindingState::kInvalid, parsed.message, "",
        parsed.resource_limit ? "too-big" : "malformed-message"));
    return result;
  }
  const pugi::xml_node root = document.document_element();
  return XmlBinder(schema, xml, options).Bind(root);
}

ValidationResult ConfigValidator::Validate(const ValidationRequest& request) const {
  if (request.scope == ValidationScope::kPartialWithContext &&
      request.context != nullptr) {
    ConfigDocument composed = *request.context;
    const auto same_instance = [&](ConfigNodeId left, const ConfigDocument& right_doc,
                                   ConfigNodeId right) {
      const ConfigNode& left_node = composed.Get(left);
      const ConfigNode& right_node = right_doc.Get(right);
      if (left_node.schema != right_node.schema) return false;
      const RuntimeSchemaNode& schema = request.schema.Get(left_node.schema);
      if (schema.kind == SchemaNodeKind::kLeafList)
        return left_node.value == right_node.value;
      if (schema.kind != SchemaNodeKind::kList) return true;
      for (RuntimeSchemaNodeId key : schema.keys) {
        const auto value = [&](const ConfigDocument& document, const ConfigNode& node) {
          const auto child = std::ranges::find_if(node.children, [&](ConfigNodeId id) {
            return document.Get(id).schema == key;
          });
          return child == node.children.end() ? std::optional<std::string>()
                                              : document.Get(*child).value;
        };
        if (value(composed, left_node) != value(right_doc, right_node)) return false;
      }
      return true;
    };
    std::function<ConfigNodeId(const ConfigDocument&, ConfigNodeId,
                               std::optional<ConfigNodeId>)>
        clone_subtree;
    clone_subtree = [&](const ConfigDocument& source, ConfigNodeId source_id,
                        std::optional<ConfigNodeId> parent) {
      ConfigNode clone = source.Get(source_id);
      clone.id = static_cast<ConfigNodeId>(composed.nodes_.size());
      clone.parent = parent;
      clone.child_coverage = Coverage::kComplete;
      clone.children.clear();
      const ConfigNodeId clone_id = clone.id;
      composed.nodes_.push_back(std::move(clone));
      for (ConfigNodeId child : source.Get(source_id).children) {
        const ConfigNodeId child_id = clone_subtree(source, child, clone_id);
        composed.nodes_.at(clone_id).children.push_back(child_id);
      }
      return clone_id;
    };
    std::function<void(ConfigNodeId, const ConfigDocument&, ConfigNodeId)> overlay;
    overlay = [&](ConfigNodeId target_id, const ConfigDocument& fragment,
                  ConfigNodeId fragment_id) {
      const ConfigNode& update = fragment.Get(fragment_id);
      if (update.value) {
        composed.nodes_.at(target_id).value = update.value;
        composed.nodes_.at(target_id).value_namespaces =
            update.value_namespaces;
      }
      for (ConfigNodeId update_child : update.children) {
        const auto& children = composed.nodes_.at(target_id).children;
        const auto match = std::ranges::find_if(
            children, [&](ConfigNodeId child) {
              return same_instance(child, fragment, update_child);
            });
        const std::optional<ConfigNodeId> matched =
            match == children.end() ? std::nullopt
                                    : std::optional<ConfigNodeId>(*match);
        if (!matched) {
          const ConfigNodeId clone_id =
              clone_subtree(fragment, update_child, target_id);
          composed.nodes_.at(target_id).children.push_back(clone_id);
        } else {
          overlay(*matched, fragment, update_child);
        }
      }
      composed.nodes_.at(target_id).child_coverage = Coverage::kComplete;
    };
    for (ConfigNodeId fragment_root : request.document.roots()) {
      const RuntimeSchemaNode& fragment_schema =
          request.schema.Get(request.document.Get(fragment_root).schema);
      std::optional<ConfigNodeId> attachment;
      if (fragment_schema.parent) {
        if (request.context_attachment) {
          attachment = request.context_attachment;
        } else {
          for (ConfigNodeId candidate = 0; candidate < composed.size(); ++candidate) {
            if (composed.Get(candidate).schema == *fragment_schema.parent) {
              if (attachment) {
                ValidationResult ambiguous;
                ambiguous.findings.push_back(Finding(
                    ValidationCode::kContextRequired, FindingState::kInvalid,
                    "context attachment is ambiguous", "", "invalid-value"));
                return ambiguous;
              }
              attachment = candidate;
            }
          }
        }
        if (!attachment) {
          ValidationResult missing;
          missing.findings.push_back(Finding(
              ValidationCode::kContextRequired, FindingState::kInvalid,
              "context attachment does not exist", "", "data-missing"));
          return missing;
        }
      }
      const auto& siblings = attachment
                                 ? composed.nodes_.at(*attachment).children
                                 : composed.roots_;
      const auto match = std::ranges::find_if(
          siblings, [&](ConfigNodeId candidate) {
            return same_instance(candidate, request.document, fragment_root);
          });
      const std::optional<ConfigNodeId> matched =
          match == siblings.end() ? std::nullopt
                                  : std::optional<ConfigNodeId>(*match);
      if (!matched) {
        const ConfigNodeId clone_id =
            clone_subtree(request.document, fragment_root, attachment);
        if (attachment) {
          composed.nodes_.at(*attachment).children.push_back(clone_id);
        } else {
          composed.roots_.push_back(clone_id);
        }
      } else {
        overlay(*matched, request.document, fragment_root);
      }
    }
    return Validate(
        {request.schema, composed,
         request.allow_state_data ? ValidationScope::kPartialStandalone
                                  : ValidationScope::kComplete,
         nullptr, std::nullopt, request.allow_state_data,
         request.allow_state_data});
  }
  ValidationResult result;
  const bool complete = request.scope == ValidationScope::kComplete;
  const EffectiveDataView effective =
      EffectiveDataView::Build(request.schema, request.document,
                               request.allow_state_data);
  std::vector<std::optional<EffectiveNodeId>> effective_for_explicit(
      request.document.size());
  for (EffectiveNodeId id = 0; id < effective.size(); ++id) {
    if (effective.Get(id).explicit_node)
      effective_for_explicit.at(*effective.Get(id).explicit_node) = id;
  }
  result.complete = complete;
  if (request.scope == ValidationScope::kPartialWithContext && request.context == nullptr) {
    result.findings.push_back(Finding(ValidationCode::kContextRequired, FindingState::kInvalid,
        "context-assisted partial validation requires a context tree", "", "missing-element"));
  }
  const auto omission_state = [&](Coverage coverage) {
    return complete && coverage == Coverage::kComplete ? FindingState::kInvalid
                                                       : FindingState::kIndeterminate;
  };
  std::vector<std::string> paths(request.document.size());
  std::function<void(const std::vector<RuntimeSchemaNodeId>&, const std::vector<ConfigNodeId>&,
                     Coverage, std::string_view,
                     std::optional<ConfigNodeId>)> validate_children;
  validate_children = [&](const std::vector<RuntimeSchemaNodeId>& schema_children,
                          const std::vector<ConfigNodeId>& config_children,
                          Coverage coverage, std::string_view parent_path,
                          std::optional<ConfigNodeId> data_parent) {
    std::vector<RuntimeSchemaNodeId> visible;
    CollectVisible(request.schema, schema_children, &visible);
    std::map<RuntimeSchemaNodeId, std::vector<ConfigNodeId>> instances;
    for (ConfigNodeId child : config_children) instances[request.document.Get(child).schema].push_back(child);
    for (RuntimeSchemaNodeId schema_id : visible) {
      const RuntimeSchemaNode& node = request.schema.Get(schema_id);
      if (!node.config && !request.allow_state_data) continue;
      const Coverage collection_coverage =
          request.document.CollectionCoverage(data_parent, schema_id)
              .value_or(coverage);
      const std::size_t count = instances[schema_id].size();
      const std::string path = std::string(parent_path) + PathComponent(node.name);
      if (node.kind != SchemaNodeKind::kList && node.kind != SchemaNodeKind::kLeafList && count > 1)
        result.findings.push_back(Finding(ValidationCode::kDuplicateNode, FindingState::kInvalid,
            "a singleton data node occurs more than once", path, "data-exists",
            node.module_name));
      const std::uint64_t numeric_count = static_cast<std::uint64_t>(count);
      if (node.min_elements && numeric_count < *node.min_elements)
        result.findings.push_back(Finding(ValidationCode::kElementCount, omission_state(collection_coverage),
            "fewer entries than min-elements", path, "too-few-elements",
            node.module_name));
      if (node.max_elements && numeric_count > *node.max_elements)
        result.findings.push_back(Finding(ValidationCode::kElementCount, FindingState::kInvalid,
            "more entries than max-elements", path, "too-many-elements",
            node.module_name));
      bool active_case = true;
      if (node.parent &&
          request.schema.Get(*node.parent).kind == SchemaNodeKind::kCase) {
        const RuntimeSchemaNode& case_node = request.schema.Get(*node.parent);
        if (case_node.parent) {
          active_case = std::ranges::any_of(
              config_children, [&](ConfigNodeId child) {
                return DirectCaseFor(request.schema, *case_node.parent,
                                     request.document.Get(child).schema) ==
                       node.parent;
              });
        }
      }
      if (node.mandatory && count == 0 && active_case)
        result.findings.push_back(Finding(ValidationCode::kMissingMandatoryNode,
            omission_state(collection_coverage), "mandatory data node is absent", path,
            "missing-element", node.module_name));
    }
    for (RuntimeSchemaNodeId candidate : schema_children) {
      const RuntimeSchemaNode& choice = request.schema.Get(candidate);
      if (choice.kind != SchemaNodeKind::kChoice || !choice.supported) continue;
      std::set<RuntimeSchemaNodeId> active_cases;
      for (ConfigNodeId child : config_children) {
        if (const auto active = DirectCaseFor(request.schema, candidate,
                                              request.document.Get(child).schema)) active_cases.insert(*active);
      }
      const std::string path = std::string(parent_path) + PathComponent(choice.name);
      if (active_cases.size() > 1)
        result.findings.push_back(Finding(ValidationCode::kChoiceConflict, FindingState::kInvalid,
            "data nodes from more than one case of a choice are present", path,
            "bad-element", choice.module_name));
      if (choice.mandatory && active_cases.empty())
        result.findings.push_back(Finding(ValidationCode::kMissingMandatoryChoice,
            omission_state(coverage), "no case of a mandatory choice is present", path,
            "missing-choice", choice.module_name));
    }
  };
  std::function<void(ConfigNodeId)> validate_node = [&](ConfigNodeId id) {
    const ConfigNode& config = request.document.Get(id);
    const RuntimeSchemaNode& schema = request.schema.Get(config.schema);
    const std::string parent_path = config.parent ? paths.at(*config.parent) : std::string();
    paths.at(id) = parent_path + PathComponent(config.name);
    if (!schema.config && !request.allow_state_data)
      result.findings.push_back(Finding(ValidationCode::kStateDataInConfiguration,
          FindingState::kInvalid, "configuration content contains a config false data node",
          paths.at(id), "operation-not-supported"));
    if (IsScalar(schema.kind) && schema.type && config.value &&
        schema.reference_kind != RuntimeReferenceKind::kIdentityRef &&
        schema.reference_kind != RuntimeReferenceKind::kInstanceIdentifier &&
        schema.union_references.empty()) {
      const bool empty_ok = schema.type->builtin == semantic::BuiltinType::kEmpty && config.value->empty();
      if (!empty_ok && !semantic::ValueMatchesType(*schema.type, *config.value))
        result.findings.push_back(Finding(ValidationCode::kInvalidValue, FindingState::kInvalid,
            "value is outside the YANG type's value space", paths.at(id), "invalid-value"));
    }
    if (config.value && schema.type && !schema.union_references.empty() &&
        !ValueMatchesOrdinaryUnionMember(*schema.type, *config.value)) {
      bool accepted = false;
      bool missing_required_instance = false;
      bool missing_required_config_instance = false;
      for (const RuntimeReferenceAlternative& alternative :
           schema.union_references) {
        if (alternative.kind == RuntimeReferenceKind::kIdentityRef) {
          const auto identity = ParseValueQName(config, *config.value);
          accepted = identity &&
              std::ranges::find(alternative.identity_values, *identity) !=
                  alternative.identity_values.end();
        } else if (alternative.kind ==
                   RuntimeReferenceKind::kInstanceIdentifier) {
          const InstanceLookupResult lookup = ResolveInstanceIdentifier(
              request.schema, request.document, config, *config.value);
          accepted = lookup.syntax_valid &&
                     (lookup.found || !alternative.require_instance);
          missing_required_instance = missing_required_instance ||
              (lookup.syntax_valid && !lookup.found &&
               alternative.require_instance);
        } else if (alternative.kind == RuntimeReferenceKind::kLeafRef &&
                   alternative.leafref_target && alternative.scalar_type &&
                   semantic::ValueMatchesType(*alternative.scalar_type,
                                              *config.value)) {
          if (!alternative.require_instance) {
            accepted = true;
          } else {
            const RuntimeSchemaNode& target =
                request.schema.Get(*alternative.leafref_target);
            std::vector<ConfigNodeId> candidates;
            if (config.parent && schema.parent == target.parent) {
              for (ConfigNodeId sibling :
                   request.document.Get(*config.parent).children) {
                if (request.document.Get(sibling).schema ==
                    *alternative.leafref_target) candidates.push_back(sibling);
              }
            } else {
              for (ConfigNodeId candidate = 0;
                   candidate < request.document.size(); ++candidate) {
                if (request.document.Get(candidate).schema ==
                    *alternative.leafref_target) candidates.push_back(candidate);
              }
            }
            accepted = std::ranges::any_of(candidates, [&](ConfigNodeId candidate) {
              return request.document.Get(candidate).value == config.value;
            });
            missing_required_instance = missing_required_instance || !accepted;
            missing_required_config_instance =
                missing_required_config_instance || (!accepted && target.config);
          }
        }
        if (accepted) break;
      }
      if (!accepted) {
        ValidationFinding finding = Finding(
            missing_required_instance ? ValidationCode::kUnresolvedReference
                                      : ValidationCode::kInvalidValue,
            missing_required_instance
                ? ((complete ||
                    (request.complete_config_context &&
                     missing_required_config_instance))
                       ? FindingState::kInvalid
                       : FindingState::kIndeterminate)
                : FindingState::kInvalid,
            missing_required_instance
                ? "union reference alternative has no required target instance"
                : "value does not match any union member",
            paths.at(id),
            missing_required_instance ? "data-missing" : "invalid-value");
        if (missing_required_instance)
          finding.netconf_error_app_tag = "instance-required";
        result.findings.push_back(std::move(finding));
      }
    }
    if (config.value && schema.reference_kind == RuntimeReferenceKind::kIdentityRef) {
      const auto identity = ParseValueQName(config, *config.value);
      if (!identity || std::ranges::find(schema.identity_values, *identity) ==
                           schema.identity_values.end()) {
        result.findings.push_back(Finding(
            ValidationCode::kInvalidValue, FindingState::kInvalid,
            "identityref does not name the base identity or a derived identity",
            paths.at(id), "invalid-value"));
      }
    }
    if (config.value && schema.reference_kind == RuntimeReferenceKind::kLeafRef &&
        schema.leafref_target && schema.require_instance) {
      const RuntimeSchemaNode& target = request.schema.Get(*schema.leafref_target);
      std::vector<ConfigNodeId> candidates;
      if (config.parent && schema.parent == target.parent) {
        for (ConfigNodeId sibling : request.document.Get(*config.parent).children) {
          if (request.document.Get(sibling).schema == *schema.leafref_target)
            candidates.push_back(sibling);
        }
      } else {
        for (ConfigNodeId candidate = 0; candidate < request.document.size(); ++candidate) {
          if (request.document.Get(candidate).schema == *schema.leafref_target)
            candidates.push_back(candidate);
        }
      }
      const bool found = std::ranges::any_of(candidates, [&](ConfigNodeId candidate) {
        if (request.document.Get(candidate).value != config.value) return false;
        for (const RuntimeLeafrefPredicate& predicate : schema.leafref_predicates) {
          const auto source_node = FindRelatedConfigNode(
              request.document, id, predicate.source);
          const auto list_entry = FindConfigAncestor(
              request.document, candidate, predicate.list);
          if (!source_node || !list_entry || !request.document.Get(*source_node).value)
            return false;
          const auto key_node = std::ranges::find_if(
              request.document.Get(*list_entry).children,
              [&](ConfigNodeId child) {
                return request.document.Get(child).schema == predicate.key;
              });
          if (key_node == request.document.Get(*list_entry).children.end() ||
              request.document.Get(*key_node).value !=
                  request.document.Get(*source_node).value) return false;
        }
        return true;
      });
      if (!found) {
        ValidationFinding finding = Finding(
            ValidationCode::kUnresolvedReference,
            (complete ||
             (request.complete_config_context && target.config))
                ? FindingState::kInvalid
                : FindingState::kIndeterminate,
            "leafref value has no matching target instance", paths.at(id),
            "data-missing");
        finding.netconf_error_app_tag = "instance-required";
        result.findings.push_back(std::move(finding));
      }
    }
    if (config.value &&
        schema.reference_kind == RuntimeReferenceKind::kInstanceIdentifier) {
      const InstanceLookupResult lookup =
          ResolveInstanceIdentifier(request.schema, request.document, config,
                                    *config.value);
      if (!lookup.syntax_valid) {
        result.findings.push_back(Finding(
            ValidationCode::kInvalidValue, FindingState::kInvalid,
            "instance-identifier has invalid XML lexical syntax", paths.at(id),
            "invalid-value"));
      } else if (!lookup.found && schema.require_instance) {
        ValidationFinding finding = Finding(
            ValidationCode::kUnresolvedReference,
            complete ? FindingState::kInvalid : FindingState::kIndeterminate,
            "instance-identifier does not select an existing data node",
            paths.at(id), "data-missing");
        finding.netconf_error_app_tag = "instance-required";
        result.findings.push_back(std::move(finding));
      }
    }
    if (effective_for_explicit.at(id)) {
      const EffectiveNodeId constrained = *effective_for_explicit.at(id);
      for (const RuntimeXPathConstraint& constraint : schema.xpath_constraints) {
        const auto context = FindRelatedEffectiveNode(
            effective, constrained, constraint.context_schema);
        if (!context) {
          result.findings.push_back(Finding(
              ValidationCode::kXPathIndeterminate, FindingState::kIndeterminate,
              "XPath evaluation context is unavailable", paths.at(id),
              "operation-failed"));
          continue;
        }
        const RuntimeXPathEvaluator evaluator(request.schema, effective,
                                              request.document, constraint,
                                              complete, constrained);
        const XPathRuntimeValue value =
            evaluator.Evaluate(constraint.expression, *context);
        if (value.indeterminate || value.kind == XPathRuntimeKind::kInvalid) {
          result.findings.push_back(Finding(
              ValidationCode::kXPathIndeterminate, FindingState::kIndeterminate,
              "XPath constraint cannot be decided from the available data",
              paths.at(id), "operation-failed"));
        } else if (!evaluator.Evaluate("boolean(" + constraint.expression + ")",
                                       *context).boolean) {
          const bool must = constraint.kind == semantic::XPathConstraintKind::kMust;
          ValidationFinding finding = Finding(
              must ? ValidationCode::kMustViolation
                   : ValidationCode::kWhenViolation,
              FindingState::kInvalid,
              constraint.error_message.value_or(
                  must ? "must constraint evaluates to false"
                       : "when constraint evaluates to false"),
              paths.at(id), "operation-failed");
          finding.netconf_error_app_tag = constraint.error_app_tag.value_or(
              must ? "must-violation" : "when-violation");
          result.findings.push_back(std::move(finding));
        }
      }
    }
    if (schema.kind == SchemaNodeKind::kList) {
      for (RuntimeSchemaNodeId key : schema.keys) {
        const bool found = std::ranges::any_of(config.children, [&](ConfigNodeId child) {
          return request.document.Get(child).schema == key;
        });
        if (!found) result.findings.push_back(Finding(ValidationCode::kMissingKey,
            FindingState::kInvalid, "list entry is missing a key leaf", paths.at(id),
            "missing-element", request.schema.Get(key).module_name));
      }
    }
    validate_children(schema.children, config.children, config.child_coverage,
                      paths.at(id), id);
    for (ConfigNodeId child : config.children) validate_node(child);
  };
  validate_children(request.schema.roots(), request.document.roots(),
                    complete ? Coverage::kComplete : Coverage::kSelected, "",
                    std::nullopt);
  for (ConfigNodeId root : request.document.roots()) validate_node(root);
  std::map<std::pair<std::optional<ConfigNodeId>, RuntimeSchemaNodeId>,
           std::set<std::vector<std::string>>> list_keys;
  for (ConfigNodeId id = 0; id < request.document.size(); ++id) {
    const ConfigNode& config = request.document.Get(id);
    const RuntimeSchemaNode& schema = request.schema.Get(config.schema);
    if (schema.kind != SchemaNodeKind::kList || schema.keys.empty()) continue;
    std::vector<std::string> values;
    bool complete_key = true;
    for (RuntimeSchemaNodeId key : schema.keys) {
      const auto child = std::ranges::find_if(config.children, [&](ConfigNodeId candidate) {
        return request.document.Get(candidate).schema == key;
      });
      if (child == config.children.end() || !request.document.Get(*child).value) {
        complete_key = false;
        break;
      }
      values.push_back(*request.document.Get(*child).value);
    }
    if (complete_key && !list_keys[{config.parent, config.schema}].insert(values).second)
      result.findings.push_back(Finding(ValidationCode::kDuplicateListKey, FindingState::kInvalid,
          "two list entries have the same key value", paths.at(id), "data-exists"));
  }
  using ListGroup = std::pair<std::optional<ConfigNodeId>, RuntimeSchemaNodeId>;
  std::map<ListGroup, std::vector<ConfigNodeId>> list_entries;
  for (ConfigNodeId id = 0; id < request.document.size(); ++id) {
    const ConfigNode& node = request.document.Get(id);
    if (request.schema.Get(node.schema).kind == SchemaNodeKind::kList) {
      list_entries[{node.parent, node.schema}].push_back(id);
    }
  }
  for (const auto& [group, entries] : list_entries) {
    const RuntimeSchemaNode& list = request.schema.Get(group.second);
    for (const auto& unique : list.unique) {
      std::map<std::vector<std::string>, ConfigNodeId> seen;
      for (ConfigNodeId entry : entries) {
        std::vector<std::string> values;
        bool participates = true;
        for (RuntimeSchemaNodeId target : unique) {
          const auto value = UniqueComponentValue(request.schema, request.document,
                                                  entry, target);
          if (!value) {
            participates = false;
            break;
          }
          values.push_back(*value);
        }
        if (!participates) continue;
        if (!seen.emplace(values, entry).second) {
          ValidationFinding finding = Finding(
              ValidationCode::kUniqueViolation, FindingState::kInvalid,
              "list entries have identical values for a unique constraint",
              paths.at(entry), "operation-failed");
          finding.netconf_error_app_tag = "data-not-unique";
          result.findings.push_back(std::move(finding));
        }
      }
      if (!complete) {
        ValidationFinding finding = Finding(
            ValidationCode::kUniqueViolation, FindingState::kIndeterminate,
            "omitted list entries may affect a unique constraint",
            (group.first ? paths.at(*group.first) : std::string()) +
                PathComponent(list.name),
            "operation-failed");
        finding.netconf_error_app_tag = "data-not-unique";
        result.findings.push_back(std::move(finding));
      }
    }
  }
  for (ValidationFinding& finding : result.findings) {
    std::size_t best_length = 0;
    for (ConfigNodeId id = 0; id < request.document.size(); ++id) {
      const std::string& candidate_path = paths.at(id);
      if ((finding.instance_path == candidate_path ||
           finding.instance_path.starts_with(candidate_path + "/")) &&
          candidate_path.size() >= best_length) {
        if (!finding.source_range)
          finding.source_range = request.document.Get(id).source_range;
        finding.config_node = id;
        best_length = candidate_path.size();
      }
    }
    if (finding.module_name.empty() && finding.config_node)
      finding.module_name = request.schema
                                .Get(request.document.Get(*finding.config_node)
                                         .schema)
                                .module_name;
  }
  std::ranges::sort(result.findings, [](const ValidationFinding& left,
                                        const ValidationFinding& right) {
    return std::tuple(left.instance_path, static_cast<int>(left.code),
                      static_cast<int>(left.state), left.message) <
           std::tuple(right.instance_path, static_cast<int>(right.code),
                      static_cast<int>(right.state), right.message);
  });
  result.valid = std::ranges::none_of(result.findings, [](const ValidationFinding& finding) {
    return finding.state == FindingState::kInvalid;
  });
  if (std::ranges::any_of(result.findings, [](const ValidationFinding& finding) {
        return finding.state == FindingState::kIndeterminate;
      })) result.complete = false;
  return result;
}
}  // namespace yang::config
