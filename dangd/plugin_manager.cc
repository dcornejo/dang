// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_manager.h"

#include "dangd/hardware_transaction.h"
#include "yang/xml_security.h"

#include <algorithm>
#include <dlfcn.h>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace dangd {
namespace {

yang::config::ValidationFinding PluginFinding(
    std::string_view plugin, const DangPluginErrorV1& error,
    std::string_view fallback) {
  yang::config::ValidationFinding finding;
  finding.code = yang::config::ValidationCode::kInvalidValue;
  finding.state = yang::config::FindingState::kInvalid;
  finding.message = "plugin " + std::string(plugin) + ": " +
                    (error.message ? error.message : std::string(fallback));
  if (error.instance_path) finding.instance_path = error.instance_path;
  finding.netconf_error_tag = "operation-failed";
  finding.netconf_error_app_tag = "plugin-error";
  finding.module_name = std::string(plugin);
  return finding;
}

}  // namespace

struct PluginManager::State {
  struct Plugin {
    void* library = nullptr;
    const DangPluginV1* api = nullptr;
    int (*invoke)(void*, const DangOperationV1*, DangOperationResultV1*,
                  DangPluginErrorV1*) = nullptr;
    int (*operational)(void*, DangOperationalDataV1*, DangPluginErrorV1*) =
        nullptr;
    int (*operational_v2)(void*, DangOperationalDataV2*, DangPluginErrorV1*) =
        nullptr;
    size_t (*hardware_action_count)(void*, void*) = nullptr;
    int (*hardware_action_at)(void*, void*, size_t, DangHardwareActionV1*,
                              DangPluginErrorV1*) = nullptr;
    int (*apply_hardware_action)(void*, void*, const char*,
                                 DangPluginErrorV1*) = nullptr;
    int (*rollback_hardware_action)(void*, void*, const char*,
                                    DangPluginErrorV1*) = nullptr;
    int (*reconcile_applied)(void*, void*, const char*,
                             DangAppliedConfigurationV1*,
                             DangPluginErrorV1*) = nullptr;
    std::string name;
    std::vector<std::string> modules;
    std::vector<std::string> dependencies;
    void* prepared = nullptr;
    bool affected = false;
  };

  std::vector<Plugin> plugins;
  std::vector<PluginYangSource> sources;
  std::vector<std::size_t> order;
  std::string before_xml;
  std::string proposed_xml;
  std::string changes_json;
  mutable std::mutex reconciliation_mutex;
  std::vector<HardwareRemnant> remnants;
  std::vector<ConfigurationOutcome> outcomes;

  void ReleasePrepared() noexcept {
    for (Plugin& plugin : plugins) {
      if (plugin.prepared && plugin.api->release)
        plugin.api->release(plugin.api->context, plugin.prepared);
      plugin.prepared = nullptr;
      plugin.affected = false;
    }
    order.clear();
    before_xml.clear();
    proposed_xml.clear();
    changes_json.clear();
  }
};

PluginManager::PluginManager() : state_(std::make_unique<State>()) {}

PluginManager::~PluginManager() {
  state_->ReleasePrepared();
  for (auto iterator = state_->plugins.rbegin();
       iterator != state_->plugins.rend(); ++iterator) {
    if (iterator->api && iterator->api->destroy)
      iterator->api->destroy(iterator->api->context);
    if (iterator->library) dlclose(iterator->library);
  }
}

