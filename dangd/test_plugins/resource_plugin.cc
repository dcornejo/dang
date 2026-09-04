// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>

#ifndef DANG_RESOURCE_PLUGIN_NAME
#error "DANG_RESOURCE_PLUGIN_NAME is required"
#endif
#ifndef DANG_RESOURCE_MODULE_NAME
#error "DANG_RESOURCE_MODULE_NAME is required"
#endif
#ifndef DANG_RESOURCE_DOMAIN
#error "DANG_RESOURCE_DOMAIN is required"
#endif

namespace {

constexpr char kModel[] =
    "module " DANG_RESOURCE_MODULE_NAME " { yang-version 1.1; "
    "namespace 'urn:dangd:test:" DANG_RESOURCE_MODULE_NAME "'; "
    "prefix resource; revision 2026-09-03; }";

size_t SourceCount(void*) { return 1; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {DANG_RESOURCE_MODULE_NAME, "2026-09-03", kModel,
             std::strlen(kModel), "test:resource", DANG_YANG_IMPLEMENTED_V1,
             nullptr, 0};
  return 1;
}
int Prepare(void*, const DangTransactionV1*, void** prepared,
            DangPluginErrorV1*) {
  if (!prepared) return 0;
  *prepared = reinterpret_cast<void*>(1);
  return 1;
}
int Accept(void*, void*, DangPluginErrorV1*) { return 1; }
void Release(void*, void*) {}
size_t ResourceDomainCount(void*) { return 1; }
const char* ResourceDomainAt(void*, size_t index) {
  return index == 0 ? DANG_RESOURCE_DOMAIN : nullptr;
}

DangPluginV7 MakePlugin() {
  DangPluginV7 plugin{};
  plugin.v6.v5.v4.v3.v2.v1 =
      {DANG_PLUGIN_ABI_V7, DANG_RESOURCE_PLUGIN_NAME, nullptr, SourceCount,
       SourceAt, nullptr, nullptr, Prepare, Accept, Accept, Accept, Release,
       nullptr};
  // ABI versions are prefix-complete. These no-op callbacks make the small
  // discovery fixture a valid v7 provider without exercising unrelated APIs.
  plugin.v6.v5.v4.v3.v2.invoke =
      [](void*, const DangOperationV1*, DangOperationResultV1*,
         DangPluginErrorV1*) { return 0; };
  plugin.v6.v5.v4.v3.get_operational_data =
      [](void*, DangOperationalDataV1* result, DangPluginErrorV1*) {
        if (result) result->data_xml = "<data/>";
        return 1;
      };
  plugin.v6.v5.v4.hardware_action_count = [](void*, void*) -> size_t {
    return 0;
  };
  plugin.v6.v5.v4.hardware_action_at =
      [](void*, void*, size_t, DangHardwareActionV1*, DangPluginErrorV1*) {
        return 0;
      };
  plugin.v6.v5.v4.apply_hardware_action =
      [](void*, void*, const char*, DangPluginErrorV1*) { return 0; };
  plugin.v6.v5.v4.rollback_hardware_action =
      [](void*, void*, const char*, DangPluginErrorV1*) { return 0; };
  plugin.v6.v5.get_operational_data_v2 =
      [](void*, DangOperationalDataV2* result, DangPluginErrorV1*) {
        if (result) *result = {"<data/>", 1};
        return 1;
      };
  plugin.v6.reconcile_applied_configuration =
      [](void*, void*, const char* current, DangAppliedConfigurationV1* result,
         DangPluginErrorV1*) {
        if (result) *result = {current, nullptr, 0};
        return 1;
      };
  plugin.resource_domain_count = ResourceDomainCount;
  plugin.resource_domain_at = ResourceDomainAt;
  return plugin;
}

const DangPluginV7 kPlugin = MakePlugin();

}  // namespace

extern "C" const DangPluginV7* dang_plugin_init_v7() { return &kPlugin; }
