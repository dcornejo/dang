// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/nacm.h"

#include "yang/resource_limits.h"

#include <functional>
#include <optional>
#include <ranges>
#include <sstream>
#include <vector>
#include <utility>

#include <pugixml.hpp>

namespace yang::netconf {
namespace {

constexpr std::string_view kNacmNamespace =
    "urn:ietf:params:xml:ns:yang:ietf-netconf-acm";

std::pair<std::string_view, std::string_view> SplitName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos
      ? std::pair(std::string_view(), name)
      : std::pair(name.substr(0, colon), name.substr(colon + 1));
}

std::string NamespaceFor(const pugi::xml_node& node,
                         std::string_view prefix) {
  const std::string attribute_name = prefix.empty()
      ? "xmlns" : "xmlns:" + std::string(prefix);
  for (pugi::xml_node current = node; current; current = current.parent()) {
    if (const pugi::xml_attribute attribute =
            current.attribute(attribute_name.c_str())) {
      return attribute.value();
    }
  }
  return "";
}

std::string PathComponent(const pugi::xml_node& node) {
  const auto [prefix, local] = SplitName(node.name());
  return "/{" + NamespaceFor(node, prefix) + "}" + std::string(local);
}

bool HasElementChild(const pugi::xml_node& node) {
  return std::ranges::any_of(node.children(), [](const pugi::xml_node& child) {
    return child.type() == pugi::node_element;
  });
}

std::string InstancePathComponent(const pugi::xml_node& node) {
  std::string result = PathComponent(node);
  for (const pugi::xml_node child : node.children()) {
    if (child.type() != pugi::node_element || HasElementChild(child)) continue;
    const auto [prefix, local] = SplitName(child.name());
    const std::string value = child.text().as_string();
    if (value.find('\'') != std::string::npos) continue;
    result += "[{" + NamespaceFor(child, prefix) + "}" +
              std::string(local) + "='" + value + "']";
  }
  return result;
}

std::string_view LocalName(std::string_view name) {
  return SplitName(name).second;
}

pugi::xml_node Child(const pugi::xml_node& parent, std::string_view name) {
  for (const pugi::xml_node child : parent.children()) {
    if (child.type() == pugi::node_element && LocalName(child.name()) == name)
      return child;
  }
  return {};
}

std::optional<AccessAction> ParseAction(std::string_view value) {
  if (value == "permit") return AccessAction::kPermit;
  if (value == "deny") return AccessAction::kDeny;
  return std::nullopt;
}

std::optional<bool> ParseBoolean(std::string_view value) {
  if (value == "true" || value == "1") return true;
  if (value == "false" || value == "0") return false;
  return std::nullopt;
}

std::optional<std::uint8_t> ParseOperations(std::string_view value) {
  if (value.empty() || value == "*") return static_cast<std::uint8_t>(31);
  std::uint8_t result = 0;
  std::istringstream words{std::string(value)};
  std::string word;
  while (words >> word) {
    if (word == "read") result |= AccessMask(AccessOperation::kRead);
    else if (word == "create") result |= AccessMask(AccessOperation::kCreate);
    else if (word == "update") result |= AccessMask(AccessOperation::kUpdate);
    else if (word == "delete") result |= AccessMask(AccessOperation::kDelete);
    else if (word == "exec") result |= AccessMask(AccessOperation::kExecute);
    else return std::nullopt;
  }
  return result == 0 ? std::nullopt : std::optional(result);
}

struct PathPredicate {
  std::string name;
  std::string value;
};

struct PathSegment {
  std::string name;
  std::vector<PathPredicate> predicates;
};

std::string_view Trim(std::string_view value) {
  const std::size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) return {};
  return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::optional<std::string> ExpandName(const pugi::xml_node& node,
                                      std::string_view value) {
  const auto [prefix, local] = SplitName(Trim(value));
  if (prefix.empty() || local.empty()) return std::nullopt;
  const std::string namespace_uri = NamespaceFor(node, prefix);
  if (namespace_uri.empty()) return std::nullopt;
  return "{" + namespace_uri + "}" + std::string(local);
}

std::optional<std::vector<PathSegment>> ParseExpandedPath(
    std::string_view value) {
  if (value == "/") return std::vector<PathSegment>{};
  if (value.empty() || value.front() != '/') return std::nullopt;
  std::vector<PathSegment> result;
  std::size_t position = 1;
  while (position < value.size()) {
    if (value[position] != '{') return std::nullopt;
    const std::size_t namespace_end = value.find('}', position + 1);
    if (namespace_end == std::string_view::npos) return std::nullopt;
    std::size_t name_end = value.find_first_of("[/", namespace_end + 1);
    if (name_end == std::string_view::npos) name_end = value.size();
    if (name_end == namespace_end + 1) return std::nullopt;
    PathSegment segment{std::string(value.substr(position, name_end - position)), {}};
    position = name_end;
    while (position < value.size() && value[position] == '[') {
      const std::size_t close = value.find(']', position + 1);
      if (close == std::string_view::npos) return std::nullopt;
      const std::string_view expression = value.substr(
          position + 1, close - position - 1);
      const std::size_t equals = expression.find('=');
      if (equals == std::string_view::npos) return std::nullopt;
      const std::string_view name = Trim(expression.substr(0, equals));
      const std::string_view literal = Trim(expression.substr(equals + 1));
      if ((name != "." && !name.starts_with('{')) || literal.size() < 2 ||
          (literal.front() != '\'' && literal.front() != '"') ||
          literal.back() != literal.front()) {
        return std::nullopt;
      }
      segment.predicates.push_back(
          {std::string(name), std::string(literal.substr(1, literal.size() - 2))});
      position = close + 1;
    }
    result.push_back(std::move(segment));
    if (position == value.size()) break;
    if (value[position] != '/' || ++position == value.size()) return std::nullopt;
  }
  return result;
}

bool PathMatches(std::string_view rule_path, std::string_view instance_path) {
  const auto rule = ParseExpandedPath(rule_path);
  const auto instance = ParseExpandedPath(instance_path);
  if (!rule || !instance || rule->size() > instance->size()) return false;
  for (std::size_t index = 0; index < rule->size(); ++index) {
    if (rule->at(index).name != instance->at(index).name) return false;
    for (const PathPredicate& expected : rule->at(index).predicates) {
      const auto found = std::ranges::find(
          instance->at(index).predicates, expected.name, &PathPredicate::name);
      if (found == instance->at(index).predicates.end() ||
          found->value != expected.value) {
        return false;
      }
    }
  }
  return true;
}

std::optional<std::string> ExpandPath(const pugi::xml_node& node,
                                      std::string_view value) {
  value = Trim(value);
  if (value == "/") return std::string("/");
  if (value.empty() || value.front() != '/') return std::nullopt;
  std::string result;
  std::size_t position = 1;
  while (position < value.size()) {
    const std::size_t name_end = value.find_first_of("[/", position);
    const std::string_view name = value.substr(
        position, (name_end == std::string_view::npos ? value.size() : name_end) -
                      position);
    const auto expanded_name = ExpandName(node, name);
    if (!expanded_name) return std::nullopt;
    result += "/" + *expanded_name;
    position = name_end == std::string_view::npos ? value.size() : name_end;
    while (position < value.size() && value[position] == '[') {
      const std::size_t close = value.find(']', position + 1);
      if (close == std::string_view::npos) return std::nullopt;
      const std::string_view expression = value.substr(
          position + 1, close - position - 1);
      const std::size_t equals = expression.find('=');
      if (equals == std::string_view::npos) return std::nullopt;
      const std::string_view key = Trim(expression.substr(0, equals));
      const std::string_view literal = Trim(expression.substr(equals + 1));
      if (literal.size() < 2 ||
          (literal.front() != '\'' && literal.front() != '"') ||
          literal.back() != literal.front()) {
        return std::nullopt;
      }
      if (key == ".") {
        result += "[.=" + std::string(literal) + "]";
      } else {
        const auto expanded_key = ExpandName(node, key);
        if (!expanded_key) return std::nullopt;
        result += "[" + *expanded_key + "=" + std::string(literal) + "]";
      }
      position = close + 1;
    }
    if (position == value.size()) break;
    if (value[position] != '/' || ++position == value.size()) return std::nullopt;
  }
  return ParseExpandedPath(result) ? std::optional(result) : std::nullopt;
}

}  // namespace

