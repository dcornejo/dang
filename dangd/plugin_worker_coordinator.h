// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGIN_WORKER_COORDINATOR_H_
#define DANGD_PLUGIN_WORKER_COORDINATOR_H_

#include <string>
#include <vector>

#include "dangd/hardware_transaction.h"
#include "dangd/plugin_manager.h"

namespace dangd {

class PluginWorkerClient;

/** One affected worker and its already copied discovery metadata. */
struct PluginWorkerParticipant {
  PluginManifest manifest;
  PluginWorkerClient* client = nullptr;
};

/** Builds and executes one dependency-ordered plan across plugin workers. */
class PluginWorkerCoordinator {
 public:
  PluginWorkerCoordinator() = default;
  ~PluginWorkerCoordinator();
  PluginWorkerCoordinator(const PluginWorkerCoordinator&) = delete;
  PluginWorkerCoordinator& operator=(const PluginWorkerCoordinator&) = delete;
  /** Copies worker action descriptions and creates the global action graph. */
  [[nodiscard]] HardwareTransactionResult Plan(
      std::vector<PluginWorkerParticipant> participants);
  /** Executes the retained plan, compensating prior actions on failure. */
  [[nodiscard]] HardwareTransactionResult Apply();
  /** Validates all applied reports, then commits or compensates the plan. */
  [[nodiscard]] PluginApplyResult Reconcile(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& proposed);
  /** Compensates retained actions and releases every worker preparation. */
  void Abort() noexcept;

 private:
  HardwareTransactionPlanner planner_;
  std::vector<PluginWorkerParticipant> participants_;
};

}  // namespace dangd

#endif  // DANGD_PLUGIN_WORKER_COORDINATOR_H_
