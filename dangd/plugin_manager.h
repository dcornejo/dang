// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGIN_MANAGER_H_
#define DANGD_PLUGIN_MANAGER_H_

#include <filesystem>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "dangd/plugin_api.h"
#include "yang/config_edit.h"
#include "yang/netconf_server.h"

namespace dangd {

/** Host-owned copy of one plugin-provided YANG source descriptor. */
struct PluginYangSource {
  std::string plugin_name;
  std::string module_name;
  std::optional<std::string> revision;
  std::string source;
  std::string source_uri;
  std::uint32_t role = DANG_YANG_IMPORT_ONLY_V1;
  std::vector<std::string> enabled_features;
};

/** Host-owned plugin identity and callback capabilities discovered at load. */
struct PluginManifest {
  std::string plugin_name;
  std::uint32_t abi_version = 0;
  std::vector<std::string> modules;
  std::vector<std::string> dependencies;
  bool supports_operations = false;
  bool supports_operational_data = false;
  bool supports_hardware_actions = false;
  bool supports_applied_reconciliation = false;
};

/** One ABI-v3 operational callback result, including provider attribution. */
struct PluginOperationalFragment {
  std::string provider;
  std::string data_xml;
  std::optional<std::string> error;
  std::string error_path;
  bool complete = false;
};

/** Failure exposed in the current operational reconciliation snapshot. */
struct OperationalProviderFailure {
  std::string provider;
  std::string stage;
  std::string instance_path;
  std::string reason;
};

/** One plugin-attributed post-apply configuration result. */
struct ConfigurationOutcome {
  std::string provider;
  std::string instance_path;
  std::uint32_t disposition = DANG_CONFIGURATION_APPLIED_V1;
  std::string reason;
};

/** Successful applied snapshot, or a transaction failure. */
struct PluginApplyResult {
  std::optional<yang::config::ValidationFinding> error;
  std::optional<yang::config::ConfigDocument> applied;
  std::string applied_xml;
};

/** Copied worker-owned hardware action safe to send across IPC. */
struct PluginWorkerHardwareAction {
  std::string action_id;
  std::string instance_path;
  std::uint32_t action_class = DANG_HARDWARE_NORMAL_V1;
  std::vector<std::string> dependencies;
};

/** Copied ABI-v6 applied-state report safe to send across IPC. */
struct PluginWorkerAppliedReport {
  std::string applied_xml;
  std::vector<ConfigurationOutcome> outcomes;
};

/** Loads ABI-v1 plugins and coordinates their configuration transactions. */
class PluginManager : public yang::netconf::OperationProvider {
 public:
  /** Constructs an empty plugin registry. */
  PluginManager();
  ~PluginManager();
  PluginManager(const PluginManager&) = delete;
  PluginManager& operator=(const PluginManager&) = delete;

  /** Loads and copies one plugin's manifest and sources. */
  [[nodiscard]] bool Load(const std::filesystem::path& path,
                          std::vector<std::string>* errors);
  /** Validates unique ownership and all declared runtime dependencies. */
  [[nodiscard]] bool ValidateDependencies(std::vector<std::string>* errors) const;
  /** Returns all copied sources in plugin discovery order. */
  [[nodiscard]] const std::vector<PluginYangSource>& yang_sources() const;
  /** Returns copied plugin manifests in successful load order. */
  [[nodiscard]] const std::vector<PluginManifest>& manifests() const;
  /** Collects attributed operational callback results from ABI-v3 plugins. */
  [[nodiscard]] std::vector<PluginOperationalFragment> OperationalData() const;
  /** Returns modeled state for hardware changes that could not be rolled back. */
  [[nodiscard]] std::string ReconciliationData(
      std::span<const OperationalProviderFailure> provider_failures = {}) const;
  /** Prepares and validates every plugin affected by a proposed replacement. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding> Prepare(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& before,
      const yang::config::ConfigDocument& after,
      std::span<const yang::config::ChangeEvent> changes);
  /** Worker-only phase one: retains one plugin's opaque prepared state. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  PrepareWorkerTransaction(std::string before_xml, std::string proposed_xml,
                           std::string changes_json);
  /** Worker-only phase two: validates the retained opaque preparation. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  ValidateWorkerTransaction();
  /** Copies this worker's action plan without exposing plugin-owned memory. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  WorkerHardwareActions(std::vector<PluginWorkerHardwareAction>* actions);
  /** Applies one named action against the retained worker preparation. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  ApplyWorkerHardwareAction(std::string_view action_id);
  /** Compensates one named action against the retained preparation. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  RollbackWorkerHardwareAction(std::string_view action_id);
  /** Copies this worker's applied-state report while preparation is retained. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  ReconcileWorkerApplied(std::string_view current_xml,
                         PluginWorkerAppliedReport* report);
  /** Applies and schema-validates ABI-v6 reports of actual backend state. */
  [[nodiscard]] PluginApplyResult Apply(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& proposed);
  /** Releases every retained preparation without applying it. */
  void Abort() noexcept;
  [[nodiscard]] yang::netconf::OperationResult InvokeRpc(
      const yang::netconf::RpcSessionContext& session,
      const yang::config::RuntimeSchemaNode& operation,
      std::string_view operation_xml) override;
  [[nodiscard]] yang::netconf::OperationResult InvokeAction(
      const yang::netconf::RpcSessionContext& session,
      const yang::config::RuntimeSchemaNode& action,
      std::string_view instance_path, std::string_view action_xml) override;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace dangd

#endif  // DANGD_PLUGIN_MANAGER_H_
