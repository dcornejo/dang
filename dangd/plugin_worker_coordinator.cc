// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_worker_coordinator.h"

#include "dangd/plugin_worker_client.h"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <utility>

namespace dangd {
namespace {

std::optional<std::string> Failure(
    const PluginWorkerTransactionResult& result) {
  if (result.worker_error) return *result.worker_error;
  if (result.finding) return result.finding->message;
  return std::nullopt;
}

}  // namespace

HardwareTransactionResult PluginWorkerCoordinator::Plan(
    std::vector<PluginWorkerParticipant> participants) {
  Abort();
  std::unordered_map<std::string, std::size_t> module_owners;
  for (std::size_t index = 0; index < participants.size(); ++index) {
    if (!participants[index].client)
      return {.ok = false, .message = "plugin worker participant is missing"};
    for (const std::string& module : participants[index].manifest.modules) {
      if (!module_owners.emplace(module, index).second)
        return {.ok = false,
                .message = "module " + module +
                    " has more than one plugin worker owner"};
    }
  }

  std::vector<std::vector<std::string>> participant_actions(
      participants.size());
  std::vector<HardwareAction> actions;
  for (std::size_t index = 0; index < participants.size(); ++index) {
    PluginWorkerHardwareActionsResult copied =
        participants[index].client->HardwareActions();
    if (!copied.ok()) {
      return {.ok = false,
              .message = copied.worker_error
                  ? *copied.worker_error
                  : copied.finding->message,
              .instance_path = copied.finding
                  ? copied.finding->instance_path
                  : std::string{}};
    }
    for (PluginWorkerHardwareAction& copied_action : copied.actions) {
      const std::string local_id = copied_action.action_id;
      const std::string global_id =
          participants[index].manifest.plugin_name + ":" + local_id;
      for (std::string& dependency : copied_action.dependencies)
        if (dependency.find(':') == std::string::npos)
          dependency =
              participants[index].manifest.plugin_name + ":" + dependency;
      HardwareActionClass action_class = HardwareActionClass::kNormal;
      if (copied_action.action_class == DANG_HARDWARE_ACTIVATE_V1)
        action_class = HardwareActionClass::kActivate;
      else if (copied_action.action_class == DANG_HARDWARE_DEACTIVATE_V1)
        action_class = HardwareActionClass::kDeactivate;
      PluginWorkerClient* client = participants[index].client;
      actions.push_back({
          global_id, std::move(copied_action.instance_path), action_class,
          std::move(copied_action.dependencies),
          [client, local_id] { return Failure(client->ApplyAction(local_id)); },
          [client, local_id] {
            return Failure(client->RollbackAction(local_id));
          }});
      participant_actions[index].push_back(global_id);
    }
  }

  for (std::size_t index = 0; index < participants.size(); ++index) {
    for (const std::string& dependency_module :
         participants[index].manifest.dependencies) {
      const auto owner = module_owners.find(dependency_module);
      if (owner == module_owners.end()) continue;
      for (HardwareAction& action : actions) {
        if (std::ranges::find(participant_actions[index], action.id) ==
            participant_actions[index].end())
          continue;
        action.dependencies.insert(action.dependencies.end(),
                                   participant_actions[owner->second].begin(),
                                   participant_actions[owner->second].end());
      }
    }
  }
  return planner_.Plan(std::move(actions));
}

HardwareTransactionResult PluginWorkerCoordinator::Apply() {
  return planner_.Apply();
}

void PluginWorkerCoordinator::Abort() noexcept { planner_.Abort(); }

}  // namespace dangd
