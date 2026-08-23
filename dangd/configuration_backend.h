// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_CONFIGURATION_BACKEND_H_
#define DANGD_CONFIGURATION_BACKEND_H_

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "yang/netconf_datastore.h"
#include "yang/nacm.h"

#include "dangd/plugin_manager.h"

namespace dangd {

/** In-memory backend that replaces its working configuration and describes it. */
class EnglishConfigurationBackend final
    : public yang::netconf::RunningConfigBackend {
 public:
  /** Initializes the backend with the already validated running document. */
  EnglishConfigurationBackend(yang::config::ConfigDocument initial,
                              PluginManager* plugins,
                              yang::netconf::NacmPolicy* nacm,
                              bool managed_nacm)
      : plugins_(plugins), nacm_(nacm), managed_nacm_(managed_nacm),
        working_xml_(initial.ToXml()), working_(std::move(initial)) {}

  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  PrepareReplacement(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& before,
      const yang::config::ConfigDocument& after,
      std::span<const yang::config::ChangeEvent> changes) override;

  /** Records exact changes in English and atomically replaces the working copy. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding> Replace(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& before,
      const yang::config::ConfigDocument& after,
      std::span<const yang::config::ChangeEvent> changes) override;
  void AbortPreparedReplacement() noexcept override;

  /** Returns an immutable snapshot of the backend working configuration. */
  [[nodiscard]] yang::config::ConfigDocument Working() const;
  /** Returns applied XML while retaining validated instance metadata. */
  [[nodiscard]] std::string WorkingXml() const;
  /** Returns and clears all descriptions accumulated since the preceding call. */
  [[nodiscard]] std::vector<std::string> DrainDeltas();

 private:
  mutable std::mutex mutex_;
  PluginManager* plugins_ = nullptr;
  yang::netconf::NacmPolicy* nacm_ = nullptr;
  bool managed_nacm_ = false;
  std::optional<yang::netconf::NacmPolicy> prepared_nacm_;
  std::string working_xml_;
  yang::config::ConfigDocument working_;
  std::vector<std::string> deltas_;
};

}  // namespace dangd

#endif  // DANGD_CONFIGURATION_BACKEND_H_
