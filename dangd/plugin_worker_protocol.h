// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGIN_WORKER_PROTOCOL_H_
#define DANGD_PLUGIN_WORKER_PROTOCOL_H_

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace dangd {

/** Result category for one framed plugin-worker transport operation. */
enum class WorkerIoStatus {
  kOk,
  kTimeout,
  kPeerClosed,
  kProtocolError,
  kSystemError,
};

/** Host-owned result of reading one complete worker protocol frame. */
struct WorkerReadResult {
  WorkerIoStatus status = WorkerIoStatus::kSystemError;
  std::string payload;
  std::string error;
};

/**
 * Writes one four-byte-length-prefixed worker message before `deadline`.
 *
 * The payload is rejected before any bytes are written when it exceeds
 * `maximum_payload_bytes`. The descriptor must be a connected POSIX socket.
 */
[[nodiscard]] WorkerIoStatus WriteWorkerFrame(
    int descriptor, std::string_view payload,
    std::chrono::steady_clock::time_point deadline,
    std::size_t maximum_payload_bytes, std::string* error);

/**
 * Reads one complete bounded worker message before `deadline`.
 *
 * EOF before the first header byte is reported as `kPeerClosed`; EOF within a
 * header or payload is a protocol error because the peer emitted a truncated
 * frame. Oversized lengths are rejected without allocating their payload.
 */
[[nodiscard]] WorkerReadResult ReadWorkerFrame(
    int descriptor, std::chrono::steady_clock::time_point deadline,
    std::size_t maximum_payload_bytes);

}  // namespace dangd

#endif  // DANGD_PLUGIN_WORKER_PROTOCOL_H_
