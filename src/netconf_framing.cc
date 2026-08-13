// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_framing.h"

#include <charconv>
#include <limits>
#include <utility>

#include <pugixml.hpp>

namespace yang::netconf {
namespace {

constexpr std::string_view kEndMarker = "]]>]]>";
constexpr std::string_view kBase11Capability =
    "urn:ietf:params:netconf:base:1.1";
constexpr std::string_view kBase10Capability =
    "urn:ietf:params:netconf:base:1.0";
constexpr std::string_view kNetconfNamespace =
    "urn:ietf:params:xml:ns:netconf:base:1.0";

std::string_view LocalName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() &&
         (value.front() == ' ' || value.front() == '\t' ||
          value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' ||
          value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1);
  }
  return value;
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

bool ClientSupportsBase11(std::string_view xml, std::string* error) {
  pugi::xml_document document;
  const pugi::xml_parse_result parsed =
      document.load_buffer(xml.data(), xml.size(), pugi::parse_default);
  if (!parsed || LocalName(document.document_element().name()) != "hello") {
    *error = "first NETCONF message must be a well-formed hello element";
    return false;
  }
  const pugi::xml_node hello = document.document_element();
  if (NamespaceFor(hello) != kNetconfNamespace) {
    *error = "client hello is not in the NETCONF base namespace";
    return false;
  }
  bool has_capabilities = false;
  bool has_base = false;
  bool base11 = false;
  for (const pugi::xml_node child : hello.children()) {
    if (LocalName(child.name()) != "capabilities") continue;
    has_capabilities = true;
    for (const pugi::xml_node capability : child.children()) {
      const std::string_view value = Trim(capability.text().as_string());
      if (LocalName(capability.name()) == "capability" &&
          value == kBase11Capability) {
        base11 = true;
        has_base = true;
      } else if (LocalName(capability.name()) == "capability" &&
                 value == kBase10Capability) {
        has_base = true;
      }
    }
  }
  if (!has_capabilities) *error = "client hello is missing capabilities";
  else if (!has_base) *error = "client hello has no supported NETCONF base capability";
  for (const pugi::xml_node child : hello.children()) {
    if (LocalName(child.name()) == "session-id")
      *error = "client hello must not contain a session-id";
  }
  return base11;
}

DecodeResult Failure(std::string message, bool* failed) {
  *failed = true;
  return {{}, std::move(message)};
}

}  // namespace

DecodeResult FramingDecoder::Feed(std::string_view bytes) {
  if (failed_) return {{}, "framing decoder is already failed"};
  if (bytes.size() > maximum_message_size_ &&
      bytes.size() - maximum_message_size_ > 32) {
    return Failure("NETCONF framing input exceeds the per-feed work limit",
                   &failed_);
  }
  buffer_.append(bytes);
  DecodeResult result;
  if (version_ == BaseVersion::kBase10) {
    while (true) {
      const std::size_t marker = buffer_.find(kEndMarker);
      if (marker == std::string::npos) break;
      if (marker > maximum_message_size_)
        return Failure("NETCONF message exceeds the size limit", &failed_);
      result.messages.push_back(buffer_.substr(0, marker));
      buffer_.erase(0, marker + kEndMarker.size());
    }
    if (buffer_.size() > maximum_message_size_)
      return Failure("NETCONF message exceeds the size limit", &failed_);
    return result;
  }

  while (true) {
    if (buffer_.size() < 2) return result;
    if (!buffer_.starts_with("\n#"))
      return Failure("chunked NETCONF message must begin with LF HASH", &failed_);
    if (buffer_.starts_with("\n##")) {
      if (buffer_.size() < 4) return result;
      if (!buffer_.starts_with("\n##\n"))
        return Failure("invalid NETCONF end-of-chunks marker", &failed_);
      if (chunked_message_.empty())
        return Failure("chunked NETCONF message contains no chunks", &failed_);
      result.messages.push_back(std::move(chunked_message_));
      chunked_message_.clear();
      buffer_.erase(0, 4);
      continue;
    }
    const std::size_t line_end = buffer_.find('\n', 2);
    if (line_end == std::string::npos) {
      if (buffer_.size() > 16)
        return Failure("NETCONF chunk header is too long", &failed_);
      return result;
    }
    const std::string_view size_text(buffer_.data() + 2, line_end - 2);
    if (size_text.empty() || size_text.front() == '0')
      return Failure("invalid NETCONF chunk size", &failed_);
    std::uint64_t chunk_size = 0;
    const auto converted = std::from_chars(
        size_text.data(), size_text.data() + size_text.size(), chunk_size);
    if (converted.ec != std::errc() ||
        converted.ptr != size_text.data() + size_text.size() ||
        chunk_size > std::numeric_limits<std::uint32_t>::max()) {
      return Failure("invalid NETCONF chunk size", &failed_);
    }
    if (chunk_size > maximum_message_size_ - chunked_message_.size())
      return Failure("NETCONF message exceeds the size limit", &failed_);
    const std::size_t header_size = line_end + 1;
    if (chunk_size > buffer_.size() - header_size) return result;
    chunked_message_.append(buffer_, header_size,
                            static_cast<std::size_t>(chunk_size));
    buffer_.erase(0, header_size + static_cast<std::size_t>(chunk_size));
  }
}

