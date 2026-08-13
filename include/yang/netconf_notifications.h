// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_NOTIFICATIONS_H_
#define YANG_NETCONF_NOTIFICATIONS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "yang/nacm.h"

namespace yang::netconf {

/** Configured RFC 5277 event stream and replay/resource policy. */
struct NotificationStreamConfig {
  std::string name = "NETCONF";
  bool replay_supported = true;
  std::size_t replay_event_limit = 1024;
};

/** Validated subscription parameters supplied by the NETCONF server. */
struct SubscriptionRequest {
  std::uint32_t session_id = 0;
  std::string username;
  std::vector<std::string> external_groups;
  std::string stream = "NETCONF";
  std::optional<std::string> filter_xml;
  std::optional<std::chrono::system_clock::time_point> start_time;
  std::optional<std::chrono::system_clock::time_point> stop_time;
};

/** Result of creating an RFC 5277 subscription. */
struct SubscriptionResult {
  bool ok = false;
  std::string error;
  std::string error_tag;
  std::string bad_element;
};

/** Thread-safe transport-neutral RFC 5277 stream and subscription manager. */
class NotificationManager {
 public:
  explicit NotificationManager(const NacmPolicy* nacm = nullptr,
      std::size_t maximum_queued_events = 1024,
      std::size_t maximum_queued_bytes = 16 * 1024 * 1024);
  [[nodiscard]] bool AddStream(NotificationStreamConfig stream);
  [[nodiscard]] SubscriptionResult Subscribe(SubscriptionRequest request,
      std::chrono::system_clock::time_point now =
          std::chrono::system_clock::now());
  /** Publishes one notification-specific XML element. */
  [[nodiscard]] bool Publish(std::string_view stream,
      std::string_view module_name, std::string_view notification_name,
      std::string_view content_xml,
      std::chrono::system_clock::time_point event_time =
          std::chrono::system_clock::now(), bool default_deny_all = false);
  /** Removes and returns queued complete notification documents. */
  [[nodiscard]] std::vector<std::string> Drain(std::uint32_t session_id,
      std::chrono::system_clock::time_point now =
          std::chrono::system_clock::now());
  void RemoveSession(std::uint32_t session_id);
  [[nodiscard]] bool configured() const noexcept;

 private:
  struct Event {
    std::chrono::system_clock::time_point time;
    std::string module;
    std::string name;
    std::string xml;
    bool default_deny_all = false;
  };
  struct Stream {
    NotificationStreamConfig config;
    std::deque<Event> replay;
  };
  struct Subscription {
    SubscriptionRequest request;
    std::deque<std::string> queue;
    std::size_t queued_bytes = 0;
    bool terminated = false;
  };
  const NacmPolicy* nacm_;
  std::size_t maximum_queued_events_;
  std::size_t maximum_queued_bytes_;
  mutable std::mutex mutex_;
  std::map<std::string, Stream, std::less<>> streams_;
  std::map<std::uint32_t, Subscription> subscriptions_;
};

/** Parses an RFC 3339 timestamp accepted by RFC 5277. */
[[nodiscard]] std::optional<std::chrono::system_clock::time_point>
ParseNotificationTime(std::string_view value);
/** Formats an RFC 5277 eventTime in UTC. */
[[nodiscard]] std::string FormatNotificationTime(
    std::chrono::system_clock::time_point value);

}  // namespace yang::netconf

#endif  // YANG_NETCONF_NOTIFICATIONS_H_