bool PluginManager::Load(const std::filesystem::path& path,
                         std::vector<std::string>* errors) {
  if (!errors) return false;
  void* library = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    errors->push_back("cannot load plugin " + path.string() + ": " +
                      dlerror());
    return false;
  }
  dlerror();
  auto initialize_v6 = reinterpret_cast<DangPluginInitV6>(
      dlsym(library, "dang_plugin_init_v6"));
  const char* v6_error = dlerror();
  const DangPluginV6* api_v6 =
      v6_error == nullptr && initialize_v6 ? initialize_v6() : nullptr;
  dlerror();
  auto initialize_v5 = reinterpret_cast<DangPluginInitV5>(
      dlsym(library, "dang_plugin_init_v5"));
  const char* v5_error = dlerror();
  const DangPluginV5* api_v5 =
      v5_error == nullptr && initialize_v5 ? initialize_v5() : nullptr;
  dlerror();
  auto initialize_v4 = reinterpret_cast<DangPluginInitV4>(
      dlsym(library, "dang_plugin_init_v4"));
  const char* v4_error = dlerror();
  const DangPluginV4* api_v4 =
      v4_error == nullptr && initialize_v4 ? initialize_v4() : nullptr;
  dlerror();
  auto initialize_v3 = reinterpret_cast<DangPluginInitV3>(
      dlsym(library, "dang_plugin_init_v3"));
  const char* v3_error = dlerror();
  const DangPluginV3* api_v3 =
      v3_error == nullptr && initialize_v3 ? initialize_v3() : nullptr;
  dlerror();
  auto initialize_v2 = reinterpret_cast<DangPluginInitV2>(
      dlsym(library, "dang_plugin_init_v2"));
  const char* v2_error = dlerror();
  const DangPluginV2* api_v2 =
      v2_error == nullptr && initialize_v2 ? initialize_v2() : nullptr;
  dlerror();
  auto initialize = reinterpret_cast<DangPluginInitV1>(
      dlsym(library, "dang_plugin_init_v1"));
  const char* v1_error = dlerror();
  if (api_v6 == nullptr && api_v5 == nullptr && api_v4 == nullptr &&
      api_v3 == nullptr && api_v2 == nullptr && v1_error != nullptr) {
    errors->push_back("plugin " + path.string() +
                      " has no supported dang_plugin_init entry point");
    dlclose(library);
    return false;
  }
  const DangPluginV1* api =
      api_v6   ? &api_v6->v5.v4.v3.v2.v1
      : api_v5 ? &api_v5->v4.v3.v2.v1
      : api_v4 ? &api_v4->v3.v2.v1
      : api_v3 ? &api_v3->v2.v1
      : api_v2 ? &api_v2->v1
               : (initialize ? initialize() : nullptr);
  if (!api || api->abi_version !=
                  (api_v6 ? DANG_PLUGIN_ABI_V6
                          : api_v5 ? DANG_PLUGIN_ABI_V5
                          : api_v4 ? DANG_PLUGIN_ABI_V4
                          : api_v3 ? DANG_PLUGIN_ABI_V3
                          : (api_v2 ? DANG_PLUGIN_ABI_V2
                                    : DANG_PLUGIN_ABI_V1)) ||
      !api->plugin_name ||
      !api->yang_source_count || !api->yang_source_at || !api->prepare ||
      !api->validate || !api->apply || !api->rollback || !api->release) {
    errors->push_back("plugin " + path.string() +
                      " does not implement the complete ABI v1 contract");
    dlclose(library);
    return false;
  }
  const DangPluginV5* complete_api = api_v6 ? &api_v6->v5 : api_v5;
  const DangPluginV4* action_api = complete_api ? &complete_api->v4 : api_v4;
  if (action_api &&
      (!action_api->hardware_action_count || !action_api->hardware_action_at ||
       !action_api->apply_hardware_action ||
       !action_api->rollback_hardware_action)) {
    errors->push_back("plugin " + std::string(api->plugin_name) +
                      " does not implement the complete ABI v4 action plan");
    if (api->destroy) api->destroy(api->context);
    dlclose(library);
    return false;
  }
  if (complete_api && !complete_api->get_operational_data_v2) {
    errors->push_back("plugin " + std::string(api->plugin_name) +
                      " does not implement ABI v5 operational publication");
    if (api->destroy) api->destroy(api->context);
    dlclose(library);
    return false;
  }
  if ((api->dependency_count == nullptr) !=
      (api->dependency_at == nullptr)) {
    errors->push_back("plugin " + std::string(api->plugin_name) +
                      " must provide both dependency callbacks or neither");
    if (api->destroy) api->destroy(api->context);
    dlclose(library);
    return false;
  }
  if (std::ranges::any_of(state_->plugins, [&](const State::Plugin& plugin) {
        return plugin.name == api->plugin_name;
      })) {
    errors->push_back("duplicate plugin name: " + std::string(api->plugin_name));
    dlclose(library);
    return false;
  }

  State::Plugin plugin;
  plugin.library = library;
  plugin.api = api;
  plugin.invoke = api_v6   ? api_v6->v5.v4.v3.v2.invoke
                  : api_v5 ? api_v5->v4.v3.v2.invoke
                  : api_v4 ? api_v4->v3.v2.invoke
                  : api_v3 ? api_v3->v2.invoke
                  : api_v2 ? api_v2->invoke
                           : nullptr;
  plugin.operational =
      api_v6   ? api_v6->v5.v4.v3.get_operational_data
      : api_v5 ? api_v5->v4.v3.get_operational_data
      : api_v4 ? api_v4->v3.get_operational_data
      : api_v3 ? api_v3->get_operational_data
               : nullptr;
  plugin.operational_v2 =
      complete_api ? complete_api->get_operational_data_v2 : nullptr;
  plugin.reconcile_applied =
      api_v6 ? api_v6->reconcile_applied_configuration : nullptr;
  if (action_api) {
    plugin.hardware_action_count = action_api->hardware_action_count;
    plugin.hardware_action_at = action_api->hardware_action_at;
    plugin.apply_hardware_action = action_api->apply_hardware_action;
    plugin.rollback_hardware_action = action_api->rollback_hardware_action;
  }
  plugin.name = api->plugin_name;
  std::vector<PluginYangSource> discovered_sources;
  const std::size_t count = api->yang_source_count(api->context);
  for (std::size_t index = 0; index < count; ++index) {
    DangYangSourceV1 source{};
    DangPluginErrorV1 error{};
    if (!api->yang_source_at(api->context, index, &source, &error) ||
        !source.module_name || !source.source || source.source_size == 0) {
      errors->push_back("plugin " + plugin.name + ": " +
                        (error.message ? error.message
                                       : "invalid YANG source descriptor"));
      if (api->destroy) api->destroy(api->context);
      dlclose(library);
      return false;
    }
    PluginYangSource copied{
        plugin.name, source.module_name,
        source.revision && *source.revision
            ? std::optional<std::string>(source.revision)
            : std::nullopt,
        std::string(source.source, source.source_size),
        source.source_uri ? source.source_uri : "", source.role, {}};
    if (source.role != DANG_YANG_IMPLEMENTED_V1 &&
        source.role != DANG_YANG_IMPORT_ONLY_V1 &&
        source.role != DANG_YANG_DEVIATION_V1) {
      errors->push_back("plugin " + plugin.name +
                        ": invalid YANG source role");
      if (api->destroy) api->destroy(api->context);
      dlclose(library);
      return false;
    }
    for (std::size_t feature = 0; feature < source.enabled_feature_count;
         ++feature) {
      if (!source.enabled_features || !source.enabled_features[feature]) {
        errors->push_back("plugin " + plugin.name +
                          ": invalid enabled feature descriptor");
        if (api->destroy) api->destroy(api->context);
        dlclose(library);
        return false;
      }
      copied.enabled_features.emplace_back(source.enabled_features[feature]);
    }
    const auto duplicate = std::ranges::find_if(
        state_->sources, [&](const PluginYangSource& existing) {
          return existing.module_name == copied.module_name &&
                 existing.revision == copied.revision;
        });
    if (duplicate != state_->sources.end() &&
        duplicate->source != copied.source) {
      errors->push_back("plugins " + duplicate->plugin_name + " and " +
                        plugin.name + " supply different sources for " +
                        copied.module_name + "@" +
                        copied.revision.value_or("<none>"));
      if (api->destroy) api->destroy(api->context);
      dlclose(library);
      return false;
    }
    const auto local_duplicate = std::ranges::find_if(
        discovered_sources, [&](const PluginYangSource& existing) {
          return existing.module_name == copied.module_name &&
                 existing.revision == copied.revision;
        });
    if (local_duplicate != discovered_sources.end()) {
      errors->push_back("plugin " + plugin.name + " supplies " +
                        copied.module_name + "@" +
                        copied.revision.value_or("<none>") + " more than once");
      if (api->destroy) api->destroy(api->context);
      dlclose(library);
      return false;
    }
    if (source.role == DANG_YANG_IMPLEMENTED_V1)
      plugin.modules.push_back(copied.module_name);
    discovered_sources.push_back(std::move(copied));
  }
  if (api->dependency_count && api->dependency_at) {
    for (std::size_t index = 0; index < api->dependency_count(api->context);
         ++index) {
      const char* dependency = api->dependency_at(api->context, index);
      if (!dependency || !*dependency) {
        errors->push_back("plugin " + plugin.name +
                          ": invalid runtime dependency descriptor");
        if (api->destroy) api->destroy(api->context);
        dlclose(library);
        return false;
      }
      plugin.dependencies.emplace_back(dependency);
    }
  }
  state_->sources.insert(state_->sources.end(),
                         std::make_move_iterator(discovered_sources.begin()),
                         std::make_move_iterator(discovered_sources.end()));
  state_->plugins.push_back(std::move(plugin));
  return true;
}

