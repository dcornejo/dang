// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_worker_runtime.h"

#include "dangd/plugin_worker_client.h"
#include "yang/xml_security.h"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace dangd {
namespace {

yang::config::ValidationFinding Failure(std::string provider,
                                        std::string message,
                                        std::string path = {}) {
  yang::config::ValidationFinding finding;
  finding.message = std::move(message);
  finding.instance_path = std::move(path);
  finding.module_name = std::move(provider);
  finding.netconf_error_tag = "operation-failed";
  finding.netconf_error_app_tag = "plugin-worker-failed";
  return finding;
}

}  // namespace

struct PluginWorkerRuntime::Entry {
  PluginManifest manifest;
  std::vector<PluginYangSource> sources;
  PluginWorkerClient::Options options;
  std::unique_ptr<PluginWorkerClient> client;

  [[nodiscard]] std::optional<std::string> Recover() {
    if (client->healthy()) return std::nullopt;
    std::vector<std::string> errors;
    auto replacement = PluginWorkerClient::Start(options, &errors);
    if (!replacement) {
      return errors.empty() ? "cannot restart plugin worker"
                            : "cannot restart plugin worker: " + errors.front();
    }
    std::string error;
    auto discovery = replacement->Discover(&error);
    const auto same_manifest = [&](const PluginManifest& other) {
      return other.plugin_name == manifest.plugin_name &&
             other.abi_version == manifest.abi_version &&
             other.modules == manifest.modules &&
             other.dependencies == manifest.dependencies &&
             other.resource_domains == manifest.resource_domains &&
             other.supports_operations == manifest.supports_operations &&
             other.supports_operational_data ==
                 manifest.supports_operational_data &&
             other.supports_hardware_actions ==
                 manifest.supports_hardware_actions &&
             other.supports_applied_reconciliation ==
                 manifest.supports_applied_reconciliation;
    };
    const auto same_source = [](const PluginYangSource& left,
                                const PluginYangSource& right) {
      return left.plugin_name == right.plugin_name &&
             left.module_name == right.module_name &&
             left.revision == right.revision && left.source == right.source &&
             left.source_uri == right.source_uri && left.role == right.role &&
             left.enabled_features == right.enabled_features;
    };
    if (!discovery || !same_manifest(discovery->manifest) ||
        discovery->sources.size() != sources.size() ||
        !std::equal(discovery->sources.begin(), discovery->sources.end(),
                    sources.begin(), same_source)) {
      return "restarted plugin discovery differs from the loaded plugin" +
             (error.empty() ? std::string{} : ": " + error);
    }
    client = std::move(replacement);
    return std::nullopt;
  }
};

