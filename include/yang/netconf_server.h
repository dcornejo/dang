// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_SERVER_H_
#define YANG_NETCONF_SERVER_H_

#include <string>
#include <string_view>
#include <span>
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
  virtual ~OperationalDataProvider() = default;
  /** Returns a complete data element augmented with operational state. */
  [[nodiscard]] virtual std::string AugmentDataXml(
      std::string_view configuration_data_xml) const = 0;
  /** Returns NETCONF capabilities associated with the supplied state. */
  [[nodiscard]] virtual std::vector<std::string> Capabilities() const {
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
                         OperationalDataProvider* operational = nullptr)
      : datastores_(datastores), nacm_(nacm), urls_(urls),
        notifications_(notifications), with_defaults_(with_defaults),
        operational_(operational) {}

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
};

}  // namespace yang::netconf

#endif  // YANG_NETCONF_SERVER_H_
