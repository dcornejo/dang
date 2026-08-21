// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_manager.h"

#include <algorithm>
#include <dlfcn.h>
#include <functional>
#include <iterator>
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
    std::string name;
    std::vector<std::string> modules;
    std::vector<std::string> dependencies;
    void* prepared = nullptr;
    bool affected = false;
    bool applied = false;
  };

  std::vector<Plugin> plugins;
  std::vector<PluginYangSource> sources;
  std::vector<std::size_t> order;
  std::string before_xml;
  std::string proposed_xml;
  std::string changes_json;

  void ReleasePrepared() noexcept {
    for (Plugin& plugin : plugins) {
      if (plugin.prepared && plugin.api->release)
        plugin.api->release(plugin.api->context, plugin.prepared);
      plugin.prepared = nullptr;
      plugin.affected = false;
      plugin.applied = false;
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
  if (api_v3 == nullptr && api_v2 == nullptr && v1_error != nullptr) {
    errors->push_back("plugin " + path.string() +
                      " has no supported dang_plugin_init entry point");
    dlclose(library);
    return false;
  }
  const DangPluginV1* api = api_v3 ? &api_v3->v2.v1
      : (api_v2 ? &api_v2->v1 : (initialize ? initialize() : nullptr));
  if (!api || api->abi_version !=
                  (api_v3 ? DANG_PLUGIN_ABI_V3
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
  plugin.invoke = api_v3 ? api_v3->v2.invoke
                         : (api_v2 ? api_v2->invoke : nullptr);
  plugin.operational = api_v3 ? api_v3->get_operational_data : nullptr;
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

std::vector<std::string> PluginManager::OperationalData() const {
  std::vector<std::string> result;
  for (const State::Plugin& plugin : state_->plugins) {
    if (!plugin.operational) continue;
    DangOperationalDataV1 data{};
    DangPluginErrorV1 error{};
    if (plugin.operational(plugin.api->context, &data, &error) && data.data_xml)
      result.emplace_back(data.data_xml);
  }
  return result;
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

std::optional<yang::config::ValidationFinding> PluginManager::Apply() {
  for (const std::size_t index : state_->order) {
    State::Plugin& plugin = state_->plugins[index];
    DangPluginErrorV1 error{};
    if (!plugin.api->apply(plugin.api->context, plugin.prepared, &error)) {
      auto finding = PluginFinding(plugin.name, error, "apply failed");
      std::vector<std::string> rollback_failures;
      for (auto rollback = state_->order.rbegin();
           rollback != state_->order.rend(); ++rollback) {
        State::Plugin& applied = state_->plugins[*rollback];
        if (!applied.applied) continue;
        DangPluginErrorV1 rollback_error{};
        if (!applied.api->rollback(applied.api->context, applied.prepared,
                                   &rollback_error)) {
          rollback_failures.push_back(
              "plugin " + applied.name + ": " +
              (rollback_error.message ? rollback_error.message
                                      : "rollback failed"));
        }
      }
      if (!rollback_failures.empty()) {
        finding.netconf_error_app_tag = "plugin-rollback-failed";
        finding.message += "; rollback also failed for ";
        for (std::size_t failure = 0; failure < rollback_failures.size();
             ++failure) {
          if (failure != 0) finding.message += ", ";
          finding.message += rollback_failures[failure];
        }
      }
      Abort();
      return finding;
    }
    plugin.applied = true;
  }
  state_->ReleasePrepared();
  return std::nullopt;
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
