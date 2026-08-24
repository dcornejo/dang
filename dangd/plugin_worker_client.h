// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGIN_WORKER_CLIENT_H_
#define DANGD_PLUGIN_WORKER_CLIENT_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "dangd/plugin_manager.h"

namespace dangd {

/** Copied discovery response from one isolated plugin worker. */
struct PluginWorkerDiscovery {
  PluginManifest manifest;
  std::vector<PluginYangSource> sources;
};

/** Operational fragments or a worker transport/supervision failure. */
struct PluginWorkerOperationalResult {
  std::vector<PluginOperationalFragment> fragments;
  std::optional<std::string> worker_error;
};

/** Plugin rejection or worker failure for one transaction phase. */
struct PluginWorkerTransactionResult {
  std::optional<yang::config::ValidationFinding> finding;
  std::optional<std::string> worker_error;
  /** Returns true only when neither the plugin nor worker rejected the phase. */
  [[nodiscard]] bool ok() const noexcept {
    return !finding.has_value() && !worker_error.has_value();
  }
};

/**
 * Owns and supervises one long-lived out-of-process plugin instance.
 *
 * Requests are serialized because a worker owns one stateful ABI context. A
 * timeout, crash, truncated response, or protocol violation permanently marks
 * the client unhealthy and terminates/reaps the worker before returning.
 */
class PluginWorkerClient {
 public:
  struct Options {
    std::filesystem::path worker_executable;
    std::filesystem::path plugin;
    std::chrono::milliseconds startup_timeout = std::chrono::seconds(5);
    std::chrono::milliseconds request_timeout = std::chrono::seconds(5);
  };

  /** Spawns a worker and validates its ready handshake. */
  [[nodiscard]] static std::unique_ptr<PluginWorkerClient> Start(
      Options options, std::vector<std::string>* errors);
  ~PluginWorkerClient();
  PluginWorkerClient(const PluginWorkerClient&) = delete;
  PluginWorkerClient& operator=(const PluginWorkerClient&) = delete;

  /** Requests and validates copied manifest/YANG discovery data. */
  [[nodiscard]] std::optional<PluginWorkerDiscovery> Discover(
      std::string* error);
  /** Invokes operational publication inside the worker. */
  [[nodiscard]] PluginWorkerOperationalResult OperationalData();
  /** Retains plugin-owned preparation for a copied transaction. */
  [[nodiscard]] PluginWorkerTransactionResult Prepare(
      std::string before_xml, std::string proposed_xml,
      std::string changes_json);
  /** Validates the preparation retained by the preceding prepare request. */
  [[nodiscard]] PluginWorkerTransactionResult Validate();
  /** Releases retained preparation without applying it. */
  [[nodiscard]] std::optional<std::string> Abort();
  /** Returns whether the worker remains usable after preceding requests. */
  [[nodiscard]] bool healthy() const noexcept;

 private:
  PluginWorkerClient(Options options, int descriptor, int process);
  struct ExchangeResult;
  [[nodiscard]] ExchangeResult Exchange(std::string request);
  [[nodiscard]] PluginWorkerTransactionResult Transaction(
      std::string request);
  void Terminate() noexcept;
  void TerminateLocked() noexcept;

  Options options_;
  int descriptor_ = -1;
  int process_ = -1;
  mutable std::mutex mutex_;
  bool healthy_ = true;
};

}  // namespace dangd

#endif  // DANGD_PLUGIN_WORKER_CLIENT_H_
