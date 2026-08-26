// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGIN_WORKER_RUNTIME_H_
#define DANGD_PLUGIN_WORKER_RUNTIME_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include "dangd/plugin_manager.h"
#include "dangd/plugin_worker_coordinator.h"

namespace dangd {

class PluginWorkerClient;

/** Live PluginRuntime whose plugin code exists only in supervised workers. */
class PluginWorkerRuntime final : public PluginRuntime {
 public:
  /** Spawns one worker per plugin and validates copied discovery metadata. */
  [[nodiscard]] static std::unique_ptr<PluginWorkerRuntime> Load(
      const std::filesystem::path& worker_executable,
      const std::vector<std::filesystem::path>& plugins,
      std::vector<std::string>* errors,
      std::chrono::milliseconds startup_timeout = std::chrono::seconds(5),
      std::chrono::milliseconds request_timeout = std::chrono::seconds(5));
  ~PluginWorkerRuntime() override;

  [[nodiscard]] const std::vector<PluginYangSource>& yang_sources()
      const override;
  [[nodiscard]] const std::vector<PluginManifest>& manifests() const override;
  [[nodiscard]] std::vector<PluginOperationalFragment> OperationalData()
      const override;
  [[nodiscard]] std::string ReconciliationData(
      std::span<const OperationalProviderFailure> failures = {}) const override;
  [[nodiscard]] std::optional<yang::config::ValidationFinding> Prepare(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& before,
      const yang::config::ConfigDocument& after,
      std::span<const yang::config::ChangeEvent> changes) override;
  [[nodiscard]] PluginApplyResult Apply(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& proposed) override;
  void Abort() noexcept override;
  [[nodiscard]] yang::netconf::OperationResult InvokeRpc(
      const yang::netconf::RpcSessionContext& session,
      const yang::config::RuntimeSchemaNode& operation,
      std::string_view operation_xml) override;
  [[nodiscard]] yang::netconf::OperationResult InvokeAction(
      const yang::netconf::RpcSessionContext& session,
      const yang::config::RuntimeSchemaNode& operation,
      std::string_view instance_path,
      std::string_view operation_xml) override;

 private:
  struct Entry;
  PluginWorkerRuntime() = default;
  std::vector<std::unique_ptr<Entry>> entries_;
  std::vector<PluginYangSource> sources_;
  std::vector<PluginManifest> manifests_;
  std::vector<std::size_t> affected_;
  PluginWorkerCoordinator coordinator_;
  mutable std::recursive_mutex worker_mutex_;
  mutable std::mutex reconciliation_mutex_;
  std::vector<HardwareRemnant> remnants_;
  std::vector<ConfigurationOutcome> outcomes_;
};

}  // namespace dangd

#endif  // DANGD_PLUGIN_WORKER_RUNTIME_H_
