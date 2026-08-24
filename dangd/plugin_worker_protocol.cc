// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_worker_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dangd {
namespace {

enum class WaitResult { kReady, kTimeout, kClosed, kError };

WaitResult WaitFor(int descriptor, short events,
                   std::chrono::steady_clock::time_point deadline,
                   std::string* error) {
  while (true) {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
      return WaitResult::kTimeout;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining + std::chrono::milliseconds(1));
    const auto bounded = std::min<std::int64_t>(
        milliseconds.count(), std::numeric_limits<int>::max());
    pollfd descriptor_state{descriptor, events, 0};
    const int polled = poll(&descriptor_state, 1, static_cast<int>(bounded));
    if (polled < 0) {
      if (errno == EINTR) continue;
      if (error) *error = std::strerror(errno);
      return WaitResult::kError;
    }
    if (polled == 0) return WaitResult::kTimeout;
    if ((descriptor_state.revents & POLLNVAL) != 0) {
      if (error) *error = "invalid worker socket";
      return WaitResult::kError;
    }
    if ((descriptor_state.revents & events) != 0) return WaitResult::kReady;
    if ((descriptor_state.revents & (POLLHUP | POLLERR)) != 0)
      return WaitResult::kClosed;
  }
}

WorkerIoStatus WriteAll(int descriptor, const unsigned char* data,
                        std::size_t size,
                        std::chrono::steady_clock::time_point deadline,
                        std::string* error) {
  std::size_t offset = 0;
  while (offset < size) {
    const WaitResult waited = WaitFor(descriptor, POLLOUT, deadline, error);
    if (waited == WaitResult::kTimeout) return WorkerIoStatus::kTimeout;
    if (waited == WaitResult::kClosed) return WorkerIoStatus::kPeerClosed;
    if (waited == WaitResult::kError) return WorkerIoStatus::kSystemError;
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    const ssize_t written = send(descriptor, data + offset, size - offset,
                                 kSendFlags);
    if (written < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (errno == EPIPE || errno == ECONNRESET)
        return WorkerIoStatus::kPeerClosed;
      if (error) *error = std::strerror(errno);
      return WorkerIoStatus::kSystemError;
    }
    if (written == 0) return WorkerIoStatus::kPeerClosed;
    offset += static_cast<std::size_t>(written);
  }
  return WorkerIoStatus::kOk;
}

WorkerIoStatus ReadAll(int descriptor, unsigned char* data, std::size_t size,
                       std::chrono::steady_clock::time_point deadline,
                       bool frame_started, std::string* error) {
  std::size_t offset = 0;
  while (offset < size) {
    const WaitResult waited = WaitFor(descriptor, POLLIN, deadline, error);
    if (waited == WaitResult::kTimeout) return WorkerIoStatus::kTimeout;
    if (waited == WaitResult::kError) return WorkerIoStatus::kSystemError;
    const ssize_t received = recv(descriptor, data + offset, size - offset, 0);
    if (received < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      if (errno == ECONNRESET)
        return offset == 0 && !frame_started ? WorkerIoStatus::kPeerClosed
                                             : WorkerIoStatus::kProtocolError;
      if (error) *error = std::strerror(errno);
      return WorkerIoStatus::kSystemError;
    }
    if (received == 0)
      return offset == 0 && !frame_started ? WorkerIoStatus::kPeerClosed
                                           : WorkerIoStatus::kProtocolError;
    offset += static_cast<std::size_t>(received);
    frame_started = true;
  }
  return WorkerIoStatus::kOk;
}

}  // namespace

WorkerIoStatus WriteWorkerFrame(
    int descriptor, std::string_view payload,
    std::chrono::steady_clock::time_point deadline,
    std::size_t maximum_payload_bytes, std::string* error) {
  if (payload.size() > maximum_payload_bytes ||
      payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    if (error) *error = "worker message exceeds the payload limit";
    return WorkerIoStatus::kProtocolError;
  }
  const std::uint32_t size = static_cast<std::uint32_t>(payload.size());
  const std::array<unsigned char, 4> header{
      static_cast<unsigned char>((size >> 24U) & 0xffU),
      static_cast<unsigned char>((size >> 16U) & 0xffU),
      static_cast<unsigned char>((size >> 8U) & 0xffU),
      static_cast<unsigned char>(size & 0xffU)};
  WorkerIoStatus status =
      WriteAll(descriptor, header.data(), header.size(), deadline, error);
  if (status != WorkerIoStatus::kOk) return status;
  return WriteAll(descriptor,
                  reinterpret_cast<const unsigned char*>(payload.data()),
                  payload.size(), deadline, error);
}

WorkerReadResult ReadWorkerFrame(
    int descriptor, std::chrono::steady_clock::time_point deadline,
    std::size_t maximum_payload_bytes) {
  WorkerReadResult result;
  std::array<unsigned char, 4> header{};
  result.status = ReadAll(descriptor, header.data(), header.size(), deadline,
                          false, &result.error);
  if (result.status != WorkerIoStatus::kOk) {
    if (result.status == WorkerIoStatus::kProtocolError)
      result.error = "worker closed a partial frame header";
    return result;
  }
  const std::uint32_t size =
      (static_cast<std::uint32_t>(header[0]) << 24U) |
      (static_cast<std::uint32_t>(header[1]) << 16U) |
      (static_cast<std::uint32_t>(header[2]) << 8U) |
      static_cast<std::uint32_t>(header[3]);
  if (size > maximum_payload_bytes) {
    result.status = WorkerIoStatus::kProtocolError;
    result.error = "worker message exceeds the payload limit";
    return result;
  }
  result.payload.resize(size);
  result.status = ReadAll(
      descriptor, reinterpret_cast<unsigned char*>(result.payload.data()),
      result.payload.size(), deadline, true, &result.error);
  if (result.status == WorkerIoStatus::kProtocolError)
    result.error = "worker closed a partial frame payload";
  return result;
}

}  // namespace dangd