const std::vector<PluginYangSource>& PluginManager::yang_sources() const {
  return state_->sources;
}

std::vector<PluginOperationalFragment> PluginManager::OperationalData() const {
  std::vector<PluginOperationalFragment> result;
  for (const State::Plugin& plugin : state_->plugins) {
    if (plugin.operational_v2) {
      DangOperationalDataV2 data{};
      DangPluginErrorV1 error{};
      if (!plugin.operational_v2(plugin.api->context, &data, &error)) {
        result.push_back({plugin.name, {}, error.message
            ? std::optional<std::string>(error.message)
            : std::optional<std::string>("operational callback failed"),
            error.instance_path ? error.instance_path : "", false});
      } else if (!data.data_xml) {
        result.push_back(
            {plugin.name, {}, "callback returned no XML data", {}, false});
      } else {
        result.push_back({plugin.name, data.data_xml, std::nullopt, {},
                          data.complete != 0});
      }
      continue;
    }
    if (!plugin.operational) continue;
    DangOperationalDataV1 data{};
    DangPluginErrorV1 error{};
    if (!plugin.operational(plugin.api->context, &data, &error)) {
      result.push_back({plugin.name, {}, error.message
          ? std::optional<std::string>(error.message)
          : std::optional<std::string>("operational callback failed"),
          error.instance_path ? error.instance_path : ""});
    } else if (!data.data_xml) {
      result.push_back(
          {plugin.name, {}, "callback returned no XML data", {}});
    } else {
      result.push_back({plugin.name, data.data_xml, std::nullopt, {}});
    }
  }
  return result;
}

