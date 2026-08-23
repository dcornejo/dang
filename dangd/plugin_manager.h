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

/** One ABI-v3 operational callback result, including provider attribution. */
struct PluginOperationalFragment {
  std::string provider;
  std::string data_xml;
  std::optional<std::string> error;
  std::string error_path;
};

/** Failure exposed in the current operational reconciliation snapshot. */
struct OperationalProviderFailure {
  std::string provider;
  std::string stage;
  std::string instance_path;
  std::string reason;
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
  /** Applies prepared plugins in dependency order with reverse rollback. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding> Apply();
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