std::string FrameMessage(BaseVersion version, std::string_view xml) {
  if (version == BaseVersion::kBase10) return FrameHello(xml);
  return "\n#" + std::to_string(xml.size()) + "\n" + std::string(xml) +
         "\n##\n";
}

std::string FrameHello(std::string_view xml) {
  return std::string(xml) + std::string(kEndMarker);
}

NetconfSession::NetconfSession(NetconfServer& server, std::uint32_t session_id,
                               std::string authenticated_identity,
                               std::size_t maximum_message_size)
    : server_(server), session_id_(session_id),
      identity_(std::move(authenticated_identity)),
      maximum_message_size_(maximum_message_size) {
  registered_ = server_.RegisterSession(session_id_, identity_);
}

NetconfSession::~NetconfSession() { TransportClosed(); }

std::string NetconfSession::Start() const {
  if (!registered_) return {};
  return FrameHello(server_.ServerHello(session_id_));
}

SessionOutput NetconfSession::Receive(std::string_view bytes) {
  SessionOutput pending = Poll();
  if (pending.close_transport) return pending;
  SessionOutput output;
  std::string post_hello_bytes;
  if (closed_) {
    output.close_transport = true;
    output.error = "NETCONF session is closed";
    return output;
  }
  if (!negotiated_) {
    hello_buffer_.append(bytes);
    const std::size_t marker = hello_buffer_.find(kEndMarker);
    if (marker == std::string::npos) {
      if (hello_buffer_.size() <= maximum_message_size_) return output;
      output.error = "NETCONF hello exceeds the message limit";
      output.close_transport = true;
      TransportClosed();
      return output;
    }
    if (marker > maximum_message_size_) {
      output.error = "NETCONF hello exceeds the message limit";
      output.close_transport = true;
      TransportClosed();
      return output;
    }
    std::string error;
    const bool base11 = ClientSupportsBase11(
        std::string_view(hello_buffer_).substr(0, marker), &error);
    if (!error.empty()) {
      output.error = std::move(error);
      output.close_transport = true;
      TransportClosed();
      return output;
    }
    version_ = base11 ? BaseVersion::kBase11 : BaseVersion::kBase10;
    negotiated_ = true;
    decoder_.emplace(version_, maximum_message_size_);
    hello_buffer_.erase(0, marker + kEndMarker.size());
    post_hello_bytes = hello_buffer_;
    hello_buffer_.clear();
    bytes = post_hello_bytes;
  }
  DecodeResult decoded = decoder_->Feed(bytes);
  if (decoded.error) {
    output.error = std::move(decoded.error);
    output.close_transport = true;
    TransportClosed();
    return output;
  }
  for (const std::string& message : decoded.messages) {
    const std::string owner = std::to_string(session_id_);
    RpcResponse response = server_.Process(
        {session_id_, identity_, owner, external_groups_}, message);
    output.bytes_to_send.push_back(FrameMessage(version_, response.xml));
    if (response.close_session) {
      output.close_transport = true;
      TransportClosed();
      break;
    }
  }
  return output;
}

SessionOutput NetconfSession::Poll() {
  SessionOutput output;
  if (!registered_) {
    output.error = "NETCONF session ID is invalid or already active";
    output.close_transport = true;
    return output;
  }
  if (closed_) {
    output.error = "NETCONF session is closed";
    output.close_transport = true;
    return output;
  }
  if (server_.CloseRequested(session_id_)) {
    output.close_transport = true;
    TransportClosed();
  } else if (negotiated_) {
    for (const std::string& notification :
         server_.DrainNotifications(session_id_)) {
      output.bytes_to_send.push_back(FrameMessage(version_, notification));
    }
  }
  return output;
}

void NetconfSession::TransportClosed() {
  if (closed_) return;
  closed_ = true;
  if (!registered_) return;
  server_.SessionClosed(session_id_, std::to_string(session_id_));
  registered_ = false;
}

}  // namespace yang::netconf
