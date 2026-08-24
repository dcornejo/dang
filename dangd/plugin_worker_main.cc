// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_manager.h"
#include "dangd/plugin_worker_protocol.h"
#include "yang/resource_limits.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

using Json = nlohmann::json;

constexpr std::chrono::seconds kResponseTimeout(5);
constexpr std::chrono::hours kIdleReadPeriod(24);

std::size_t MaximumMessageBytes() {
  return yang::DefaultResourceLimits().maximum_snapshot_bytes;
}

bool Send(int descriptor, const Json& response) {
  std::string error;
  return dangd::WriteWorkerFrame(
             descriptor, response.dump(),
             std::chrono::steady_clock::now() + kResponseTimeout,
             MaximumMessageBytes(), &error) == dangd::WorkerIoStatus::kOk;
}

Json Discovery(const dangd::PluginManager& manager) {
  Json manifests = Json::array();
  for (const dangd::PluginManifest& manifest : manager.manifests()) {
    manifests.push_back(
        {{"plugin_name", manifest.plugin_name},
         {"abi_version", manifest.abi_version},
         {"modules", manifest.modules},
         {"dependencies", manifest.dependencies},
         {"supports_operations", manifest.supports_operations},
         {"supports_operational_data", manifest.supports_operational_data},
         {"supports_hardware_actions", manifest.supports_hardware_actions},
         {"supports_applied_reconciliation",
          manifest.supports_applied_reconciliation}});
  }
  Json sources = Json::array();
  for (const dangd::PluginYangSource& source : manager.yang_sources()) {
    sources.push_back(
        {{"plugin_name", source.plugin_name},
         {"module_name", source.module_name},
         {"revision", source.revision ? Json(*source.revision) : Json(nullptr)},
         {"source", source.source},
         {"source_uri", source.source_uri},
         {"role", source.role},
         {"enabled_features", source.enabled_features}});
  }
  return {{"ok", true},
          {"stage", "discovery"},
          {"manifests", std::move(manifests)},
          {"sources", std::move(sources)}};
}

Json Operational(const dangd::PluginManager& manager) {
  Json fragments = Json::array();
  for (const dangd::PluginOperationalFragment& fragment :
       manager.OperationalData()) {
    fragments.push_back(
        {{"provider", fragment.provider},
         {"data_xml", fragment.data_xml},
         {"error", fragment.error ? Json(*fragment.error) : Json(nullptr)},
         {"error_path", fragment.error_path},
         {"complete", fragment.complete}});
  }
  return {{"ok", true},
          {"stage", "operational"},
          {"fragments", std::move(fragments)}};
}

Json Finding(std::optional<yang::config::ValidationFinding> finding,
             std::string_view stage) {
  if (!finding)
    return {{"ok", true}, {"stage", stage}, {"accepted", true}};
  return {{"ok", true},
          {"stage", stage},
          {"accepted", false},
          {"finding",
           {{"message", finding->message},
            {"instance_path", finding->instance_path},
            {"netconf_error_tag", finding->netconf_error_tag},
            {"netconf_error_app_tag", finding->netconf_error_app_tag},
            {"module_name", finding->module_name}}}};
}

