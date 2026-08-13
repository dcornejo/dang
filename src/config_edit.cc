// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/config_edit.h"

#include "yang/resource_limits.h"

#include <algorithm>
#include <functional>
#include <ranges>
#include <sstream>
#include <utility>

#include <pugixml.hpp>

namespace yang::config {
namespace {
constexpr std::string_view kNetconfNamespace =
    "urn:ietf:params:xml:ns:netconf:base:1.0";
constexpr std::string_view kYangNamespace =
    "urn:ietf:params:xml:ns:yang:1";

ValidationFinding Error(ValidationCode code, std::string message,
                        std::string path, std::string tag) {
  ValidationFinding result;
  result.code = code;
  result.state = FindingState::kInvalid;
  result.message = std::move(message);
  result.instance_path = std::move(path);
  result.netconf_error_tag = std::move(tag);
  return result;
}

std::pair<std::string_view, std::string_view> SplitName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos
             ? std::pair(std::string_view(), name)
             : std::pair(name.substr(0, colon), name.substr(colon + 1));
}

std::optional<std::string> NamespaceFor(const pugi::xml_node& node,
                                        std::string_view prefix) {
  pugi::xml_node current = node;
  while (current) {
    const std::string attribute_name = prefix.empty()
        ? "xmlns" : "xmlns:" + std::string(prefix);
    if (const pugi::xml_attribute attribute = current.attribute(attribute_name.c_str()))
      return std::string(attribute.value());
    current = current.parent();
  }
  return std::nullopt;
}

std::vector<XmlNamespaceBinding> NamespaceBindings(const pugi::xml_node& node) {
  std::vector<XmlNamespaceBinding> result;
  for (pugi::xml_node current = node; current; current = current.parent()) {
    for (const pugi::xml_attribute attribute : current.attributes()) {
      const std::string_view name = attribute.name();
      if (name != "xmlns" && !name.starts_with("xmlns:")) continue;
      const std::string prefix =
          name == "xmlns" ? "" : std::string(name.substr(6));
      if (std::ranges::find(result, prefix, &XmlNamespaceBinding::prefix) ==
          result.end()) {
        result.push_back({prefix, attribute.value()});
      }
    }
  }
  return result;
}

std::optional<EditOperation> ParseOperation(std::string_view value) {
  if (value == "merge") return EditOperation::kMerge;
  if (value == "replace") return EditOperation::kReplace;
  if (value == "create") return EditOperation::kCreate;
  if (value == "delete") return EditOperation::kDelete;
  if (value == "remove") return EditOperation::kRemove;
  return std::nullopt;
}

std::optional<InsertPosition> ParseInsertPosition(std::string_view value) {
  if (value == "first") return InsertPosition::kFirst;
  if (value == "last") return InsertPosition::kLast;
  if (value == "before") return InsertPosition::kBefore;
  if (value == "after") return InsertPosition::kAfter;
  return std::nullopt;
}

std::string PathComponent(const RuntimeSchemaNode& schema) {
  return "/{" + schema.name.namespace_uri + "}" + schema.name.local_name;
}

struct MutableNode {
  RuntimeSchemaNodeId schema = kInvalidRuntimeSchemaNodeId;
  QualifiedXmlName name;
  std::optional<std::string> value;
  std::vector<XmlNamespaceBinding> namespaces;
  std::vector<MutableNode> children;
};

MutableNode CloneNode(const ConfigDocument& document, ConfigNodeId id) {
  const ConfigNode& source = document.Get(id);
  MutableNode result{source.schema, source.name, source.value,
                     source.value_namespaces, {}};
  for (ConfigNodeId child : source.children)
    result.children.push_back(CloneNode(document, child));
  return result;
}

std::optional<std::string> ChildValue(const MutableNode& node,
                                      RuntimeSchemaNodeId schema) {
  const auto found = std::ranges::find(node.children, schema, &MutableNode::schema);
  return found == node.children.end() ? std::nullopt : found->value;
}

std::optional<std::string> ChildValue(const ConfigDocument& document,
                                      const ConfigNode& node,
                                      RuntimeSchemaNodeId schema) {
  const auto found = std::ranges::find_if(node.children, [&](ConfigNodeId child) {
    return document.Get(child).schema == schema;
  });
  return found == node.children.end() ? std::nullopt : document.Get(*found).value;
}

bool SameInstance(const RuntimeSchema& runtime, const MutableNode& target,
                  const ConfigDocument& edit, ConfigNodeId edit_id) {
  const ConfigNode& update = edit.Get(edit_id);
  if (target.schema != update.schema) return false;
  const RuntimeSchemaNode& schema = runtime.Get(target.schema);
  if (schema.kind == semantic::SchemaNodeKind::kLeafList)
    return target.value == update.value;
  if (schema.kind != semantic::SchemaNodeKind::kList) return true;
  return std::ranges::all_of(schema.keys, [&](RuntimeSchemaNodeId key) {
    return ChildValue(target, key) == ChildValue(edit, update, key);
  });
}

bool SameInstance(const RuntimeSchema& runtime,
                  const ConfigDocument& left, ConfigNodeId left_id,
                  const ConfigDocument& right, ConfigNodeId right_id) {
  const ConfigNode& left_node = left.Get(left_id);
  const ConfigNode& right_node = right.Get(right_id);
  if (left_node.schema != right_node.schema) return false;
  const RuntimeSchemaNode& schema = runtime.Get(left_node.schema);
  if (schema.kind == semantic::SchemaNodeKind::kLeafList)
    return left_node.value == right_node.value;
  if (schema.kind != semantic::SchemaNodeKind::kList) return true;
  return std::ranges::all_of(schema.keys, [&](RuntimeSchemaNodeId key) {
    return ChildValue(left, left_node, key) == ChildValue(right, right_node, key);
  });
}

std::string InstancePath(const RuntimeSchema& runtime,
                         const ConfigDocument& document, ConfigNodeId id,
                         std::string_view parent) {
  const ConfigNode& node = document.Get(id);
  std::string path = std::string(parent) + PathComponent(runtime.Get(node.schema));
  const RuntimeSchemaNode& schema = runtime.Get(node.schema);
  if (schema.kind == semantic::SchemaNodeKind::kList) {
    for (RuntimeSchemaNodeId key : schema.keys) {
      const RuntimeSchemaNode& key_schema = runtime.Get(key);
      path += "[{" + key_schema.name.namespace_uri + "}" +
              key_schema.name.local_name + "='" +
              ChildValue(document, node, key).value_or("") + "']";
    }
  } else if (schema.kind == semantic::SchemaNodeKind::kLeafList) {
    path += "[.='" + node.value.value_or("") + "']";
  }
  return path;
}

std::optional<std::vector<std::pair<QualifiedXmlName, std::string>>>
ParseKeyPredicate(std::string_view text, std::string_view default_namespace,
                  const std::vector<XmlNamespaceBinding>& namespaces) {
  std::vector<std::pair<QualifiedXmlName, std::string>> result;
  std::size_t offset = 0;
  while (offset < text.size()) {
    while (offset < text.size() && text[offset] == ' ') ++offset;
    if (offset == text.size()) break;
    if (text[offset++] != '[') return std::nullopt;
    const std::size_t equals = text.find('=', offset);
    if (equals == std::string_view::npos) return std::nullopt;
    std::string name(text.substr(offset, equals - offset));
    while (!name.empty() && name.back() == ' ') name.pop_back();
    std::string namespace_uri(default_namespace);
    if (const std::size_t colon = name.find(':'); colon != std::string::npos) {
      const auto binding = std::ranges::find(
          namespaces, name.substr(0, colon), &XmlNamespaceBinding::prefix);
      if (binding == namespaces.end()) return std::nullopt;
      namespace_uri = binding->namespace_uri;
      name.erase(0, colon + 1);
    }
    offset = equals + 1;
    while (offset < text.size() && text[offset] == ' ') ++offset;
    if (offset == text.size() || (text[offset] != '\'' && text[offset] != '"'))
      return std::nullopt;
    const char quote = text[offset++];
    const std::size_t close = text.find(quote, offset);
    if (close == std::string_view::npos) return std::nullopt;
    std::string value(text.substr(offset, close - offset));
    offset = close + 1;
    while (offset < text.size() && text[offset] == ' ') ++offset;
    if (offset == text.size() || text[offset++] != ']') return std::nullopt;
    result.emplace_back(
        QualifiedXmlName{std::move(namespace_uri), std::move(name)},
        std::move(value));
  }
  return result.empty() ? std::nullopt : std::optional(std::move(result));
}

bool MatchesAnchor(const RuntimeSchema& runtime, const MutableNode& candidate,
                   const InsertDirective& insertion) {
  const RuntimeSchemaNode& schema = runtime.Get(candidate.schema);
  if (!insertion.anchor) return false;
  if (schema.kind == semantic::SchemaNodeKind::kLeafList)
    return candidate.value == insertion.anchor;
  const auto keys = ParseKeyPredicate(*insertion.anchor,
                                      schema.name.namespace_uri,
                                      insertion.namespaces);
  if (!keys || keys->size() != schema.keys.size()) return false;
  return std::ranges::all_of(schema.keys, [&](RuntimeSchemaNodeId key) {
    const QualifiedXmlName& name = runtime.Get(key).name;
    const auto found = std::ranges::find(
        *keys, name, &std::pair<QualifiedXmlName, std::string>::first);
    return found != keys->end() && ChildValue(candidate, key) == found->second;
  });
}

bool Reposition(const RuntimeSchema& runtime, std::vector<MutableNode>& siblings,
                std::size_t node_index, const InsertDirective& insertion) {
  MutableNode node = std::move(siblings.at(node_index));
  siblings.erase(siblings.begin() + static_cast<std::ptrdiff_t>(node_index));
  auto same_schema = [&](const MutableNode& candidate) {
    return candidate.schema == node.schema;
  };
  auto position = siblings.end();
  if (insertion.position == InsertPosition::kFirst) {
    position = std::ranges::find_if(siblings, same_schema);
  } else if (insertion.position == InsertPosition::kBefore ||
             insertion.position == InsertPosition::kAfter) {
    position = std::ranges::find_if(siblings, [&](const MutableNode& candidate) {
      return same_schema(candidate) && MatchesAnchor(runtime, candidate, insertion);
    });
    if (position == siblings.end()) return false;
    if (insertion.position == InsertPosition::kAfter) ++position;
  } else {
    auto reverse_view = siblings | std::views::reverse;
    const auto reverse = std::ranges::find_if(reverse_view, same_schema);
    if (reverse != reverse_view.end()) position = reverse.base();
  }
  siblings.insert(position, std::move(node));
  return true;
}

bool SchemaContains(const RuntimeSchema& runtime, RuntimeSchemaNodeId ancestor,
                    RuntimeSchemaNodeId target) {
  if (ancestor == target) return true;
  return std::ranges::any_of(runtime.Get(ancestor).children,
                            [&](RuntimeSchemaNodeId child) {
    return SchemaContains(runtime, child, target);
  });
}

std::optional<RuntimeSchemaNodeId> ChoiceCaseFor(
    const RuntimeSchema& runtime, RuntimeSchemaNodeId choice,
    RuntimeSchemaNodeId child) {
  for (RuntimeSchemaNodeId candidate : runtime.Get(choice).children) {
    if (runtime.Get(candidate).kind == semantic::SchemaNodeKind::kCase &&
        SchemaContains(runtime, candidate, child)) return candidate;
  }
  return std::nullopt;
}

std::string InstancePath(const RuntimeSchema& runtime, const MutableNode& node,
                         std::string_view parent) {
  std::string path = std::string(parent) + PathComponent(runtime.Get(node.schema));
  const RuntimeSchemaNode& schema = runtime.Get(node.schema);
  if (schema.kind == semantic::SchemaNodeKind::kList) {
    for (RuntimeSchemaNodeId key : schema.keys) {
      path += "[{" + runtime.Get(key).name.namespace_uri + "}" +
              runtime.Get(key).name.local_name + "='" +
              ChildValue(node, key).value_or("") + "']";
    }
  } else if (schema.kind == semantic::SchemaNodeKind::kLeafList) {
    path += "[.='" + node.value.value_or("") + "']";
  }
  return path;
}
}  // namespace