std::unique_ptr<PluginWorkerRuntime> PluginWorkerRuntime::Load(
    const std::filesystem::path& worker_executable,
    const std::vector<std::filesystem::path>& plugins,
    std::vector<std::string>* errors, std::chrono::milliseconds startup_timeout,
    std::chrono::milliseconds request_timeout) {
  if (!errors) return nullptr;
  auto runtime = std::unique_ptr<PluginWorkerRuntime>(new PluginWorkerRuntime);
  std::unordered_map<std::string, std::string> owners;
  std::unordered_map<std::string, std::string> resource_owners;
  for (const auto& plugin : plugins) {
    auto client = PluginWorkerClient::Start(
        {worker_executable, plugin, startup_timeout, request_timeout}, errors);
    if (!client) continue;
    std::string discovery_error;
    auto discovery = client->Discover(&discovery_error);
    if (!discovery) {
      errors->push_back(plugin.string() + ": " + discovery_error);
      continue;
    }
    for (const std::string& module : discovery->manifest.modules) {
      const auto [owner, inserted] =
          owners.emplace(module, discovery->manifest.plugin_name);
      if (!inserted)
        errors->push_back("module " + module + " is implemented by plugins " +
                          owner->second + " and " +
                          discovery->manifest.plugin_name);
    }
    for (const std::string& domain : discovery->manifest.resource_domains) {
      const auto [owner, inserted] =
          resource_owners.emplace(domain, discovery->manifest.plugin_name);
      if (!inserted)
        errors->push_back("resource domain " + domain +
                          " is owned by plugins " + owner->second + " and " +
                          discovery->manifest.plugin_name);
    }
    runtime->sources_.insert(runtime->sources_.end(),
                             discovery->sources.begin(),
                             discovery->sources.end());
    runtime->manifests_.push_back(discovery->manifest);
    PluginWorkerClient::Options options{worker_executable, plugin,
                                        startup_timeout, request_timeout};
    runtime->entries_.push_back(std::make_unique<Entry>(Entry{
        std::move(discovery->manifest), std::move(discovery->sources),
        std::move(options), std::move(client)}));
  }
  std::unordered_map<std::string, std::size_t> owner_indexes;
  for (std::size_t index = 0; index < runtime->entries_.size(); ++index)
    for (const std::string& module : runtime->entries_[index]->manifest.modules)
      owner_indexes.emplace(module, index);
  for (const auto& entry : runtime->entries_)
    for (const std::string& dependency : entry->manifest.dependencies)
      if (!owner_indexes.contains(dependency))
        errors->push_back("plugin " + entry->manifest.plugin_name +
                          " requires missing implementation module " +
                          dependency);
  std::vector<unsigned char> visit(runtime->entries_.size(), 0);
  std::function<bool(std::size_t)> acyclic = [&](std::size_t index) {
    if (visit[index] == 1) return false;
    if (visit[index] == 2) return true;
    visit[index] = 1;
    for (const std::string& dependency :
         runtime->entries_[index]->manifest.dependencies) {
      const auto owner = owner_indexes.find(dependency);
      if (owner != owner_indexes.end() && !acyclic(owner->second)) return false;
    }
    visit[index] = 2;
    return true;
  };
  for (std::size_t index = 0; index < runtime->entries_.size(); ++index)
    if (!acyclic(index)) {
      errors->push_back("plugin runtime dependencies contain a cycle");
      break;
    }
  if (!errors->empty()) return nullptr;
  return runtime;
}

PluginWorkerRuntime::~PluginWorkerRuntime() { Abort(); }

const std::vector<PluginYangSource>& PluginWorkerRuntime::yang_sources() const {
  return sources_;
}

const std::vector<PluginManifest>& PluginWorkerRuntime::manifests() const {
  return manifests_;
}

std::vector<PluginOperationalFragment>
PluginWorkerRuntime::OperationalData() const {
  std::lock_guard lock(worker_mutex_);
  std::vector<PluginOperationalFragment> fragments;
  for (const auto& entry : entries_) {
    if (auto error = entry->Recover()) {
      fragments.push_back(
          {entry->manifest.plugin_name, {}, std::move(error), {}});
      continue;
    }
    PluginWorkerOperationalResult result = entry->client->OperationalData();
    if (result.worker_error) {
      fragments.push_back({entry->manifest.plugin_name, {},
                           *result.worker_error, {}});
      continue;
    }
    fragments.insert(fragments.end(),
                     std::make_move_iterator(result.fragments.begin()),
                     std::make_move_iterator(result.fragments.end()));
  }
  return fragments;
}

std::vector<PluginNotification> PluginWorkerRuntime::Notifications() {
  std::lock_guard lock(worker_mutex_);
  std::vector<PluginNotification> events;
  for (const auto& entry : entries_) {
    if (auto error = entry->Recover()) {
      events.push_back({.provider = entry->manifest.plugin_name,
                        .error = std::move(error)});
      continue;
    }
    PluginWorkerNotificationResult result = entry->client->Notifications();
    if (result.worker_error) {
      events.push_back({.provider = entry->manifest.plugin_name,
                        .error = std::move(result.worker_error)});
      continue;
    }
    events.insert(events.end(),
                  std::make_move_iterator(result.notifications.begin()),
                  std::make_move_iterator(result.notifications.end()));
  }
  return events;
}

