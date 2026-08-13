// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_FRAMING_H_
#define YANG_NETCONF_FRAMING_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "yang/netconf_server.h"

namespace yang::netconf {

enum class BaseVersion { kBase10, kBase11 };

struct DecodeResult {
  std::vector<std::string> messages;
  std::optional<std::string> error;
};

/** Incremental RFC 6242 EOM or chunked-message decoder. */
class FramingDecoder {
 public:
  explicit FramingDecoder(BaseVersion version,
                          std::size_t maximum_message_size = 16 * 1024 * 1024)
      : version_(version), maximum_message_size_(maximum_message_size) {}

  [[nodiscard]] DecodeResult Feed(std::string_view bytes);
  [[nodiscard]] bool failed() const noexcept { return failed_; }

 private:
  BaseVersion version_;
  std::size_t maximum_message_size_;
  std::string buffer_;
  std::string chunked_message_;
  bool failed_ = false;
};

/** Frames one non-empty XML document using the negotiated RFC 6242 mechanism. */
[[nodiscard]] std::string FrameMessage(BaseVersion version,
                                       std::string_view xml);
/** Hello messages always use the backwards-compatible EOM delimiter. */
[[nodiscard]] std::string FrameHello(std::string_view xml);

struct SessionOutput {
  std::vector<std::string> bytes_to_send;
  bool close_transport = false;
  std::optional<std::string> error;
};

/** Incremental transport-neutral NETCONF session and framing state machine. */
class NetconfSession {
 public:
  NetconfSession(NetconfServer& server, std::uint32_t session_id,
                 std::string authenticated_identity,
                 std::size_t maximum_message_size = 16 * 1024 * 1024);
  ~NetconfSession();

  NetconfSession(const NetconfSession&) = delete;
  NetconfSession& operator=(const NetconfSession&) = delete;

  [[nodiscard]] std::string Start() const;
  [[nodiscard]] SessionOutput Receive(std::string_view bytes);
  /** Observes asynchronous shutdown requests such as kill-session. */
  [[nodiscard]] SessionOutput Poll();
  void TransportClosed();
  /** Replaces transport-authenticated groups used by RFC 8341 NACM. */
  void set_external_groups(std::vector<std::string> groups) {
    external_groups_ = std::move(groups);
  }
  [[nodiscard]] bool valid() const noexcept { return registered_; }
  [[nodiscard]] bool negotiated() const noexcept { return negotiated_; }
  [[nodiscard]] BaseVersion version() const noexcept { return version_; }

 private:
  NetconfServer& server_;
  std::uint32_t session_id_;
  std::string identity_;
  std::vector<std::string> external_groups_;
  std::size_t maximum_message_size_;
  std::string hello_buffer_;
  std::optional<FramingDecoder> decoder_;
  BaseVersion version_ = BaseVersion::kBase10;
  bool negotiated_ = false;
  bool closed_ = false;
  bool registered_ = false;
};

}  // namespace yang::netconf

#endif  // YANG_NETCONF_FRAMING_H_
