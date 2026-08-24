// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_worker_client.h"

#include "dangd/plugin_worker_protocol.h"
#include "yang/resource_limits.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <utility>

#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

extern char** environ;

namespace dangd {
namespace {

using Json = nlohmann::json;
constexpr int kWorkerDescriptor = 198;

std::size_t MaximumMessageBytes() {
  return yang::DefaultResourceLimits().maximum_snapshot_bytes;
}

std::string WorkerFailure(WorkerIoStatus status, std::string_view detail) {
  std::string message;
  switch (status) {
    case WorkerIoStatus::kTimeout:
      message = "plugin worker timed out";
      break;
    case WorkerIoStatus::kPeerClosed:
      message = "plugin worker exited";
      break;
    case WorkerIoStatus::kProtocolError:
      message = "plugin worker protocol failure";
      break;
    case WorkerIoStatus::kSystemError:
      message = "plugin worker I/O failure";
      break;
    case WorkerIoStatus::kOk:
      return {};
  }
  if (!detail.empty()) message += ": " + std::string(detail);
  return message;
}

yang::config::ValidationFinding ParseFinding(const Json& serialized) {
  yang::config::ValidationFinding finding;
  finding.code = yang::config::ValidationCode::kInvalidValue;
  finding.state = yang::config::FindingState::kInvalid;
  finding.message = serialized.at("message").get<std::string>();
  finding.instance_path = serialized.at("instance_path").get<std::string>();
  finding.netconf_error_tag =
      serialized.at("netconf_error_tag").get<std::string>();
  finding.netconf_error_app_tag =
      serialized.at("netconf_error_app_tag").get<std::string>();
  finding.module_name = serialized.at("module_name").get<std::string>();
  const std::size_t maximum = yang::DefaultResourceLimits().maximum_xpath_bytes;
  if (finding.message.empty() || finding.message.size() > maximum ||
      finding.instance_path.size() > maximum ||
      finding.netconf_error_tag.size() > maximum ||
      finding.netconf_error_app_tag.size() > maximum ||
      finding.module_name.size() > maximum)
    throw Json::other_error::create(501, "invalid transaction finding",
                                    &serialized);
  return finding;
}

}  // namespace

struct PluginWorkerClient::ExchangeResult {
  std::optional<Json> response;
  std::string error;
};

PluginWorkerClient::PluginWorkerClient(Options options, int descriptor,
                                       int process)
    : options_(std::move(options)), descriptor_(descriptor), process_(process) {}

std::unique_ptr<PluginWorkerClient> PluginWorkerClient::Start(
    Options options, std::vector<std::string>* errors) {
  if (!errors) return nullptr;
  if (options.worker_executable.empty() || options.plugin.empty() ||
      options.startup_timeout <= std::chrono::milliseconds::zero() ||
      options.request_timeout <= std::chrono::milliseconds::zero()) {
    errors->push_back("invalid plugin worker options");
    return nullptr;
  }
  std::array<int, 2> sockets{-1, -1};
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) != 0) {
    errors->push_back("cannot create plugin worker socket: " +
                      std::string(std::strerror(errno)));
    return nullptr;
  }
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  (void)setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled));
  (void)setsockopt(sockets[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled));
#endif
  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(sockets[0]);
    close(sockets[1]);
    errors->push_back("cannot initialize plugin worker spawn actions");
    return nullptr;
  }
  int action_error =
      posix_spawn_file_actions_adddup2(&actions, sockets[1], kWorkerDescriptor);
  if (action_error == 0)
    action_error = posix_spawn_file_actions_addclose(&actions, sockets[0]);
  if (action_error == 0)
    action_error = posix_spawn_file_actions_addclose(&actions, sockets[1]);
  if (action_error != 0) {
    (void)posix_spawn_file_actions_destroy(&actions);
    close(sockets[0]);
    close(sockets[1]);
    errors->push_back("cannot configure plugin worker descriptors: " +
                      std::string(std::strerror(action_error)));
    return nullptr;
  }
  std::string executable = options.worker_executable.string();
  std::string plugin = options.plugin.string();
  std::string descriptor = std::to_string(kWorkerDescriptor);
  std::array<char*, 6> arguments{
      executable.data(), const_cast<char*>("--socket-fd"), descriptor.data(),
      const_cast<char*>("--plugin"), plugin.data(), nullptr};
  pid_t process = -1;
  const int spawned = posix_spawn(&process, executable.c_str(), &actions,
                                  nullptr, arguments.data(), environ);
  (void)posix_spawn_file_actions_destroy(&actions);
  close(sockets[1]);
  if (spawned != 0) {
    close(sockets[0]);
    errors->push_back("cannot spawn plugin worker: " +
                      std::string(std::strerror(spawned)));
    return nullptr;
  }
  auto client = std::unique_ptr<PluginWorkerClient>(
      new PluginWorkerClient(std::move(options), sockets[0], process));
  WorkerReadResult ready = ReadWorkerFrame(
      client->descriptor_,
      std::chrono::steady_clock::now() + client->options_.startup_timeout,
      MaximumMessageBytes());
  if (ready.status != WorkerIoStatus::kOk) {
    errors->push_back(WorkerFailure(ready.status, ready.error));
    client->Terminate();
    return nullptr;
  }
  const Json response = Json::parse(ready.payload, nullptr, false);
  if (response.is_discarded() || !response.is_object() ||
      !response.value("ok", false) || response.value("stage", "") != "ready") {
    if (response.is_object() && response.contains("errors") &&
        response["errors"].is_array()) {
      for (const Json& error : response["errors"])
        if (error.is_string()) errors->push_back(error.get<std::string>());
    }
    if (errors->empty()) errors->push_back("invalid plugin worker handshake");
    client->Terminate();
    return nullptr;
  }
  return client;
}

