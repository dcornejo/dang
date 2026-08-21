// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_SERVER_H_
#define YANG_NETCONF_SERVER_H_

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "yang/netconf_datastore.h"
#include "yang/netconf_notifications.h"
#include "yang/netconf_session_registry.h"
#include "yang/netconf_url.h"
#include "yang/netconf_with_defaults.h"
#include "yang/nacm.h"

namespace yang::netconf {

/** Supplies config-false XML children for NETCONF get responses. */
class OperationalDataProvider {
 public:
  /** Result of looking up one source for RFC 6022 get-schema. */
  struct SchemaLookup {
    enum class Status { kFound, kNotFound, kNotUnique, kUnsupportedFormat };
    Status status = Status::kNotFound;
    std::string content;
  };
  virtual ~OperationalDataProvider() = default;
  /** Returns a complete data element augmented with operational state. */
  [[nodiscard]] virtual std::string AugmentDataXml(
      std::string_view configuration_data_xml) const = 0;
  /** Returns NETCONF capabilities associated with the supplied state. */
  [[nodiscard]] virtual std::vector<std::string> Capabilities() const {
    return {};
  }
  /** Finds a model source by module, optional revision, and format. */
  [[nodiscard]] virtual SchemaLookup GetSchema(
      std::string_view identifier, std::optional<std::string_view> version,
      std::string_view format) const {
    (void)identifier;
    (void)version;
    (void)format;
    return {};
  }
};

/** Result of processing one complete NETCONF RPC message. */
struct RpcResponse {
  std::string xml;
  bool close_session = false;
};

/** Authenticated and protocol identity of the caller of an RPC. */
struct RpcSessionContext {
  std::uint32_t session_id = 0;
  std::string_view username;
  std::string_view datastore_owner;
  std::span<const std::string> external_groups;
};

/** Privacy-minimal audit record for use of host recovery privilege. */
struct RecoveryAuditRecord {
  std::uint32_t session_id = 0;
  std::string username;
  std::size_t rpc_bytes = 0;
};

using RecoveryAuditSink = std::function<void(const RecoveryAuditRecord&)>;

/** Host result for an application-defined schema RPC or action. */
struct OperationResult {
  TransactionResult result;
  std::string output_xml;
};

/** Dispatches schema-defined operations after the server completes NACM. */
class OperationProvider {
 public:
  virtual ~OperationProvider() = default;
  [[nodiscard]] virtual OperationResult InvokeRpc(
      const RpcSessionContext& session,
      const config::RuntimeSchemaNode& operation,
      std::string_view operation_xml) = 0;
  [[nodiscard]] virtual OperationResult InvokeAction(
      const RpcSessionContext& session,
      const config::RuntimeSchemaNode& action,
      std::string_view instance_path, std::string_view action_xml) {
    (void)session;
    (void)action;
    (void)instance_path;
    (void)action_xml;
    config::ValidationFinding finding;
    finding.message = "no handler is registered for the requested action";
    finding.netconf_error_tag = "operation-not-supported";
    return {{false, {std::move(finding)}, {}}, {}};
  }
};

/**
 * XML RPC service for the RFC 6241 datastore operations.
 *
 * Message framing, authentication, and I/O remain the host's responsibility.
 */
class NetconfServer {
 public:
  explicit NetconfServer(DatastoreManager& datastores,
                         const NacmPolicy* nacm = nullptr,
                         UrlDatastoreProvider* urls = nullptr,
                         NotificationManager* notifications = nullptr,
                         std::optional<WithDefaultsConfig> with_defaults = std::nullopt,
                         OperationalDataProvider* operational = nullptr,
                         OperationProvider* operations = nullptr)
      : datastores_(datastores), nacm_(nacm), urls_(urls),
        notifications_(notifications), with_defaults_(with_defaults),
        operational_(operational), operations_(operations) {}

  /** Returns a server hello with the supported capability URIs. */
  [[nodiscard]] std::string ServerHello(std::uint32_t session_id) const;
  /** Returns the statically advertised RFC 6241 capability set. */
  [[nodiscard]] static std::vector<std::string> Capabilities();
  /** Returns capabilities configured for this server instance. */
  [[nodiscard]] std::vector<std::string> AdvertisedCapabilities() const;
  /** Parses, dispatches, and serializes one unframed rpc element. */
  [[nodiscard]] RpcResponse Process(std::string_view session,
                                    std::string_view rpc_xml);
  /** Processes an RPC with distinct protocol, authorization, and lock IDs. */
  [[nodiscard]] RpcResponse Process(const RpcSessionContext& session,
                                    std::string_view rpc_xml);
  /** Installs a host audit sink called for every recovery-user RPC attempt. */
  void SetRecoveryAuditSink(RecoveryAuditSink sink) {
    recovery_audit_sink_ = std::move(sink);
  }
  /** Registers a transport session before its server hello is sent. */
  [[nodiscard]] bool RegisterSession(std::uint32_t session_id,
                                     std::string username);
  /** Releases resources and removes a terminated transport session. */
  void SessionClosed(std::uint32_t session_id,
                     std::string_view datastore_owner);
  /** Returns whether kill-session has requested transport termination. */
  [[nodiscard]] bool CloseRequested(std::uint32_t session_id) const;
  /** Returns a snapshot of active protocol sessions. */
  [[nodiscard]] std::vector<SessionInfo> Sessions() const;
  /** Drains asynchronous RFC 5277 messages for a transport session. */
  [[nodiscard]] std::vector<std::string> DrainNotifications(
      std::uint32_t session_id);

 private:
  DatastoreManager& datastores_;
  const NacmPolicy* nacm_ = nullptr;
  SessionRegistry sessions_;
  UrlDatastoreProvider* urls_ = nullptr;
  NotificationManager* notifications_ = nullptr;
  std::optional<WithDefaultsConfig> with_defaults_;
  OperationalDataProvider* operational_ = nullptr;
  OperationProvider* operations_ = nullptr;
  RecoveryAuditSink recovery_audit_sink_;
};

}  // namespace yang::netconf

#endif  // YANG_NETCONF_SERVER_H_