std::string PluginManager::ReconciliationData(
    std::span<const OperationalProviderFailure> provider_failures) const {
  std::lock_guard lock(state_->reconciliation_mutex);
  std::ostringstream output;
  output << "<hardware-reconciliation xmlns=\"urn:dangd:reconciliation\">"
         << "<diverged>" << (state_->remnants.empty() ? "false" : "true")
         << "</diverged>";
  for (const HardwareRemnant& remnant : state_->remnants) {
    output << "<remnant><action-id>"
           << yang::EscapeXmlText(remnant.action_id)
           << "</action-id><instance-path>"
           << yang::EscapeXmlText(remnant.instance_path)
           << "</instance-path><reason>"
           << yang::EscapeXmlText(remnant.reason)
           << "</reason></remnant>";
  }
  for (const ConfigurationOutcome& outcome : state_->outcomes) {
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
      output << "<reason>" << yang::EscapeXmlText(outcome.reason)
             << "</reason>";
    output << "</configuration-outcome>";
  }
  for (const OperationalProviderFailure& failure : provider_failures) {
    output << "<operational-provider-failure><provider>"
           << yang::EscapeXmlText(failure.provider)
           << "</provider><stage>" << yang::EscapeXmlText(failure.stage)
           << "</stage>";
    if (!failure.instance_path.empty())
      output << "<instance-path>"
             << yang::EscapeXmlText(failure.instance_path)
             << "</instance-path>";
    output << "<reason>" << yang::EscapeXmlText(failure.reason)
           << "</reason></operational-provider-failure>";
  }
  output << "</hardware-reconciliation>";
  return output.str();
}

bool PluginManager::ValidateDependencies(
    std::vector<std::string>* errors) const {
  std::unordered_map<std::string, std::string> owners;
  std::unordered_map<std::string, std::size_t> owner_indexes;
  for (std::size_t index = 0; index < state_->plugins.size(); ++index) {
    const State::Plugin& plugin = state_->plugins[index];
    for (const std::string& module : plugin.modules) {
      const auto [found, inserted] = owners.emplace(module, plugin.name);
      if (!inserted)
        errors->push_back("module " + module + " is implemented by plugins " +
                          found->second + " and " + plugin.name);
      else
        owner_indexes.emplace(module, index);
    }
  }
  for (const State::Plugin& plugin : state_->plugins) {
    for (const std::string& dependency : plugin.dependencies) {
      if (!owners.contains(dependency))
        errors->push_back("plugin " + plugin.name +
                          " requires missing implementation module " +
                          dependency);
    }
  }
  std::vector<unsigned char> visit(state_->plugins.size(), 0);
  std::function<bool(std::size_t)> acyclic = [&](std::size_t index) {
    if (visit[index] == 1) return false;
    if (visit[index] == 2) return true;
    visit[index] = 1;
    for (const std::string& dependency :
         state_->plugins[index].dependencies) {
      const auto owner = owner_indexes.find(dependency);
      if (owner != owner_indexes.end() && !acyclic(owner->second)) return false;
    }
    visit[index] = 2;
    return true;
  };
  for (std::size_t index = 0; index < state_->plugins.size(); ++index) {
    if (!acyclic(index)) {
      errors->push_back("plugin runtime dependencies contain a cycle");
      break;
    }
  }
  return errors->empty();
}

