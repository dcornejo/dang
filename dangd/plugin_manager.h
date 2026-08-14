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

/** Loads ABI-v1 plugins and coordinates their configuration transactions. */
class PluginManager {
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

 private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace dangd

#endif  // DANGD_PLUGIN_MANAGER_H_