PluginWorkerClient::~PluginWorkerClient() { Terminate(); }

PluginWorkerClient::ExchangeResult PluginWorkerClient::Exchange(
    std::string request) {
  std::lock_guard lock(mutex_);
  if (!healthy_)
    return {std::nullopt, "plugin worker is not available"};
  const auto deadline =
      std::chrono::steady_clock::now() + options_.request_timeout;
  std::string detail;
  const WorkerIoStatus written = WriteWorkerFrame(
      descriptor_, request, deadline, MaximumMessageBytes(), &detail);
  if (written != WorkerIoStatus::kOk) {
    const std::string error = WorkerFailure(written, detail);
    TerminateLocked();
    return {std::nullopt, error};
  }
  WorkerReadResult read =
      ReadWorkerFrame(descriptor_, deadline, MaximumMessageBytes());
  if (read.status != WorkerIoStatus::kOk) {
    const std::string error = WorkerFailure(read.status, read.error);
    TerminateLocked();
    return {std::nullopt, error};
  }
  Json parsed = Json::parse(read.payload, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    TerminateLocked();
    return {std::nullopt, "plugin worker returned invalid JSON"};
  }
  return {std::move(parsed), {}};
}

std::optional<PluginWorkerDiscovery> PluginWorkerClient::Discover(
    std::string* error) {
  ExchangeResult exchanged = Exchange(Json{{"operation", "discover"}}.dump());
  if (!exchanged.response) {
    if (error) *error = exchanged.error;
    return std::nullopt;
  }
  const Json& response = *exchanged.response;
  if (!response.value("ok", false) || !response.contains("manifests") ||
      !response["manifests"].is_array() ||
      response["manifests"].size() != 1 || !response.contains("sources") ||
      !response["sources"].is_array()) {
    if (error) *error = "invalid plugin worker discovery response";
    Terminate();
    return std::nullopt;
  }
  try {
    const Json& value = response["manifests"][0];
    PluginWorkerDiscovery discovery;
    discovery.manifest = {
        value.at("plugin_name").get<std::string>(),
        value.at("abi_version").get<std::uint32_t>(),
        value.at("modules").get<std::vector<std::string>>(),
        value.at("dependencies").get<std::vector<std::string>>(),
        value.at("supports_operations").get<bool>(),
        value.at("supports_operational_data").get<bool>(),
        value.at("supports_hardware_actions").get<bool>(),
        value.at("supports_applied_reconciliation").get<bool>()};
    if (discovery.manifest.plugin_name.empty() ||
        discovery.manifest.abi_version < DANG_PLUGIN_ABI_V1 ||
        discovery.manifest.abi_version > DANG_PLUGIN_ABI_V6)
      throw Json::other_error::create(501, "invalid plugin manifest", &value);
    const auto valid_names = [](const std::vector<std::string>& values) {
      return std::ranges::none_of(values, &std::string::empty);
    };
    if (!valid_names(discovery.manifest.modules) ||
        !valid_names(discovery.manifest.dependencies))
      throw Json::other_error::create(501, "invalid plugin names", &value);
    for (const Json& source : response["sources"]) {
      PluginYangSource copied;
      copied.plugin_name = source.at("plugin_name").get<std::string>();
      copied.module_name = source.at("module_name").get<std::string>();
      if (!source.at("revision").is_null())
        copied.revision = source.at("revision").get<std::string>();
      copied.source = source.at("source").get<std::string>();
      copied.source_uri = source.at("source_uri").get<std::string>();
      copied.role = source.at("role").get<std::uint32_t>();
      copied.enabled_features =
          source.at("enabled_features").get<std::vector<std::string>>();
      if (copied.plugin_name != discovery.manifest.plugin_name ||
          copied.module_name.empty() || copied.source.empty() ||
          copied.source.size() >
              yang::DefaultResourceLimits().maximum_source_bytes ||
          (copied.role != DANG_YANG_IMPLEMENTED_V1 &&
           copied.role != DANG_YANG_IMPORT_ONLY_V1 &&
           copied.role != DANG_YANG_DEVIATION_V1) ||
          !valid_names(copied.enabled_features))
        throw Json::other_error::create(501, "invalid plugin source", &source);
      discovery.sources.push_back(std::move(copied));
    }
    return discovery;
  } catch (const Json::exception&) {
    if (error) *error = "malformed plugin worker discovery values";
    Terminate();
    return std::nullopt;
  }
}