int Run(int descriptor, const std::filesystem::path& plugin_path) {
  dangd::PluginManager manager;
  std::vector<std::string> errors;
  if (!manager.Load(plugin_path, &errors)) {
    (void)Send(descriptor,
               {{"ok", false}, {"stage", "load"}, {"errors", errors}});
    return 1;
  }
  if (!Send(descriptor, {{"ok", true}, {"stage", "ready"}})) return 1;

  while (true) {
    dangd::WorkerReadResult request = dangd::ReadWorkerFrame(
        descriptor, std::chrono::steady_clock::now() + kIdleReadPeriod,
        MaximumMessageBytes());
    if (request.status == dangd::WorkerIoStatus::kTimeout) continue;
    if (request.status != dangd::WorkerIoStatus::kOk) return 1;
    const Json parsed = Json::parse(request.payload, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() ||
        !parsed.contains("operation") || !parsed["operation"].is_string()) {
      if (!Send(descriptor, {{"ok", false},
                             {"stage", "protocol"},
                             {"error", "invalid worker request"}}))
        return 1;
      continue;
    }
    const std::string operation = parsed["operation"].get<std::string>();
    if (operation == "discover") {
      if (!Send(descriptor, Discovery(manager))) return 1;
    } else if (operation == "operational") {
      if (!Send(descriptor, Operational(manager))) return 1;
    } else if (operation == "prepare") {
      try {
        if (!Send(descriptor,
                  Finding(manager.PrepareWorkerTransaction(
                              parsed.at("before_xml").get<std::string>(),
                              parsed.at("proposed_xml").get<std::string>(),
                              parsed.at("changes_json").get<std::string>()),
                          "prepare")))
          return 1;
      } catch (const Json::exception&) {
        if (!Send(descriptor, {{"ok", false},
                               {"stage", "protocol"},
                               {"error", "invalid prepare request"}}))
          return 1;
      }
    } else if (operation == "validate") {
      if (!Send(descriptor,
                Finding(manager.ValidateWorkerTransaction(), "validate")))
        return 1;
    } else if (operation == "hardware-actions") {
      std::vector<dangd::PluginWorkerHardwareAction> actions;
      const auto finding = manager.WorkerHardwareActions(&actions);
      if (finding) {
        if (!Send(descriptor, Finding(finding, "hardware-actions"))) return 1;
        continue;
      }
      Json serialized = Json::array();
      for (const auto& action : actions)
        serialized.push_back({{"action_id", action.action_id},
                              {"instance_path", action.instance_path},
                              {"action_class", action.action_class},
                              {"dependencies", action.dependencies}});
      if (!Send(descriptor, {{"ok", true},
                             {"stage", "hardware-actions"},
                             {"accepted", true},
                             {"actions", std::move(serialized)}}))
        return 1;
    } else if (operation == "apply-action" ||
               operation == "rollback-action") {
      try {
        const std::string action_id =
            parsed.at("action_id").get<std::string>();
        const auto finding = operation == "apply-action"
            ? manager.ApplyWorkerHardwareAction(action_id)
            : manager.RollbackWorkerHardwareAction(action_id);
        if (!Send(descriptor, Finding(finding, operation))) return 1;
      } catch (const Json::exception&) {
        if (!Send(descriptor, {{"ok", false},
                               {"stage", "protocol"},
                               {"error", "invalid action request"}}))
          return 1;
      }
    } else if (operation == "reconcile") {
      try {
        dangd::PluginWorkerAppliedReport report;
        const auto finding = manager.ReconcileWorkerApplied(
            parsed.at("current_xml").get<std::string>(), &report);
        if (finding) {
          if (!Send(descriptor, Finding(finding, "reconcile"))) return 1;
          continue;
        }
        Json outcomes = Json::array();
        for (const auto& outcome : report.outcomes)
          outcomes.push_back({{"provider", outcome.provider},
                              {"instance_path", outcome.instance_path},
                              {"disposition", outcome.disposition},
                              {"reason", outcome.reason}});
        if (!Send(descriptor, {{"ok", true},
                               {"stage", "reconcile"},
                               {"accepted", true},
                               {"applied_xml", report.applied_xml},
                               {"outcomes", std::move(outcomes)}}))
          return 1;
      } catch (const Json::exception&) {
        if (!Send(descriptor, {{"ok", false},
                               {"stage", "protocol"},
                               {"error", "invalid reconcile request"}}))
          return 1;
      }
    } else if (operation == "invoke") {
      try {
        const auto invoked = manager.InvokeWorkerOperation(
            parsed.at("module_name").get<std::string>(),
            parsed.at("operation_name").get<std::string>(),
            parsed.at("instance_path").get<std::string>(),
            parsed.at("input_xml").get<std::string>());
        if (!invoked.result.ok) {
          yang::config::ValidationFinding finding;
          if (invoked.result.errors.empty()) {
            finding.message = "plugin operation failed without diagnostics";
            finding.module_name =
                parsed.at("module_name").get<std::string>();
            finding.netconf_error_tag = "operation-failed";
          } else {
            finding = invoked.result.errors.front();
          }
          if (!Send(descriptor, Finding(std::move(finding), "invoke")))
            return 1;
          continue;
        }
        if (!Send(descriptor, {{"ok", true},
                               {"stage", "invoke"},
                               {"accepted", true},
                               {"output_xml", invoked.output_xml}}))
          return 1;
      } catch (const Json::exception&) {
        if (!Send(descriptor, {{"ok", false},
                               {"stage", "protocol"},
                               {"error", "invalid invoke request"}}))
          return 1;
      }
    } else if (operation == "abort") {
      manager.Abort();
      if (!Send(descriptor,
                {{"ok", true}, {"stage", "abort"}, {"accepted", true}}))
        return 1;
    } else if (operation == "shutdown") {
      (void)Send(descriptor, {{"ok", true}, {"stage", "shutdown"}});
      return 0;
    } else if (!Send(descriptor, {{"ok", false},
                                  {"stage", "protocol"},
                                  {"error", "unknown worker operation"}})) {
      return 1;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  int descriptor = -1;
  std::filesystem::path plugin_path;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--socket-fd" && index + 1 < argc) {
      const std::string_view value(argv[++index]);
      const auto parsed =
          std::from_chars(value.data(), value.data() + value.size(), descriptor);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != value.data() + value.size() || descriptor < 0)
        return 2;
    } else if (argument == "--plugin" && index + 1 < argc) {
      plugin_path = argv[++index];
    } else {
      return 2;
    }
  }
  if (descriptor < 0 || plugin_path.empty()) return 2;
  const int result = Run(descriptor, plugin_path);
  close(descriptor);
  return result;
}