std::string PluginWorkerRuntime::ReconciliationData(
    std::span<const OperationalProviderFailure> failures) const {
  std::lock_guard lock(reconciliation_mutex_);
  std::ostringstream output;
  output << "<hardware-reconciliation xmlns=\"urn:dangd:reconciliation\">"
         << "<diverged>" << (remnants_.empty() ? "false" : "true")
         << "</diverged>";
  for (const HardwareRemnant& remnant : remnants_)
    output << "<remnant><action-id>" << yang::EscapeXmlText(remnant.action_id)
           << "</action-id><instance-path>"
           << yang::EscapeXmlText(remnant.instance_path)
           << "</instance-path><reason>" << yang::EscapeXmlText(remnant.reason)
           << "</reason></remnant>";
  for (const ConfigurationOutcome& outcome : outcomes_) {
    const char* disposition =
        outcome.disposition == DANG_CONFIGURATION_APPLIED_V1 ? "applied"
        : outcome.disposition == DANG_CONFIGURATION_TRANSFORMED_V1
            ? "transformed"
        : outcome.disposition == DANG_CONFIGURATION_REJECTED_V1 ? "rejected"
                                                                 : "delayed";
    output << "<configuration-outcome><provider>"
           << yang::EscapeXmlText(outcome.provider)
           << "</provider><instance-path>"
           << yang::EscapeXmlText(outcome.instance_path)
           << "</instance-path><disposition>" << disposition
           << "</disposition>";
    if (!outcome.reason.empty())
      output << "<reason>" << yang::EscapeXmlText(outcome.reason) << "</reason>";
    output << "</configuration-outcome>";
  }
  for (const auto& failure : failures) {
    output << "<operational-provider-failure><provider>"
           << yang::EscapeXmlText(failure.provider) << "</provider><stage>"
           << yang::EscapeXmlText(failure.stage) << "</stage>";
    if (!failure.instance_path.empty())
      output << "<instance-path>"
             << yang::EscapeXmlText(failure.instance_path)
             << "</instance-path>";
    output << "<reason>"
           << yang::EscapeXmlText(failure.reason)
           << "</reason></operational-provider-failure>";
  }
  return output.str() + "</hardware-reconciliation>";
}

std::optional<yang::config::ValidationFinding> PluginWorkerRuntime::Prepare(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& before,
    const yang::config::ConfigDocument& after,
    std::span<const yang::config::ChangeEvent> changes) {
  std::lock_guard lock(worker_mutex_);
  Abort();
  std::set<std::string> changed_modules;
  nlohmann::json serialized = nlohmann::json::array();
  for (const auto& change : changes) {
    const std::string& module = schema.Get(change.schema).module_name;
    changed_modules.insert(module);
    serialized.push_back({{"module", module}, {"path", change.instance_path},
                          {"kind", static_cast<int>(change.kind)},
                          {"before", change.before}, {"after", change.after}});
  }
  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (std::ranges::find(affected_, index) != affected_.end()) continue;
      const auto& manifest = entries_[index]->manifest;
      const bool direct = std::ranges::any_of(
          manifest.modules, [&](const std::string& module) {
            return changed_modules.contains(module);
          });
      const bool dependency = std::ranges::any_of(
          manifest.dependencies, [&](const std::string& module) {
            return changed_modules.contains(module);
          });
      if (!direct && !dependency) continue;
      affected_.push_back(index);
      changed_modules.insert(manifest.modules.begin(), manifest.modules.end());
      expanded = true;
    }
  }
  std::unordered_map<std::string, std::size_t> owners;
  for (std::size_t index = 0; index < entries_.size(); ++index)
    for (const std::string& module : entries_[index]->manifest.modules)
      owners.emplace(module, index);
  std::set<std::size_t> pending(affected_.begin(), affected_.end());
  std::vector<std::size_t> ordered;
  while (!pending.empty()) {
    bool progressed = false;
    for (auto iterator = pending.begin(); iterator != pending.end();) {
      const std::size_t index = *iterator;
      const bool ready = std::ranges::all_of(
          entries_[index]->manifest.dependencies,
          [&](const std::string& module) {
            const auto owner = owners.find(module);
            return owner == owners.end() || !pending.contains(owner->second);
          });
      if (!ready) {
        ++iterator;
        continue;
      }
      ordered.push_back(index);
      iterator = pending.erase(iterator);
      progressed = true;
    }
    if (!progressed) {
      Abort();
      return Failure("dependency graph",
                     "affected plugin dependencies contain a cycle");
    }
  }
  affected_ = std::move(ordered);
  const std::string before_xml = before.ToXml();
  const std::string after_xml = after.ToXml();
  const std::string changes_json = serialized.dump();
  for (const std::size_t index : affected_) {
    if (auto error = entries_[index]->Recover()) {
      auto finding = Failure(entries_[index]->manifest.plugin_name, *error);
      Abort();
      return finding;
    }
    auto result = entries_[index]->client->Prepare(before_xml, after_xml,
                                                    changes_json);
    if (!result.ok()) {
      auto finding = result.finding.value_or(Failure(
          entries_[index]->manifest.plugin_name,
          result.worker_error.value_or("plugin worker prepare failed")));
      Abort();
      return finding;
    }
  }
  for (const std::size_t index : affected_) {
    auto result = entries_[index]->client->Validate();
    if (!result.ok()) {
      auto finding = result.finding.value_or(Failure(
          entries_[index]->manifest.plugin_name,
          result.worker_error.value_or("plugin worker validation failed")));
      Abort();
      return finding;
    }
  }
  return std::nullopt;
}