std::optional<yang::config::ValidationFinding> PluginManager::Prepare(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& before,
    const yang::config::ConfigDocument& after,
    std::span<const yang::config::ChangeEvent> changes) {
  Abort();
  std::set<std::string> changed_modules;
  nlohmann::json serialized_changes = nlohmann::json::array();
  for (const auto& change : changes) {
    const std::string& module = schema.Get(change.schema).module_name;
    changed_modules.insert(module);
    serialized_changes.push_back({{"module", module},
                                  {"path", change.instance_path},
                                  {"kind", static_cast<int>(change.kind)},
                                  {"before", change.before},
                                  {"after", change.after}});
  }
  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (State::Plugin& plugin : state_->plugins) {
      const bool direct = std::ranges::any_of(
          plugin.modules, [&](const std::string& module) {
            return changed_modules.contains(module);
          });
      const bool dependency = std::ranges::any_of(
          plugin.dependencies, [&](const std::string& module) {
            return changed_modules.contains(module);
          });
      if ((direct || dependency) && !plugin.affected) {
        plugin.affected = true;
        changed_modules.insert(plugin.modules.begin(), plugin.modules.end());
        expanded = true;
      }
    }
  }

  std::unordered_map<std::string, std::size_t> owners;
  for (std::size_t index = 0; index < state_->plugins.size(); ++index) {
    for (const std::string& module : state_->plugins[index].modules) {
      if (!owners.emplace(module, index).second)
        return PluginFinding(state_->plugins[index].name, {},
                             "module has more than one implementation owner");
    }
  }
  std::set<std::size_t> pending;
  for (std::size_t index = 0; index < state_->plugins.size(); ++index)
    if (state_->plugins[index].affected) pending.insert(index);
  while (!pending.empty()) {
    bool progressed = false;
    for (auto iterator = pending.begin(); iterator != pending.end();) {
      const std::size_t index = *iterator;
      const bool ready = std::ranges::all_of(
          state_->plugins[index].dependencies, [&](const std::string& module) {
            const auto owner = owners.find(module);
            return owner == owners.end() || !pending.contains(owner->second);
          });
      if (!ready) {
        ++iterator;
        continue;
      }
      state_->order.push_back(index);
      iterator = pending.erase(iterator);
      progressed = true;
    }
    if (!progressed) {
      Abort();
      return PluginFinding("dependency graph", {},
                           "affected plugin dependencies contain a cycle");
    }
  }

  state_->before_xml = before.ToXml();
  state_->proposed_xml = after.ToXml();
  state_->changes_json = serialized_changes.dump();
  const DangTransactionV1 transaction{state_->before_xml.c_str(),
                                      state_->proposed_xml.c_str(),
                                      state_->changes_json.c_str()};
  for (const std::size_t index : state_->order) {
    State::Plugin& plugin = state_->plugins[index];
    DangPluginErrorV1 error{};
    if (!plugin.api->prepare(plugin.api->context, &transaction,
                             &plugin.prepared, &error) || !plugin.prepared) {
      auto finding = PluginFinding(plugin.name, error, "prepare failed");
      Abort();
      return finding;
    }
  }
  for (const std::size_t index : state_->order) {
    State::Plugin& plugin = state_->plugins[index];
    DangPluginErrorV1 error{};
    if (!plugin.api->validate(plugin.api->context, plugin.prepared, &error)) {
      auto finding = PluginFinding(plugin.name, error, "validation failed");
      Abort();
      return finding;
    }
  }
  return std::nullopt;
}