std::optional<EditOperation> EditDocument::operation(ConfigNodeId id) const {
  return operations_.at(id);
}

std::optional<InsertDirective> EditDocument::insertion(ConfigNodeId id) const {
  return insertions_.at(id);
}

EditParseResult ParseEditXml(const RuntimeSchema& schema, std::string_view xml) {
  EditParseResult result;
  if (xml.size() > DefaultResourceLimits().maximum_xml_bytes) {
    result.findings.push_back(Error(ValidationCode::kResourceLimitExceeded,
        "edit XML exceeds the byte limit", "", "too-big"));
    return result;
  }
  pugi::xml_document document;
  const pugi::xml_parse_result parsed =
      document.load_buffer(xml.data(), xml.size(), pugi::parse_default);
  if (!parsed) {
    result.findings.push_back(Error(ValidationCode::kMalformedXml,
        std::string("malformed XML: ") + parsed.description(), "",
        "malformed-message"));
    return result;
  }
  std::string resource_error;
  if (!XmlWithinResourceLimits(document, xml, DefaultResourceLimits(),
                               &resource_error)) {
    result.findings.push_back(Error(ValidationCode::kResourceLimitExceeded,
        resource_error, "", "too-big"));
    return result;
  }
  std::vector<std::optional<EditOperation>> operations;
  std::vector<std::optional<InsertDirective>> insertions;
  std::function<void(pugi::xml_node, bool)> visit;
  visit = [&](pugi::xml_node node, bool wrapper) {
    std::optional<EditOperation> operation;
    std::optional<InsertPosition> insert_position;
    std::optional<std::string> key_anchor;
    std::optional<std::string> value_anchor;
    for (pugi::xml_attribute attribute = node.first_attribute(); attribute;) {
      pugi::xml_attribute next = attribute.next_attribute();
      const auto [prefix, local] = SplitName(attribute.name());
      const auto namespace_uri = NamespaceFor(node, prefix);
      if (local == "operation" && namespace_uri == kNetconfNamespace) {
        operation = ParseOperation(attribute.value());
        if (!operation) result.findings.push_back(Error(
            ValidationCode::kInvalidValue, "unknown NETCONF operation value",
            "", "invalid-value"));
        node.remove_attribute(attribute);
      } else if (namespace_uri == kYangNamespace && local == "insert") {
        insert_position = ParseInsertPosition(attribute.value());
        if (!insert_position) result.findings.push_back(Error(
            ValidationCode::kInvalidValue, "unknown YANG insert value", "",
            "bad-attribute"));
        node.remove_attribute(attribute);
      } else if (namespace_uri == kYangNamespace && local == "key") {
        key_anchor = attribute.value();
        node.remove_attribute(attribute);
      } else if (namespace_uri == kYangNamespace && local == "value") {
        value_anchor = attribute.value();
        node.remove_attribute(attribute);
      }
      attribute = next;
    }
    if (!wrapper) {
      operations.push_back(operation);
      if (key_anchor && value_anchor) {
        result.findings.push_back(Error(
            ValidationCode::kInvalidValue,
            "an insertion cannot contain both YANG key and value anchors", "",
            "bad-attribute"));
      }
      if (insert_position) {
        insertions.push_back(
            InsertDirective{*insert_position,
                            key_anchor ? key_anchor : value_anchor,
                            key_anchor.has_value(),
                            NamespaceBindings(node)});
      } else {
        if (key_anchor || value_anchor) result.findings.push_back(Error(
            ValidationCode::kInvalidValue,
            "YANG key/value insertion anchor requires an insert attribute", "",
            "bad-attribute"));
        insertions.push_back(std::nullopt);
      }
    }
    for (pugi::xml_node child : node.children()) {
      if (child.type() == pugi::node_element) visit(child, false);
    }
  };
  const pugi::xml_node root = document.document_element();
  const auto [root_prefix, root_local] = SplitName(root.name());
  const bool wrapper = NamespaceFor(root, root_prefix) == kNetconfNamespace &&
                       (root_local == "config" || root_local == "data");
  visit(root, wrapper);
  if (!result.findings.empty()) return result;
  std::ostringstream serialized;
  document.print(serialized, "", pugi::format_raw);
  ConfigParseResult bound = ParseDatastoreXml(
      schema, serialized.str(), {.coverage = Coverage::kSelected});
  if (!bound.document) {
    result.findings = std::move(bound.findings);
    return result;
  }
  std::vector<ConfigNodeId> preorder;
  std::function<void(ConfigNodeId)> collect = [&](ConfigNodeId id) {
    preorder.push_back(id);
    for (ConfigNodeId child : bound.document->Get(id).children) collect(child);
  };
  for (ConfigNodeId id : bound.document->roots()) collect(id);
  if (preorder.size() != operations.size()) {
    result.findings.push_back(Error(ValidationCode::kInvalidNodeShape,
        "edit metadata cannot be associated with opaque XML content", "",
        "operation-not-supported"));
    return result;
  }
  EditDocument edit;
  edit.data_ = std::move(*bound.document);
  edit.operations_.resize(edit.data_.size());
  edit.insertions_.resize(edit.data_.size());
  for (std::size_t index = 0; index < preorder.size(); ++index) {
    edit.operations_.at(preorder[index]) = operations[index];
    edit.insertions_.at(preorder[index]) = insertions[index];
    if (insertions[index]) {
      const RuntimeSchemaNode& node = schema.Get(edit.data_.Get(preorder[index]).schema);
      const bool repeated = node.kind == semantic::SchemaNodeKind::kList ||
                            node.kind == semantic::SchemaNodeKind::kLeafList;
      const bool relative = insertions[index]->position == InsertPosition::kBefore ||
                            insertions[index]->position == InsertPosition::kAfter;
      if (!repeated || !node.ordered_by_user) {
        result.findings.push_back(Error(
            ValidationCode::kInvalidValue,
            "insertion attributes require an ordered-by user list or leaf-list",
            "", "unknown-attribute"));
      } else if ((relative && !insertions[index]->anchor) ||
                 (!relative && insertions[index]->anchor) ||
                 (relative &&
                  insertions[index]->anchor_is_key !=
                      (node.kind == semantic::SchemaNodeKind::kList)) ||
                 (node.kind == semantic::SchemaNodeKind::kList && relative &&
           !ParseKeyPredicate(*insertions[index]->anchor,
                              node.name.namespace_uri,
                              insertions[index]->namespaces))) {
        result.findings.push_back(Error(
            ValidationCode::kInvalidValue,
            "invalid insertion directive for this schema node", "",
            "bad-attribute"));
      }
    }
  }
  if (!result.findings.empty()) return result;
  result.document = std::move(edit);
  return result;
}