PluginApplyResult PluginWorkerRuntime::Apply(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& proposed) {
  std::lock_guard lock(worker_mutex_);
  std::vector<PluginWorkerParticipant> participants;
  for (const std::size_t index : affected_)
    participants.push_back(
        {entries_[index]->manifest, entries_[index]->client.get()});
  HardwareTransactionResult planned = coordinator_.Plan(std::move(participants));
  if (!planned.ok) {
    Abort();
    return {Failure("hardware planner", planned.message,
                    planned.instance_path), std::nullopt, {}};
  }
  HardwareTransactionResult applied = coordinator_.Apply();
  if (!applied.ok) {
    {
      std::lock_guard reconciliation_lock(reconciliation_mutex_);
      remnants_ = applied.remnants;
    }
    affected_.clear();
    return {Failure("hardware planner", applied.message,
                    applied.instance_path), std::nullopt, {}};
  }
  PluginApplyResult reconciled = coordinator_.Reconcile(schema, proposed);
  affected_.clear();
  if (!reconciled.error) {
    std::lock_guard reconciliation_lock(reconciliation_mutex_);
    remnants_.clear();
    outcomes_ = reconciled.outcomes;
  }
  return reconciled;
}

void PluginWorkerRuntime::Abort() noexcept {
  std::lock_guard lock(worker_mutex_);
  coordinator_.Abort();
  for (const std::size_t index : affected_)
    (void)entries_[index]->client->Abort();
  affected_.clear();
}

yang::netconf::OperationResult PluginWorkerRuntime::InvokeRpc(
    const yang::netconf::RpcSessionContext& session,
    const yang::config::RuntimeSchemaNode& operation,
    std::string_view operation_xml) {
  return InvokeAction(session, operation, {}, operation_xml);
}

yang::netconf::OperationResult PluginWorkerRuntime::InvokeAction(
    const yang::netconf::RpcSessionContext& session,
    const yang::config::RuntimeSchemaNode& operation,
    std::string_view instance_path, std::string_view operation_xml) {
  std::lock_guard lock(worker_mutex_);
  (void)session;
  const auto owner = std::ranges::find_if(entries_, [&](const auto& entry) {
    return std::ranges::find(entry->manifest.modules, operation.module_name) !=
        entry->manifest.modules.end();
  });
  if (owner == entries_.end()) {
    auto finding = Failure(operation.module_name,
                           "no plugin owns the operation's module");
    finding.netconf_error_tag = "operation-not-supported";
    finding.netconf_error_app_tag = "plugin-operation-unsupported";
    return {{false, {std::move(finding)}, {}}, {}};
  }
  if (auto error = (*owner)->Recover())
    return {{false, {Failure((*owner)->manifest.plugin_name, *error)}, {}}, {}};
  PluginWorkerOperationResult invoked = (*owner)->client->Invoke(
      operation.module_name, operation.name.local_name,
      std::string(instance_path), std::string(operation_xml));
  if (invoked.worker_error)
    return {{false,
             {Failure((*owner)->manifest.plugin_name, *invoked.worker_error)},
             {}},
            {}};
  return std::move(invoked.operation);
}

}  // namespace dangd