PluginWorkerOperationalResult PluginWorkerClient::OperationalData() {
  ExchangeResult exchanged =
      Exchange(Json{{"operation", "operational"}}.dump());
  if (!exchanged.response) return {{}, exchanged.error};
  const Json& response = *exchanged.response;
  if (!response.value("ok", false) || !response.contains("fragments") ||
      !response["fragments"].is_array()) {
    Terminate();
    return {{}, "invalid plugin worker operational response"};
  }
  PluginWorkerOperationalResult result;
  try {
    for (const Json& fragment : response["fragments"]) {
      PluginOperationalFragment copied;
      copied.provider = fragment.at("provider").get<std::string>();
      copied.data_xml = fragment.at("data_xml").get<std::string>();
      if (!fragment.at("error").is_null())
        copied.error = fragment.at("error").get<std::string>();
      copied.error_path = fragment.at("error_path").get<std::string>();
      copied.complete = fragment.at("complete").get<bool>();
      const yang::ResourceLimits& limits = yang::DefaultResourceLimits();
      if (copied.provider.empty() ||
          copied.data_xml.size() > limits.maximum_xml_bytes ||
          (copied.error && copied.error->size() > limits.maximum_xpath_bytes) ||
          copied.error_path.size() > limits.maximum_xpath_bytes)
        throw Json::other_error::create(501, "invalid operational fragment",
                                        &fragment);
      result.fragments.push_back(std::move(copied));
    }
  } catch (const Json::exception&) {
    Terminate();
    return {{}, "malformed plugin worker operational values"};
  }
  return result;
}

PluginWorkerTransactionResult PluginWorkerClient::Transaction(
    std::string request) {
  ExchangeResult exchanged = Exchange(std::move(request));
  if (!exchanged.response) return {std::nullopt, exchanged.error};
  const Json& response = *exchanged.response;
  if (!response.value("ok", false) || !response.contains("accepted") ||
      !response["accepted"].is_boolean()) {
    Terminate();
    return {std::nullopt, "invalid plugin worker transaction response"};
  }
  if (response["accepted"].get<bool>()) return {};
  try {
    return {ParseFinding(response.at("finding")), std::nullopt};
  } catch (const Json::exception&) {
    Terminate();
    return {std::nullopt, "malformed plugin worker transaction finding"};
  }
}

PluginWorkerTransactionResult PluginWorkerClient::Prepare(
    std::string before_xml, std::string proposed_xml,
    std::string changes_json) {
  const yang::ResourceLimits& limits = yang::DefaultResourceLimits();
  if (before_xml.size() > limits.maximum_xml_bytes ||
      proposed_xml.size() > limits.maximum_xml_bytes ||
      changes_json.size() > limits.maximum_snapshot_bytes)
    return {std::nullopt, "plugin transaction exceeds the resource limit"};
  return Transaction(Json{{"operation", "prepare"},
                          {"before_xml", std::move(before_xml)},
                          {"proposed_xml", std::move(proposed_xml)},
                          {"changes_json", std::move(changes_json)}}
                         .dump());
}

PluginWorkerTransactionResult PluginWorkerClient::Validate() {
  return Transaction(Json{{"operation", "validate"}}.dump());
}

PluginWorkerHardwareActionsResult PluginWorkerClient::HardwareActions() {
  ExchangeResult exchanged =
      Exchange(Json{{"operation", "hardware-actions"}}.dump());
  if (!exchanged.response) return {{}, std::nullopt, exchanged.error};
  const Json& response = *exchanged.response;
  try {
    if (!response.value("ok", false) || !response.contains("accepted") ||
        !response["accepted"].is_boolean())
      throw Json::other_error::create(501, "invalid action response", &response);
    if (!response["accepted"].get<bool>())
      return {{}, ParseFinding(response.at("finding")), std::nullopt};
    const Json& serialized = response.at("actions");
    if (!serialized.is_array())
      throw Json::other_error::create(501, "invalid action list", &serialized);
    PluginWorkerHardwareActionsResult result;
    const auto& limits = yang::DefaultResourceLimits();
    for (const Json& value : serialized) {
      PluginWorkerHardwareAction action{
          value.at("action_id").get<std::string>(),
          value.at("instance_path").get<std::string>(),
          value.at("action_class").get<std::uint32_t>(),
          value.at("dependencies").get<std::vector<std::string>>()};
      if (action.action_id.empty() ||
          action.action_id.size() > limits.maximum_xpath_bytes ||
          action.instance_path.size() > limits.maximum_xpath_bytes ||
          action.action_class > DANG_HARDWARE_DEACTIVATE_V1 ||
          std::ranges::any_of(action.dependencies, [&](const std::string& item) {
            return item.empty() || item.size() > limits.maximum_xpath_bytes;
          }))
        throw Json::other_error::create(501, "invalid hardware action", &value);
      result.actions.push_back(std::move(action));
    }
    return result;
  } catch (const Json::exception&) {
    Terminate();
    return {{}, std::nullopt, "malformed plugin worker hardware actions"};
  }
}