void NacmPolicy::AddUserToGroup(std::string user, std::string group) {
  memberships_.emplace_back(std::move(user), std::move(group));
}

void NacmPolicy::AddRecoveryUser(std::string user) {
  recovery_users_.insert(std::move(user));
}

void NacmPolicy::AddRule(NacmRule rule) {
  rules_.push_back(std::move(rule));
}

bool NacmPolicy::IsRecovery(std::string_view user) const {
  return recovery_users_.contains(user);
}

bool NacmPolicy::InGroup(
    std::string_view user, std::string_view group,
    std::span<const std::string> external_groups) const {
  if (group == "*") return true;
  for (const auto& [member, membership] : memberships_)
    if (member == user && membership == group) return true;
  if (external_groups_enabled_ &&
      std::ranges::find(external_groups, group) != external_groups.end()) {
    return true;
  }
  return false;
}

bool NacmPolicy::RuleApplies(
    const NacmRule& rule, std::string_view user,
    std::span<const std::string> external_groups) const {
  if (!rule.groups.empty()) {
    return std::ranges::any_of(rule.groups, [&](const std::string& group) {
      return InGroup(user, group, external_groups);
    });
  }
  return InGroup(user, rule.group, external_groups);
}

bool NacmPolicy::AuthorizeRpc(std::string_view user,
                              std::string_view rpc_name) const {
  return AuthorizeRpc(user, "", rpc_name);
}

bool NacmPolicy::AuthorizeRpc(
    std::string_view user, std::string_view module_name,
    std::string_view rpc_name,
    std::span<const std::string> external_groups,
    bool default_deny_all) const {
  if (!enabled_ || IsRecovery(user)) return true;
  if (rpc_name == "close-session") return true;
  for (const NacmRule& rule : rules_) {
    if (!RuleApplies(rule, user, external_groups) ||
        !(rule.operations & AccessMask(AccessOperation::kExecute)) ||
        !rule.path_prefix.empty() || !rule.notification_name.empty() ||
        (!rule.module_name.empty() && rule.module_name != "*" &&
         rule.module_name != module_name) ||
        (!rule.rpc_name.empty() && rule.rpc_name != "*" &&
         rule.rpc_name != rpc_name)) {
      continue;
    }
    const bool permitted = rule.action == AccessAction::kPermit;
    if (!permitted) counters_->denied_operations.fetch_add(1);
    return permitted;
  }
  const bool permitted = !default_deny_all && rpc_name != "delete-config" &&
      rpc_name != "kill-session" && exec_default_ == AccessAction::kPermit;
  if (!permitted) counters_->denied_operations.fetch_add(1);
  return permitted;
}

bool NacmPolicy::AuthorizeData(std::string_view user, AccessOperation operation,
                               std::string_view instance_path) const {
  return AuthorizeData(user, "", operation, instance_path);
}

bool NacmPolicy::AuthorizeData(
    std::string_view user, std::string_view module_name,
    AccessOperation operation, std::string_view instance_path,
    std::span<const std::string> external_groups,
    bool default_deny_all, bool default_deny_write) const {
  if (!enabled_ || IsRecovery(user)) return true;
  for (const NacmRule& rule : rules_) {
    if (!RuleApplies(rule, user, external_groups) ||
        !(rule.operations & AccessMask(operation)) ||
        !rule.rpc_name.empty() || !rule.notification_name.empty() ||
        (!rule.module_name.empty() && rule.module_name != "*" &&
         ((module_name.empty() && rule.path_prefix.empty()) ||
          (!module_name.empty() && rule.module_name != module_name))) ||
        (!rule.path_prefix.empty() &&
         !PathMatches(rule.path_prefix, instance_path))) {
      continue;
    }
    const bool permitted = rule.action == AccessAction::kPermit;
    if (!permitted && operation != AccessOperation::kRead)
      counters_->denied_data_writes.fetch_add(1);
    return permitted;
  }
  const bool annotation_denies = default_deny_all ||
      (default_deny_write && operation != AccessOperation::kRead);
  const bool permitted = !annotation_denies &&
      (operation == AccessOperation::kRead ? read_default_ : write_default_) ==
      AccessAction::kPermit;
  if (!permitted && operation != AccessOperation::kRead)
    counters_->denied_data_writes.fetch_add(1);
  return permitted;
}

bool NacmPolicy::AuthorizeNotification(
    std::string_view user, std::string_view module_name,
    std::string_view notification_name,
    std::span<const std::string> external_groups,
    bool default_deny_all) const {
  if (!enabled_ || IsRecovery(user)) return true;
  for (const NacmRule& rule : rules_) {
    if (!RuleApplies(rule, user, external_groups) ||
        !(rule.operations & AccessMask(AccessOperation::kRead)) ||
        !rule.rpc_name.empty() || !rule.path_prefix.empty() ||
        (!rule.module_name.empty() && rule.module_name != "*" &&
         rule.module_name != module_name) ||
        (!rule.notification_name.empty() && rule.notification_name != "*" &&
         rule.notification_name != notification_name)) {
      continue;
    }
    const bool permitted = rule.action == AccessAction::kPermit;
    if (!permitted) counters_->denied_notifications.fetch_add(1);
    return permitted;
  }
  const bool permitted = !default_deny_all &&
                         read_default_ == AccessAction::kPermit;
  if (!permitted) counters_->denied_notifications.fetch_add(1);
  return permitted;
}

NacmCounters NacmPolicy::counters() const noexcept {
  return {counters_->denied_operations.load(),
          counters_->denied_data_writes.load(),
          counters_->denied_notifications.load()};
}

NacmLoadResult LoadNacmPolicy(std::string_view xml) {
  NacmLoadResult loaded;
  if (xml.size() > DefaultResourceLimits().maximum_xml_bytes) {
    loaded.errors.push_back("NACM XML exceeds the byte limit");
    return loaded;
  }
  pugi::xml_document document;
  const pugi::xml_parse_result parsed =
      document.load_buffer(xml.data(), xml.size(), pugi::parse_default);
  if (!parsed) {
    loaded.errors.push_back(std::string("malformed NACM XML: ") +
                            parsed.description());
    return loaded;
  }
  std::string resource_error;
  if (!XmlWithinResourceLimits(document, xml, DefaultResourceLimits(),
                               &resource_error)) {
    loaded.errors.push_back(resource_error);
    return loaded;
  }
  const pugi::xml_node root = document.document_element();
  const auto [prefix, local] = SplitName(root.name());
  if (local != "nacm" || NamespaceFor(root, prefix) != kNacmNamespace) {
    loaded.errors.push_back("expected the ietf-netconf-acm nacm container");
    return loaded;
  }
  NacmPolicy policy;
  const auto load_boolean = [&](std::string_view name, bool default_value,
                                auto setter) {
    const pugi::xml_node child = Child(root, name);
    if (!child) {
      setter(default_value);
      return;
    }
    const auto value = ParseBoolean(child.text().as_string());
    if (!value) loaded.errors.push_back(std::string(name) + " is not boolean");
    else setter(*value);
  };
  load_boolean("enable-nacm", true,
               [&](bool value) { policy.set_enabled(value); });
  load_boolean("enable-external-groups", true, [&](bool value) {
    policy.set_external_groups_enabled(value);
  });
  const auto load_default = [&](std::string_view name, AccessAction fallback,
                                auto setter) {
    const pugi::xml_node child = Child(root, name);
    if (!child) {
      setter(fallback);
      return;
    }
    const auto action = ParseAction(child.text().as_string());
    if (!action) loaded.errors.push_back(std::string(name) + " is invalid");
    else setter(*action);
  };
  load_default("read-default", AccessAction::kPermit,
               [&](AccessAction value) { policy.set_read_default(value); });
  load_default("write-default", AccessAction::kDeny,
               [&](AccessAction value) { policy.set_write_default(value); });
  load_default("exec-default", AccessAction::kPermit,
               [&](AccessAction value) { policy.set_exec_default(value); });

  if (const pugi::xml_node groups = Child(root, "groups")) {
    std::set<std::string> names;
    for (const pugi::xml_node group : groups.children()) {
      if (group.type() != pugi::node_element ||
          LocalName(group.name()) != "group") continue;
      const std::string name = Child(group, "name").text().as_string();
      if (name.empty() || !names.insert(name).second) {
        loaded.errors.push_back("NACM group names must be nonempty and unique");
        continue;
      }
      for (const pugi::xml_node user : group.children()) {
        if (LocalName(user.name()) == "user-name" &&
            !std::string_view(user.text().as_string()).empty()) {
          policy.AddUserToGroup(user.text().as_string(), name);
        }
      }
    }
  }

  std::set<std::string> list_names;
  for (const pugi::xml_node list : root.children()) {
    if (list.type() != pugi::node_element ||
        LocalName(list.name()) != "rule-list") continue;
    const std::string list_name = Child(list, "name").text().as_string();
    if (list_name.empty() || !list_names.insert(list_name).second) {
      loaded.errors.push_back("NACM rule-list names must be nonempty and unique");
      continue;
    }
    std::vector<std::string> groups;
    for (const pugi::xml_node child : list.children())
      if (LocalName(child.name()) == "group") groups.emplace_back(child.text().as_string());
    if (groups.empty()) {
      loaded.errors.push_back("NACM rule-list requires at least one group");
      continue;
    }
    std::set<std::string> rule_names;
    for (const pugi::xml_node entry : list.children()) {
      if (LocalName(entry.name()) != "rule") continue;
      NacmRule rule;
      rule.name = Child(entry, "name").text().as_string();
      rule.groups = groups;
      rule.module_name = Child(entry, "module-name")
          ? Child(entry, "module-name").text().as_string() : "*";
      rule.rpc_name = Child(entry, "rpc-name").text().as_string();
      rule.notification_name =
          Child(entry, "notification-name").text().as_string();
      if (const pugi::xml_node path = Child(entry, "path")) {
        const auto expanded = ExpandPath(path, path.text().as_string());
        if (!expanded) loaded.errors.push_back("NACM rule path is invalid");
        else rule.path_prefix = *expanded;
      }
      const unsigned selectors = !rule.rpc_name.empty() +
          !rule.notification_name.empty() + !rule.path_prefix.empty();
      if (rule.name.empty() || !rule_names.insert(rule.name).second)
        loaded.errors.push_back("NACM rule names must be nonempty and unique within a list");
      if (selectors > 1) loaded.errors.push_back("NACM rule has multiple rule types");
      const auto operations = ParseOperations(
          Child(entry, "access-operations")
              ? Child(entry, "access-operations").text().as_string() : "*");
      const auto action = ParseAction(Child(entry, "action").text().as_string());
      if (!operations) loaded.errors.push_back("NACM access-operations is invalid");
      if (!action) loaded.errors.push_back("NACM rule action is required");
      if (!rule.name.empty() && operations && action && selectors <= 1) {
        rule.operations = *operations;
        rule.action = *action;
        policy.AddRule(std::move(rule));
      }
    }
  }
  if (loaded.errors.empty()) loaded.policy = std::move(policy);
  return loaded;
}

std::string NacmPolicy::FilterReadableData(std::string_view user,
    std::string_view data_xml,
    std::span<const std::string> external_groups,
    const config::RuntimeSchema* schema) const {
  if (data_xml.size() > DefaultResourceLimits().maximum_xml_bytes) return "";
  if (!enabled_ || IsRecovery(user)) return std::string(data_xml);
  pugi::xml_document document;
  if (!document.load_buffer(data_xml.data(), data_xml.size(), pugi::parse_default))
    return "";
  std::string resource_error;
  if (!XmlWithinResourceLimits(document, data_xml, DefaultResourceLimits(),
                               &resource_error)) {
    return "";
  }
  std::function<void(pugi::xml_node, std::string_view,
                     std::optional<config::RuntimeSchemaNodeId>)> filter;
  filter = [&](pugi::xml_node parent, std::string_view parent_path,
               std::optional<config::RuntimeSchemaNodeId> parent_schema) {
    for (pugi::xml_node child = parent.first_child(); child;) {
      pugi::xml_node next = child.next_sibling();
      if (child.type() == pugi::node_element) {
        const std::string path =
            std::string(parent_path) + InstancePathComponent(child);
        std::optional<config::RuntimeSchemaNodeId> child_schema;
        if (schema != nullptr) {
          const auto [child_prefix, child_local] = SplitName(child.name());
          const config::QualifiedXmlName name{
              NamespaceFor(child, child_prefix), std::string(child_local)};
          child_schema = parent_schema ? schema->FindChild(*parent_schema, name)
                                       : schema->FindRoot(name);
        }
        const config::RuntimeSchemaNode* metadata =
            child_schema ? &schema->Get(*child_schema) : nullptr;
        if (!AuthorizeData(user, "", AccessOperation::kRead, path,
                           external_groups,
                           metadata != nullptr &&
                               metadata->nacm_default_deny_all,
                           metadata != nullptr &&
                               metadata->nacm_default_deny_write)) {
          parent.remove_child(child);
        } else {
          filter(child, path, child_schema);
        }
      }
      child = next;
    }
  };
  filter(document.document_element(), "", std::nullopt);
  std::ostringstream serialized;
  document.print(serialized, "", pugi::format_raw);
  return serialized.str();
}

}  // namespace yang::netconf
