// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_CONFIGURATION_BACKEND_H_
#define DANGD_CONFIGURATION_BACKEND_H_

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "yang/netconf_datastore.h"

namespace dangd {

/** In-memory backend that replaces its working configuration and describes it. */
class EnglishConfigurationBackend final
    : public yang::netconf::RunningConfigBackend {
 public:
  /** Initializes the backend with the already validated running document. */
  explicit EnglishConfigurationBackend(yang::config::ConfigDocument initial)
      : working_(std::move(initial)) {}

  /** Records exact changes in English and atomically replaces the working copy. */
  void Replace(const yang::config::RuntimeSchema& schema,
               const yang::config::ConfigDocument& before,
               const yang::config::ConfigDocument& after,
               std::span<const yang::config::ChangeEvent> changes) override;

  /** Returns an immutable snapshot of the backend working configuration. */
  [[nodiscard]] yang::config::ConfigDocument Working() const;
  /** Returns and clears all descriptions accumulated since the preceding call. */
  [[nodiscard]] std::vector<std::string> DrainDeltas();

 private:
  mutable std::mutex mutex_;
  yang::config::ConfigDocument working_;
  std::vector<std::string> deltas_;
};

}  // namespace dangd

#endif  // DANGD_CONFIGURATION_BACKEND_H_