EditResult ConfigEditor::Apply(const EditRequest& request) const {
  EditResult result;
  const bool target_complete = std::ranges::all_of(
      std::views::iota(ConfigNodeId{0},
                       static_cast<ConfigNodeId>(request.target.size())),
      [&](ConfigNodeId id) {
        return request.target.Get(id).child_coverage == Coverage::kComplete;
      });
  if (!target_complete) {
    if (request.context == nullptr) {
      result.errors.push_back(Error(
          ValidationCode::kContextRequired,
          "editing a partial target requires a complete context tree", "",
          "operation-not-supported"));
      return result;
    }
    EditDocument overlay;
    overlay.data_ = request.target;
    overlay.operations_.resize(request.target.size());
    overlay.insertions_.resize(request.target.size());
    EditResult composed = Apply(
        {request.schema, *request.context, overlay, EditOperation::kMerge});
    if (!composed.candidate) return composed;
    return Apply({request.schema, *composed.candidate, request.edit,
                  request.default_operation, nullptr,
                  request.validate_candidate});
  }
  std::vector<MutableNode> roots;
  for (ConfigNodeId root : request.target.roots())
    roots.push_back(CloneNode(request.target, root));

  std::function<bool(std::vector<MutableNode>&, const ConfigDocument&, ConfigNodeId,
                     EditOperation, std::string_view,
                     std::optional<RuntimeSchemaNodeId>)>
      apply;
  apply = [&](std::vector<MutableNode>& siblings, const ConfigDocument& edit,
              ConfigNodeId edit_id, EditOperation inherited,
              std::string_view parent_path,
              std::optional<RuntimeSchemaNodeId> parent_schema) {
    const EditOperation operation =
        request.edit.operation(edit_id).value_or(inherited);
    const ConfigNode& update = edit.Get(edit_id);
    const auto match = std::ranges::find_if(siblings, [&](const MutableNode& node) {
      return SameInstance(request.schema, node, edit, edit_id);
    });
    MutableNode update_node = CloneNode(edit, edit_id);
    const std::string path = InstancePath(request.schema, update_node, parent_path);
    const auto apply_insertion = [&](std::size_t index) {
      const auto insertion = request.edit.insertion(edit_id);
      if (!insertion) return true;
      if (Reposition(request.schema, siblings, index, *insertion)) return true;
      result.errors.push_back(Error(
          ValidationCode::kUnknownDataNode,
          "ordered-by insertion anchor does not exist", path, "data-missing"));
      return false;
    };
    if (operation == EditOperation::kCreate) {
      if (match != siblings.end()) {
        result.errors.push_back(Error(ValidationCode::kDuplicateNode,
            "create target already exists", path, "data-exists"));
        return false;
      }
      siblings.push_back(std::move(update_node));
      result.changes.push_back({ChangeKind::kCreated, path, std::nullopt,
                                update.value, update.schema});
      return apply_insertion(siblings.size() - 1);
    }
    if (operation == EditOperation::kDelete || operation == EditOperation::kRemove) {
      if (match == siblings.end()) {
        if (operation == EditOperation::kDelete) {
          result.errors.push_back(Error(ValidationCode::kUnknownDataNode,
              "delete target does not exist", path, "data-missing"));
          return false;
        }
        return true;
      }
      result.changes.push_back({ChangeKind::kDeleted, path, match->value,
                                std::nullopt, update.schema});
      siblings.erase(match);
      return true;
    }
    if (operation == EditOperation::kNone) {
      if (match == siblings.end()) {
        result.errors.push_back(Error(
            ValidationCode::kUnknownDataNode,
            "default-operation none requires the ancestor to exist", path,
            "data-missing"));
        return false;
      }
      for (ConfigNodeId child : update.children) {
        if (!apply(match->children, edit, child, EditOperation::kNone, path,
                   update.schema)) {
          return false;
        }
      }
      return true;
    }
    if (operation == EditOperation::kReplace) {
      if (match == siblings.end()) {
        siblings.push_back(std::move(update_node));
        result.changes.push_back({ChangeKind::kCreated, path, std::nullopt,
                                  update.value, update.schema});
        if (!apply_insertion(siblings.size() - 1)) return false;
      } else {
        const std::size_t index = static_cast<std::size_t>(match - siblings.begin());
        const auto before = match->value;
        *match = std::move(update_node);
        result.changes.push_back({ChangeKind::kSubtreeReplaced, path, before,
                                  update.value, update.schema});
        if (!apply_insertion(index)) return false;
      }
      return true;
    }
    if (match == siblings.end()) {
      siblings.push_back(std::move(update_node));
      result.changes.push_back({ChangeKind::kCreated, path, std::nullopt,
                                update.value, update.schema});
      return apply_insertion(siblings.size() - 1);
    }
    const std::size_t existing_index =
        static_cast<std::size_t>(match - siblings.begin());
    if (update.value && update.value != match->value) {
      result.changes.push_back({ChangeKind::kValueChanged, path, match->value,
                                update.value, update.schema});
      match->value = update.value;
      match->namespaces = update.value_namespaces;
    }
    for (ConfigNodeId child : update.children) {
      if (!apply(match->children, edit, child, request.default_operation, path,
                 update.schema)) return false;
      if (parent_schema || update.schema != kInvalidRuntimeSchemaNodeId) {
        for (RuntimeSchemaNodeId choice : request.schema.Get(update.schema).children) {
          if (request.schema.Get(choice).kind != semantic::SchemaNodeKind::kChoice)
            continue;
          const auto selected = ChoiceCaseFor(request.schema, choice,
                                              edit.Get(child).schema);
          if (!selected) continue;
          std::erase_if(match->children, [&](const MutableNode& sibling) {
            const auto active = ChoiceCaseFor(request.schema, choice, sibling.schema);
            return active && *active != *selected;
          });
        }
      }
    }
    return apply_insertion(existing_index);
  };
  for (ConfigNodeId root : request.edit.data().roots()) {
    if (!apply(roots, request.edit.data(), root, request.default_operation, "",
               std::nullopt)) {
      result.changes.clear();
      return result;
    }
  }
  const auto build_candidate = [&]() {
    ConfigDocument candidate;
    candidate.source_ = request.target.source_;
    std::function<ConfigNodeId(const MutableNode&, std::optional<ConfigNodeId>)>
        flatten;
    flatten = [&](const MutableNode& source,
                  std::optional<ConfigNodeId> parent) {
      const ConfigNodeId id =
          static_cast<ConfigNodeId>(candidate.nodes_.size());
      candidate.nodes_.push_back({id, source.schema, parent, source.name,
          source.value, source.namespaces, Coverage::kComplete, {},
          std::nullopt});
      for (const MutableNode& child : source.children) {
        const ConfigNodeId child_id = flatten(child, id);
        candidate.nodes_.at(id).children.push_back(child_id);
      }
      return id;
    };
    for (const MutableNode& root : roots)
      candidate.roots_.push_back(flatten(root, std::nullopt));
    return candidate;
  };
  ConfigDocument candidate = build_candidate();
  if (!request.validate_candidate) {
    result.candidate = std::move(candidate);
    std::ranges::sort(result.changes, {}, &ChangeEvent::instance_path);
    return result;
  }
  ConfigValidator validator;
  ValidationResult validation = validator.Validate({request.schema, candidate});
  std::vector<ConfigNodeId> inaccessible;
  for (const ValidationFinding& finding : validation.findings) {
    if (finding.code == ValidationCode::kWhenViolation && finding.config_node)
      inaccessible.push_back(*finding.config_node);
  }
  if (!inaccessible.empty()) {
    std::ranges::sort(inaccessible);
    inaccessible.erase(std::ranges::unique(inaccessible).begin(),
                       inaccessible.end());
    std::function<std::optional<MutableNode>(ConfigNodeId)> clone_accessible;
    clone_accessible = [&](ConfigNodeId id) -> std::optional<MutableNode> {
      if (std::ranges::binary_search(inaccessible, id)) {
        result.changes.push_back({ChangeKind::kDeleted, "", candidate.Get(id).value,
                                  std::nullopt, candidate.Get(id).schema});
        return std::nullopt;
      }
      const ConfigNode& source = candidate.Get(id);
      MutableNode node{source.schema, source.name, source.value,
                       source.value_namespaces, {}};
      for (ConfigNodeId child : source.children) {
        if (auto retained = clone_accessible(child))
          node.children.push_back(std::move(*retained));
      }
      return node;
    };
    std::vector<MutableNode> accessible_roots;
    for (ConfigNodeId root : candidate.roots()) {
      if (auto retained = clone_accessible(root))
        accessible_roots.push_back(std::move(*retained));
    }
    roots = std::move(accessible_roots);
    candidate = build_candidate();
    validation = validator.Validate({request.schema, candidate});
  }
  if (!validation.valid || !validation.complete) {
    result.errors = std::move(validation.findings);
    result.changes.clear();
    return result;
  }
  result.candidate = std::move(candidate);
  std::ranges::sort(result.changes, {}, &ChangeEvent::instance_path);
  return result;
}

