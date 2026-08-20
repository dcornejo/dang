// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "ip_management_models.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using nlohmann::json;

struct Action {
  std::string forward;
  std::string reverse;
};

struct Prepared {
  std::vector<Action> actions;
};

constexpr std::string_view kInterfacesModule = "ietf-interfaces";
constexpr std::string_view kIpModule = "ietf-ip";

size_t SourceCount(void*) { return 2; }

int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (!source) return 0;
  if (index == 0) {
    *source = {"ietf-interfaces", "2018-02-20",
               dangd::ip_management::kIetfInterfacesYang.data(),
               dangd::ip_management::kIetfInterfacesYang.size(),
               "urn:ietf:rfc:8343", DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
    return 1;
  }
  if (index == 1) {
    *source = {"ietf-ip", "2018-02-22",
               dangd::ip_management::kIetfIpYang.data(),
               dangd::ip_management::kIetfIpYang.size(),
               "urn:ietf:rfc:8344", DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
    return 1;
  }
  return 0;
}

size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, size_t) { return nullptr; }

std::optional<std::string> JsonValue(const json& value) {
  if (value.is_null()) return std::nullopt;
  if (value.is_string()) return value.get<std::string>();
  return value.dump();
}

std::string Quoted(const std::optional<std::string>& value) {
  return value ? json(*value).dump() : "<absent>";
}

Action MakeAction(const json& change) {
  const int kind = change.at("kind").get<int>();
  const std::string path = change.at("path").get<std::string>();
  const auto before = JsonValue(change.at("before"));
  const auto after = JsonValue(change.at("after"));
  if (kind == 0) {
    return {"create " + path + (after ? " with value " + Quoted(after) : ""),
            "delete " + path};
  }
  if (kind == 1) {
    return {"delete " + path,
            "create " + path + (before ? " with value " + Quoted(before) : "")};
  }
  if (kind == 2) {
    return {"set " + path + " from " + Quoted(before) + " to " + Quoted(after),
            "set " + path + " from " + Quoted(after) + " to " + Quoted(before)};
  }
  return {"replace subtree " + path, "restore subtree " + path};
}

int Prepare(void*, const DangTransactionV1* transaction, void** result,
            DangPluginErrorV1* error) {
  if (!transaction || !transaction->changes_json || !result) return 0;
  try {
    auto prepared = std::make_unique<Prepared>();
    for (const json& change : json::parse(transaction->changes_json)) {
      const std::string module = change.at("module").get<std::string>();
      if (module == kInterfacesModule || module == kIpModule)
        prepared->actions.push_back(MakeAction(change));
    }
    *result = prepared.release();
    return 1;
  } catch (const json::exception&) {
    if (error) {
      error->message = "dangd supplied malformed configuration changes";
      error->instance_path = nullptr;
    }
    return 0;
  }
}

int Validate(void*, void* opaque, DangPluginErrorV1*) {
  return opaque != nullptr;
}

int Apply(void*, void* opaque, DangPluginErrorV1*) {
  if (!opaque) return 0;
  for (const Action& action : static_cast<Prepared*>(opaque)->actions)
    std::clog << "ip-management: " << action.forward << '\n';
  return 1;
}

int Rollback(void*, void* opaque, DangPluginErrorV1*) {
  if (!opaque) return 0;
  const auto& actions = static_cast<Prepared*>(opaque)->actions;
  for (auto action = actions.rbegin(); action != actions.rend(); ++action)
    std::clog << "ip-management rollback: " << action->reverse << '\n';
  return 1;
}

void Release(void*, void* opaque) { delete static_cast<Prepared*>(opaque); }

const DangPluginV1 kPlugin{
    DANG_PLUGIN_ABI_V1,
    "dangd-ip-management",
    nullptr,
    SourceCount,
    SourceAt,
    DependencyCount,
    DependencyAt,
    Prepare,
    Validate,
    Apply,
    Rollback,
    Release,
    nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }
