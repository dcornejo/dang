// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NACM_H_
#define YANG_NACM_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "yang/config_validation.h"

namespace yang::netconf {

enum class AccessOperation : std::uint8_t {
  kRead = 1,
  kCreate = 2,
  kUpdate = 4,
  kDelete = 8,
  kExecute = 16,
};

enum class AccessAction { kPermit, kDeny };

/** RFC 8341 operational denial counters. */
struct NacmCounters {
  std::uint32_t denied_operations = 0;
  std::uint32_t denied_data_writes = 0;
  std::uint32_t denied_notifications = 0;
};

struct NacmRule {
  std::string name;
  std::string group;
  /** Local RPC name for execute rules; empty matches any RPC. */
  std::string rpc_name;
  /** Expanded-name instance-path prefix for data rules; empty matches all. */
  std::string path_prefix;
  std::uint8_t operations = 0;
  AccessAction action = AccessAction::kDeny;
  /** YANG module name, or "*"/empty for every module. */
  std::string module_name;
  /** Notification name for notification rules; empty for other rule types. */
  std::string notification_name;
  /** Ordered rule-list group selectors loaded from ietf-netconf-acm. */
  std::vector<std::string> groups;
  /** True when rpc-name was explicitly present, including an empty value. */
  bool rpc_name_present = false;
  /** True when notification-name was explicitly present, including empty. */
  bool notification_name_present = false;
  /** True when module-name was explicitly present, including an empty value. */
  bool module_name_present = false;
};

/** Schema-qualified data instance used for ancestor authorization. */
struct NacmDataNode {
  std::string module_name;
  std::string instance_path;
  bool default_deny_all = false;
};

/** Thread-safe RFC 8341 policy engine supporting atomic live replacement. */
class NacmPolicy {
 public:
  NacmPolicy() = default;
  NacmPolicy(const NacmPolicy& other);
  NacmPolicy(NacmPolicy&& other);
  NacmPolicy& operator=(const NacmPolicy& other);
  NacmPolicy& operator=(NacmPolicy&& other);
  void set_enabled(bool enabled);
  void set_read_default(AccessAction action);
  void set_write_default(AccessAction action);
  void set_exec_default(AccessAction action);
  void set_external_groups_enabled(bool enabled);
  void AddUserToGroup(std::string user, std::string group);
  /** Adds one canonical host recovery identity; rejects invalid/duplicate names. */
  [[nodiscard]] bool AddRecoveryUser(std::string user);
  void AddRule(NacmRule rule);
  /** Returns whether the authenticated identity has host recovery privilege. */
  [[nodiscard]] bool IsRecoveryUser(std::string_view user) const;

  [[nodiscard]] bool AuthorizeRpc(std::string_view user,
                                  std::string_view rpc_name) const;
  /** Authorizes an RPC using its defining module and transport groups. */
  [[nodiscard]] bool AuthorizeRpc(
      std::string_view user, std::string_view module_name,
      std::string_view rpc_name,
      std::span<const std::string> external_groups = {},
      bool default_deny_all = false) const;
  /** Authorizes a data-node action after checking readable ancestors. */
  [[nodiscard]] bool AuthorizeAction(
      std::string_view user, std::string_view module_name,
      std::string_view action_name, std::string_view instance_path,
      std::span<const NacmDataNode> ancestors,
      std::span<const std::string> external_groups = {},
      bool default_deny_all = false) const;
  [[nodiscard]] bool AuthorizeData(std::string_view user,
                                   AccessOperation operation,
                                   std::string_view instance_path) const;
  /** Authorizes data access with schema-module and transport-group context. */
  [[nodiscard]] bool AuthorizeData(
      std::string_view user, std::string_view module_name,
      AccessOperation operation, std::string_view instance_path,
      std::span<const std::string> external_groups = {},
      bool default_deny_all = false,
      bool default_deny_write = false) const;
  /** Authorizes delivery of a notification to a session. */
  [[nodiscard]] bool AuthorizeNotification(
      std::string_view user, std::string_view module_name,
      std::string_view notification_name,
      std::span<const std::string> external_groups = {},
      bool default_deny_all = false) const;
  /** Authorizes a data-associated notification and its readable ancestors. */
  [[nodiscard]] bool AuthorizeNotification(
      std::string_view user, std::string_view module_name,
      std::string_view notification_name, std::string_view instance_path,
      std::span<const NacmDataNode> ancestors,
      std::span<const std::string> external_groups = {},
      bool default_deny_all = false) const;
  /** Returns an atomic snapshot of RFC 8341 operational counters. */
  [[nodiscard]] NacmCounters counters() const;
  /** Retains host-owned recovery identities and counters across policy reloads. */
  void PreserveRuntimeStateFrom(const NacmPolicy& previous);
  /** Silently removes read-denied data nodes, as required by RFC 8341. */
  [[nodiscard]] std::string FilterReadableData(std::string_view user,
      std::string_view data_xml,
      std::span<const std::string> external_groups = {},
      const config::RuntimeSchema* schema = nullptr) const;

 private:
  /** Serializes live replacement with immutable per-operation snapshots. */
  mutable std::recursive_mutex mutex_;
  [[nodiscard]] bool IsRecovery(std::string_view user) const;
  [[nodiscard]] bool InGroup(std::string_view user,
                             std::string_view group,
                             std::span<const std::string> external_groups) const;
  [[nodiscard]] bool RuleApplies(
      const NacmRule& rule, std::string_view user,
      std::span<const std::string> external_groups) const;
  bool enabled_ = true;
  AccessAction read_default_ = AccessAction::kPermit;
  AccessAction write_default_ = AccessAction::kDeny;
  AccessAction exec_default_ = AccessAction::kPermit;
  bool external_groups_enabled_ = true;
  std::vector<std::pair<std::string, std::string>> memberships_;
  std::set<std::string, std::less<>> recovery_users_;
  std::vector<NacmRule> rules_;
  struct CounterState {
    std::atomic<std::uint32_t> denied_operations{0};
    std::atomic<std::uint32_t> denied_data_writes{0};
    std::atomic<std::uint32_t> denied_notifications{0};
  };
  std::shared_ptr<CounterState> counters_ = std::make_shared<CounterState>();
};

/** Result of loading an ietf-netconf-acm configuration subtree. */
struct NacmLoadResult {
  std::optional<NacmPolicy> policy;
  std::vector<std::string> errors;
};

/** Parses the RFC 8341 nacm container from NETCONF XML encoding. */
[[nodiscard]] NacmLoadResult LoadNacmPolicy(std::string_view xml);

[[nodiscard]] constexpr std::uint8_t AccessMask(AccessOperation operation) {
  return static_cast<std::uint8_t>(operation);
}

}  // namespace yang::netconf

#endif  // YANG_NACM_H_
