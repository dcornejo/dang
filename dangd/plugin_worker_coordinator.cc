// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_worker_coordinator.h"

#include "dangd/plugin_worker_client.h"

#include <algorithm>
#include <optional>
#include <set>
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

PluginWorkerCoordinator::~PluginWorkerCoordinator() { Abort(); }

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
  std::set<std::size_t> pending;
  for (std::size_t index = 0; index < participants.size(); ++index)
    pending.insert(index);
  std::vector<PluginWorkerParticipant> ordered;
  while (!pending.empty()) {
    bool progressed = false;
    for (auto iterator = pending.begin(); iterator != pending.end();) {
      const std::size_t index = *iterator;
      const bool ready = std::ranges::all_of(
          participants[index].manifest.dependencies,
          [&](const std::string& module) {
            const auto owner = module_owners.find(module);
            return owner == module_owners.end() ||
                !pending.contains(owner->second);
          });
      if (!ready) {
        ++iterator;
        continue;
      }
      ordered.push_back(std::move(participants[index]));
      iterator = pending.erase(iterator);
      progressed = true;
    }
    if (!progressed)
      return {.ok = false,
              .message = "plugin worker dependencies contain a cycle"};
  }
  participants = std::move(ordered);
  module_owners.clear();
  for (std::size_t index = 0; index < participants.size(); ++index)
    for (const std::string& module : participants[index].manifest.modules)
      module_owners.emplace(module, index);

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
  HardwareTransactionResult result = planner_.Plan(std::move(actions));
  if (result.ok) participants_ = std::move(participants);
  return result;
}

HardwareTransactionResult PluginWorkerCoordinator::Apply() {
  HardwareTransactionResult result = planner_.ApplyRetained();
  if (!result.ok) {
    for (const PluginWorkerParticipant& participant : participants_)
      (void)participant.client->Abort();
    participants_.clear();
  }
  return result;
}

PluginApplyResult PluginWorkerCoordinator::Reconcile(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& proposed) {
  const auto reject = [&](yang::config::ValidationFinding finding) {
    const HardwareTransactionResult rolled_back = planner_.RollbackApplied();
    for (const PluginWorkerParticipant& participant : participants_)
      (void)participant.client->Abort();
    participants_.clear();
    if (!rolled_back.rollback_failures.empty()) {
      finding.netconf_error_app_tag = "hardware-state-diverged";
      finding.message += "; rollback incomplete: ";
      for (std::size_t index = 0;
           index < rolled_back.rollback_failures.size(); ++index) {
        if (index) finding.message += ", ";
        finding.message += rolled_back.rollback_failures[index];
      }
    }
    return PluginApplyResult{std::move(finding), std::nullopt, {}};
  };
  yang::config::ConfigDocument accepted = proposed;
  std::string accepted_xml = proposed.ToXml();
  std::set<std::string> claimed_paths;
  std::vector<ConfigurationOutcome> accepted_outcomes;
  for (const PluginWorkerParticipant& participant : participants_) {
    PluginWorkerReconcileResult report =
        participant.client->Reconcile(accepted_xml);
    if (!report.ok()) {
      yang::config::ValidationFinding finding;
      if (report.finding) {
        finding = std::move(*report.finding);
      } else {
        finding.message = report.worker_error.value_or(
            "plugin worker reconciliation failed");
        finding.module_name = participant.manifest.plugin_name;
        finding.netconf_error_tag = "operation-failed";
      }
      return reject(std::move(finding));
    }
    auto parsed = yang::config::ParseDatastoreXml(
        schema, report.report->applied_xml, {.allow_origin_metadata = true});
    if (!parsed.document) {
      yang::config::ValidationFinding finding = parsed.findings.empty()
          ? yang::config::ValidationFinding{}
          : parsed.findings.front();
      finding.message = "plugin " + participant.manifest.plugin_name +
          " returned invalid applied state: " + finding.message;
      finding.module_name = participant.manifest.plugin_name;
      return reject(std::move(finding));
    }
    for (const auto& change :
         yang::config::DiffConfigDocuments(schema, accepted, *parsed.document)) {
      const std::string& module = schema.Get(change.schema).module_name;
      if (std::ranges::find(participant.manifest.modules, module) !=
          participant.manifest.modules.end())
        continue;
      yang::config::ValidationFinding finding;
      finding.message = "plugin " + participant.manifest.plugin_name +
          " modified a module it does not own";
      finding.instance_path = change.instance_path;
      finding.module_name = module;
      finding.netconf_error_tag = "operation-failed";
      return reject(std::move(finding));
    }
    for (const ConfigurationOutcome& outcome : report.report->outcomes) {
      if (outcome.provider != participant.manifest.plugin_name ||
          !claimed_paths.insert(outcome.instance_path).second) {
        yang::config::ValidationFinding finding;
        finding.message = "invalid or duplicate configuration outcome";
        finding.instance_path = outcome.instance_path;
        finding.module_name = participant.manifest.plugin_name;
        finding.netconf_error_tag = "operation-failed";
        return reject(std::move(finding));
      }
      accepted_outcomes.push_back(outcome);
    }
    accepted = std::move(*parsed.document);
    accepted_xml = std::move(report.report->applied_xml);
  }
  planner_.Commit();
  for (const PluginWorkerParticipant& participant : participants_)
    (void)participant.client->Abort();
  participants_.clear();
  return {std::nullopt, std::move(accepted), std::move(accepted_xml),
          std::move(accepted_outcomes)};
}

void PluginWorkerCoordinator::Abort() noexcept {
  (void)planner_.RollbackApplied();
  for (const PluginWorkerParticipant& participant : participants_)
    (void)participant.client->Abort();
  participants_.clear();
}

}  // namespace dangd