PluginApplyResult PluginManager::Apply(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& proposed) {
  struct PlannedPlugin {
    std::size_t index = 0;
    std::vector<std::string> action_ids;
  };
  std::vector<PlannedPlugin> planned_plugins;
  std::vector<HardwareAction> actions;
  std::unordered_map<std::string, std::size_t> module_owners;
  for (std::size_t index = 0; index < state_->plugins.size(); ++index)
    for (const std::string& module : state_->plugins[index].modules)
      module_owners.emplace(module, index);

  for (const std::size_t index : state_->order) {
    State::Plugin& plugin = state_->plugins[index];
    PlannedPlugin planned{index, {}};
    if (!plugin.hardware_action_count) {
      const std::string id = plugin.name + ":transaction";
      planned.action_ids.push_back(id);
      actions.push_back({
          id, "", HardwareActionClass::kNormal, {},
          [&plugin]() -> std::optional<std::string> {
            DangPluginErrorV1 error{};
            if (plugin.api->apply(plugin.api->context, plugin.prepared, &error))
              return std::nullopt;
            return error.message ? std::string(error.message) : "apply failed";
          },
          [&plugin]() -> std::optional<std::string> {
            DangPluginErrorV1 error{};
            if (plugin.api->rollback(plugin.api->context, plugin.prepared,
                                     &error))
              return std::nullopt;
            return error.message ? std::string(error.message)
                                 : "rollback failed";
          }});
      planned_plugins.push_back(std::move(planned));
      continue;
    }

    const std::size_t count =
        plugin.hardware_action_count(plugin.api->context, plugin.prepared);
    for (std::size_t action_index = 0; action_index < count; ++action_index) {
      DangHardwareActionV1 descriptor{};
      DangPluginErrorV1 error{};
      if (!plugin.hardware_action_at(plugin.api->context, plugin.prepared,
                                     action_index, &descriptor, &error) ||
          !descriptor.action_id || !*descriptor.action_id ||
          (descriptor.dependency_count != 0 && !descriptor.dependencies)) {
        auto finding = PluginFinding(plugin.name, error,
                                     "invalid hardware action descriptor");
        Abort();
        return {finding, std::nullopt, {}};
      }
      const std::string local_id = descriptor.action_id;
      const std::string id = plugin.name + ":" + local_id;
      std::vector<std::string> dependencies;
      for (std::size_t dependency = 0;
           dependency < descriptor.dependency_count; ++dependency) {
        if (!descriptor.dependencies[dependency] ||
            !*descriptor.dependencies[dependency]) {
          auto finding = PluginFinding(
              plugin.name, {}, "invalid hardware action dependency");
          Abort();
          return {finding, std::nullopt, {}};
        }
        std::string copied = descriptor.dependencies[dependency];
        if (copied.find(':') == std::string::npos)
          copied = plugin.name + ":" + copied;
        dependencies.push_back(std::move(copied));
      }
      HardwareActionClass action_class = HardwareActionClass::kNormal;
      if (descriptor.action_class == DANG_HARDWARE_ACTIVATE_V1)
        action_class = HardwareActionClass::kActivate;
      else if (descriptor.action_class == DANG_HARDWARE_DEACTIVATE_V1)
        action_class = HardwareActionClass::kDeactivate;
      else if (descriptor.action_class != DANG_HARDWARE_NORMAL_V1) {
        auto finding = PluginFinding(plugin.name, {},
                                     "invalid hardware action class");
        Abort();
        return {finding, std::nullopt, {}};
      }
      const std::string path =
          descriptor.instance_path ? descriptor.instance_path : "";
      planned.action_ids.push_back(id);
      actions.push_back({
          id, path, action_class, std::move(dependencies),
          [&plugin, local_id]() -> std::optional<std::string> {
            DangPluginErrorV1 error{};
            if (plugin.apply_hardware_action(
                    plugin.api->context, plugin.prepared, local_id.c_str(),
                    &error))
              return std::nullopt;
            return error.message ? std::string(error.message) : "apply failed";
          },
          [&plugin, local_id]() -> std::optional<std::string> {
            DangPluginErrorV1 error{};
            if (plugin.rollback_hardware_action(
                    plugin.api->context, plugin.prepared, local_id.c_str(),
                    &error))
              return std::nullopt;
            return error.message ? std::string(error.message)
                                 : "rollback failed";
          }});
    }
    planned_plugins.push_back(std::move(planned));
  }

  for (const PlannedPlugin& planned : planned_plugins) {
    const State::Plugin& plugin = state_->plugins[planned.index];
    for (const std::string& dependency_module : plugin.dependencies) {
      const auto owner = module_owners.find(dependency_module);
      if (owner == module_owners.end()) continue;
      const auto provider = std::ranges::find(
          planned_plugins, owner->second, &PlannedPlugin::index);
      if (provider == planned_plugins.end()) continue;
      for (HardwareAction& action : actions) {
        if (std::ranges::find(planned.action_ids, action.id) ==
            planned.action_ids.end())
          continue;
        action.dependencies.insert(action.dependencies.end(),
                                   provider->action_ids.begin(),
                                   provider->action_ids.end());
      }
    }
  }

  HardwareTransactionPlanner planner;
  HardwareTransactionResult planned = planner.Plan(std::move(actions));
  if (!planned.ok) {
    DangPluginErrorV1 error{planned.message.c_str(),
                            planned.instance_path.empty()
                                ? nullptr
                                : planned.instance_path.c_str()};
    auto finding = PluginFinding("hardware planner", error, "planning failed");
    finding.netconf_error_app_tag = "hardware-plan-invalid";
    Abort();
    return {finding, std::nullopt, {}};
  }
  HardwareTransactionResult applied = planner.Apply();
  if (!applied.ok) {
    DangPluginErrorV1 error{applied.message.c_str(),
                            applied.instance_path.empty()
                                ? nullptr
                                : applied.instance_path.c_str()};
    auto finding = PluginFinding("hardware planner", error, "apply failed");
    if (!applied.rollback_failures.empty()) {
      {
        std::lock_guard lock(state_->reconciliation_mutex);
        state_->remnants = applied.remnants;
      }
      finding.netconf_error_app_tag = "hardware-state-diverged";
      finding.message +=
          "; rollback incomplete; hardware may diverge from running: ";
      for (std::size_t index = 0; index < applied.rollback_failures.size();
           ++index) {
        if (index != 0) finding.message += ", ";
        finding.message += applied.rollback_failures[index];
      }
    }
    Abort();
    return {finding, std::nullopt, {}};
  }
  {
    std::lock_guard lock(state_->reconciliation_mutex);
    state_->remnants.clear();
  }
  yang::config::ConfigDocument accepted = proposed;
  std::string accepted_xml = proposed.ToXml();
  std::vector<ConfigurationOutcome> outcomes;
  std::set<std::string> claimed_paths;
  for (const std::size_t index : state_->order) {
    State::Plugin& plugin = state_->plugins[index];
    if (!plugin.reconcile_applied) continue;
    DangAppliedConfigurationV1 report{};
    DangPluginErrorV1 error{};
    if (!plugin.reconcile_applied(plugin.api->context, plugin.prepared,
                                  accepted_xml.c_str(), &report, &error) ||
        !report.applied_xml ||
        (report.outcome_count != 0 && !report.outcomes)) {
      auto finding = PluginFinding(plugin.name, error,
                                   "invalid applied-state report");
      for (auto rollback = state_->order.rbegin();
           rollback != state_->order.rend(); ++rollback) {
        State::Plugin& applied_plugin = state_->plugins[*rollback];
        DangPluginErrorV1 ignored{};
        applied_plugin.api->rollback(applied_plugin.api->context,
                                     applied_plugin.prepared, &ignored);
      }
      Abort();
      return {finding, std::nullopt, {}};
    }
    auto parsed = yang::config::ParseDatastoreXml(
        schema, report.applied_xml, {.allow_origin_metadata = true});
    if (!parsed.document) {
      auto finding = parsed.findings.empty()
          ? PluginFinding(plugin.name, {}, "applied-state XML is invalid")
          : parsed.findings.front();
      finding.message = "plugin " + plugin.name +
                        " returned invalid applied state: " + finding.message;
      for (auto rollback = state_->order.rbegin();
           rollback != state_->order.rend(); ++rollback) {
        State::Plugin& applied_plugin = state_->plugins[*rollback];
        DangPluginErrorV1 ignored{};
        applied_plugin.api->rollback(applied_plugin.api->context,
                                     applied_plugin.prepared, &ignored);
      }
      Abort();
      return {finding, std::nullopt, {}};
    }
    for (const auto& change : yang::config::DiffConfigDocuments(
             schema, accepted, *parsed.document)) {
      const std::string& module = schema.Get(change.schema).module_name;
      if (std::ranges::find(plugin.modules, module) != plugin.modules.end())
        continue;
      auto finding = PluginFinding(
          plugin.name, {},
          "applied-state report modified a module it does not own");
      finding.instance_path = change.instance_path;
      finding.module_name = module;
      for (auto rollback = state_->order.rbegin();
           rollback != state_->order.rend(); ++rollback) {
        State::Plugin& applied_plugin = state_->plugins[*rollback];
        DangPluginErrorV1 ignored{};
        applied_plugin.api->rollback(applied_plugin.api->context,
                                     applied_plugin.prepared, &ignored);
      }
      Abort();
      return {finding, std::nullopt, {}};
    }
    for (std::size_t outcome_index = 0;
         outcome_index < report.outcome_count; ++outcome_index) {
      const DangConfigurationOutcomeV1& outcome =
          report.outcomes[outcome_index];
      const bool valid_disposition =
          outcome.disposition >= DANG_CONFIGURATION_APPLIED_V1 &&
          outcome.disposition <= DANG_CONFIGURATION_DELAYED_V1;
      if (!outcome.instance_path || !*outcome.instance_path ||
          !valid_disposition ||
          !claimed_paths.insert(outcome.instance_path).second) {
        auto finding = PluginFinding(
            plugin.name, {}, "invalid or duplicate configuration outcome");
        finding.instance_path =
            outcome.instance_path ? outcome.instance_path : "";
        for (auto rollback = state_->order.rbegin();
             rollback != state_->order.rend(); ++rollback) {
          State::Plugin& applied_plugin = state_->plugins[*rollback];
          DangPluginErrorV1 ignored{};
          applied_plugin.api->rollback(applied_plugin.api->context,
                                       applied_plugin.prepared, &ignored);
        }
        Abort();
        return {finding, std::nullopt, {}};
      }
      outcomes.push_back({plugin.name, outcome.instance_path,
                          outcome.disposition,
                          outcome.reason ? outcome.reason : ""});
    }
    accepted = std::move(*parsed.document);
    accepted_xml = report.applied_xml;
  }
  {
    std::lock_guard lock(state_->reconciliation_mutex);
    state_->outcomes = std::move(outcomes);
  }
  state_->ReleasePrepared();
  return {std::nullopt, std::move(accepted), std::move(accepted_xml)};
}

