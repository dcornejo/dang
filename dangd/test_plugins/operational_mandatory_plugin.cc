// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>

#ifndef DANG_MANDATORY_PROVIDER_NAME
#error "DANG_MANDATORY_PROVIDER_NAME must identify this test plugin"
#endif

namespace {

#ifdef DANG_MANDATORY_MODEL_OWNER
constexpr char kModel[] = R"yang(module dangd-test-operational-mandatory {
  yang-version 1.1;
  namespace "urn:dangd:test:operational-mandatory";
  prefix mandatory;
  revision 2026-08-23;
  container state {
    config false;
    list endpoint {
      key "name";
      leaf name { type string; }
      leaf status { type string; mandatory true; }
    }
  }
})yang";
#endif

size_t SourceCount(void*) {
#ifdef DANG_MANDATORY_MODEL_OWNER
  return 1;
#else
  return 0;
#endif
}

int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
#ifdef DANG_MANDATORY_MODEL_OWNER
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-operational-mandatory", "2026-08-23", kModel,
             std::strlen(kModel), "plugin:test-operational-mandatory",
             DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
  return 1;
#else
  (void)index;
  (void)source;
  return 0;
#endif
}

size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, size_t) { return nullptr; }
int Prepare(void*, const DangTransactionV1*, void** prepared,
            DangPluginErrorV1*) {
  if (!prepared) return 0;
  *prepared = new int(0);
  return 1;
}
int Success(void*, void*, DangPluginErrorV1*) { return 1; }
void Release(void*, void* prepared) { delete static_cast<int*>(prepared); }
#ifdef DANG_MANDATORY_DATA_PROVIDER
size_t ActionCount(void*, void*) { return 0; }
int ActionAt(void*, void*, size_t, DangHardwareActionV1*, DangPluginErrorV1*) {
  return 0;
}
int ApplyAction(void*, void*, const char*, DangPluginErrorV1*) { return 0; }

int OperationalV2(void*, DangOperationalDataV2* result, DangPluginErrorV1*) {
  if (!result) return 0;
  result->data_xml =
      "<state xmlns=\"urn:dangd:test:operational-mandatory\">"
      "<endpoint><name>uplink</name></endpoint></state>";
  // Completeness makes the absent mandatory status leaf an error. An ABI-v3
  // provider would leave this collection selected and the absence unknown.
  result->complete = 1;
  return 1;
}
#endif

#ifdef DANG_MANDATORY_DATA_PROVIDER
const DangPluginV5 kPlugin{{{{{
    DANG_PLUGIN_ABI_V5, DANG_MANDATORY_PROVIDER_NAME, nullptr, SourceCount,
    SourceAt, DependencyCount, DependencyAt, Prepare, Success, Success,
    Success, Release, nullptr}, nullptr}, nullptr}, ActionCount, ActionAt,
    ApplyAction, ApplyAction},
    OperationalV2
};
#else
const DangPluginV1 kPlugin{
    DANG_PLUGIN_ABI_V1, DANG_MANDATORY_PROVIDER_NAME, nullptr, SourceCount,
    SourceAt, DependencyCount, DependencyAt, Prepare, Success, Success,
    Success, Release, nullptr};
#endif

}  // namespace

#ifdef DANG_MANDATORY_DATA_PROVIDER
extern "C" const DangPluginV5* dang_plugin_init_v5() { return &kPlugin; }
#else
extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }
#endif