PluginWorkerTransactionResult PluginWorkerClient::ApplyAction(
    std::string action_id) {
  return Transaction(Json{{"operation", "apply-action"},
                          {"action_id", std::move(action_id)}}.dump());
}

PluginWorkerTransactionResult PluginWorkerClient::RollbackAction(
    std::string action_id) {
  return Transaction(Json{{"operation", "rollback-action"},
                          {"action_id", std::move(action_id)}}.dump());
}

PluginWorkerReconcileResult PluginWorkerClient::Reconcile(
    std::string current_xml) {
  if (current_xml.size() > yang::DefaultResourceLimits().maximum_xml_bytes)
    return {std::nullopt, std::nullopt,
            "applied configuration exceeds the resource limit"};
  ExchangeResult exchanged = Exchange(
      Json{{"operation", "reconcile"},
           {"current_xml", std::move(current_xml)}}.dump());
  if (!exchanged.response)
    return {std::nullopt, std::nullopt, exchanged.error};
  const Json& response = *exchanged.response;
  try {
    if (!response.value("ok", false) || !response.contains("accepted") ||
        !response["accepted"].is_boolean())
      throw Json::other_error::create(501, "invalid reconcile response",
                                      &response);
    if (!response["accepted"].get<bool>())
      return {std::nullopt, ParseFinding(response.at("finding")), std::nullopt};
    PluginWorkerAppliedReport report;
    report.applied_xml = response.at("applied_xml").get<std::string>();
    const Json& outcomes = response.at("outcomes");
    if (report.applied_xml.empty() ||
        report.applied_xml.size() >
            yang::DefaultResourceLimits().maximum_xml_bytes ||
        !outcomes.is_array())
      throw Json::other_error::create(501, "invalid reconcile values",
                                      &response);
    const std::size_t maximum =
        yang::DefaultResourceLimits().maximum_xpath_bytes;
    for (const Json& value : outcomes) {
      ConfigurationOutcome outcome{
          value.at("provider").get<std::string>(),
          value.at("instance_path").get<std::string>(),
          value.at("disposition").get<std::uint32_t>(),
          value.at("reason").get<std::string>()};
      if (outcome.provider.empty() || outcome.provider.size() > maximum ||
          outcome.instance_path.empty() ||
          outcome.instance_path.size() > maximum ||
          outcome.reason.size() > maximum ||
          outcome.disposition < DANG_CONFIGURATION_APPLIED_V1 ||
          outcome.disposition > DANG_CONFIGURATION_DELAYED_V1)
        throw Json::other_error::create(501, "invalid reconcile outcome",
                                        &value);
      report.outcomes.push_back(std::move(outcome));
    }
    return {std::move(report), std::nullopt, std::nullopt};
  } catch (const Json::exception&) {
    Terminate();
    return {std::nullopt, std::nullopt,
            "malformed plugin worker applied-state report"};
  }
}

std::optional<std::string> PluginWorkerClient::Abort() {
  PluginWorkerTransactionResult result =
      Transaction(Json{{"operation", "abort"}}.dump());
  if (result.worker_error) return result.worker_error;
  if (result.finding) return result.finding->message;
  return std::nullopt;
}

bool PluginWorkerClient::healthy() const noexcept {
  std::lock_guard lock(mutex_);
  return healthy_;
}

void PluginWorkerClient::Terminate() noexcept {
  std::lock_guard lock(mutex_);
  TerminateLocked();
}

void PluginWorkerClient::TerminateLocked() noexcept {
  healthy_ = false;
  if (descriptor_ >= 0) {
    close(descriptor_);
    descriptor_ = -1;
  }
  if (process_ <= 0) return;
  int status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(process_, &status, WNOHANG);
  } while (waited < 0 && errno == EINTR);
  if (waited == 0) {
    (void)kill(process_, SIGKILL);
    while (waitpid(process_, &status, 0) < 0 && errno == EINTR) {
    }
  }
  process_ = -1;
}

}  // namespace dangd