std::vector<ChangeEvent> DiffConfigDocuments(
    const RuntimeSchema& schema, const ConfigDocument& before,
    const ConfigDocument& after) {
  std::vector<ChangeEvent> changes;
  std::function<void(const ConfigDocument&, ConfigNodeId, std::string_view,
                     ChangeKind)> add_subtree;
  add_subtree = [&](const ConfigDocument& document, ConfigNodeId id,
                    std::string_view parent_path, ChangeKind kind) {
    const ConfigNode& node = document.Get(id);
    const std::string path = InstancePath(schema, document, id, parent_path);
    changes.push_back({kind, path,
                       kind == ChangeKind::kDeleted ? node.value : std::nullopt,
                       kind == ChangeKind::kCreated ? node.value : std::nullopt,
                       node.schema});
    for (ConfigNodeId child : node.children)
      add_subtree(document, child, path, kind);
  };
  std::function<void(const std::vector<ConfigNodeId>&,
                     const std::vector<ConfigNodeId>&, std::string_view)>
      compare;
  compare = [&](const std::vector<ConfigNodeId>& left,
                const std::vector<ConfigNodeId>& right,
                std::string_view parent_path) {
    std::vector<bool> matched(right.size(), false);
    for (ConfigNodeId left_id : left) {
      std::optional<std::size_t> match;
      for (std::size_t index = 0; index < right.size(); ++index) {
        if (!matched[index] &&
            SameInstance(schema, before, left_id, after, right[index])) {
          match = index;
          break;
        }
      }
      if (!match) {
        add_subtree(before, left_id, parent_path, ChangeKind::kDeleted);
        continue;
      }
      matched[*match] = true;
      const ConfigNode& left_node = before.Get(left_id);
      const ConfigNode& right_node = after.Get(right[*match]);
      const std::string path = InstancePath(schema, before, left_id, parent_path);
      if (left_node.value != right_node.value) {
        changes.push_back({ChangeKind::kValueChanged, path, left_node.value,
                           right_node.value, left_node.schema});
      }
      compare(left_node.children, right_node.children, path);
    }
    for (std::size_t index = 0; index < right.size(); ++index) {
      if (!matched[index])
        add_subtree(after, right[index], parent_path, ChangeKind::kCreated);
    }
  };
  compare(before.roots(), after.roots(), "");
  std::ranges::sort(changes, {}, &ChangeEvent::instance_path);
  return changes;
}

}  // namespace yang::config