void PluginManager::Abort() noexcept { state_->ReleasePrepared(); }

yang::netconf::OperationResult PluginManager::InvokeRpc(
    const yang::netconf::RpcSessionContext& session,
    const yang::config::RuntimeSchemaNode& operation,
    std::string_view operation_xml) {
  (void)session;
  return InvokeAction(session, operation, {}, operation_xml);
}

yang::netconf::OperationResult PluginManager::InvokeAction(
    const yang::netconf::RpcSessionContext& session,
    const yang::config::RuntimeSchemaNode& operation,
    std::string_view instance_path, std::string_view operation_xml) {
  (void)session;
  const auto owner = std::ranges::find_if(
      state_->plugins, [&](const State::Plugin& plugin) {
        return std::ranges::find(plugin.modules, operation.module_name) !=
               plugin.modules.end();
      });
  if (owner == state_->plugins.end() || owner->invoke == nullptr) {
    yang::config::ValidationFinding finding;
    finding.message = owner == state_->plugins.end()
        ? "no plugin owns the operation's module"
        : "the module plugin does not implement operation dispatch";
    finding.module_name = operation.module_name;
    finding.netconf_error_tag = "operation-not-supported";
    finding.netconf_error_app_tag = "plugin-operation-unsupported";
    return {{false, {std::move(finding)}, {}}, {}};
  }
  const std::string input(operation_xml);
  const std::string path(instance_path);
  const DangOperationV1 request{operation.module_name.c_str(),
                                operation.name.local_name.c_str(),
                                path.empty() ? nullptr : path.c_str(),
                                input.c_str()};
  DangOperationResultV1 response{};
  DangPluginErrorV1 error{};
  if (!owner->invoke(owner->api->context, &request, &response, &error)) {
    return {{false, {PluginFinding(owner->name, error,
                                    "operation invocation failed")}, {}}, {}};
  }
  return {{true, {}, {}}, response.output_xml ? response.output_xml : ""};
}

}  // namespace dangd
