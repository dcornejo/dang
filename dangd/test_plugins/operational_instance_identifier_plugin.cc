// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>

#ifndef DANG_INSTANCE_PROVIDER_NAME
#error "DANG_INSTANCE_PROVIDER_NAME must identify this test plugin"
#endif

namespace {

#ifdef DANG_INSTANCE_MODEL_OWNER
constexpr char kModel[] = R"yang(module dangd-test-operational-instance {
  yang-version 1.1;
  namespace "urn:dangd:test:operational-instance";
  prefix oi;
  revision 2026-08-23;
  container targets {
    config false;
    list target { key "name"; leaf name { type string; } }
  }
  container references {
    config false;
    leaf selected { type instance-identifier; }
  }
})yang";
#endif

size_t SourceCount(void*) {
#ifdef DANG_INSTANCE_MODEL_OWNER
  return 1;
#else
  return 0;
#endif
}
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
#ifdef DANG_INSTANCE_MODEL_OWNER
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-operational-instance", "2026-08-23", kModel,
             std::strlen(kModel), "plugin:test-operational-instance",
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
size_t ActionCount(void*, void*) { return 0; }
int ActionAt(void*, void*, size_t, DangHardwareActionV1*, DangPluginErrorV1*) {
  return 0;
}
int ApplyAction(void*, void*, const char*, DangPluginErrorV1*) { return 0; }

int OperationalV2(void*, DangOperationalDataV2* result, DangPluginErrorV1*) {
  if (!result) return 0;
#ifdef DANG_INSTANCE_TARGET_PROVIDER
  result->data_xml =
      "<targets xmlns=\"urn:dangd:test:operational-instance\">"
      "<target><name>present</name></target></targets>";
  result->complete = 1;
#elif defined(DANG_INSTANCE_REFERENCE_PROVIDER)
  result->data_xml =
      "<references xmlns=\"urn:dangd:test:operational-instance\" "
      "xmlns:oi=\"urn:dangd:test:operational-instance\"><selected>"
      "/oi:targets/oi:target[oi:name='" DANG_INSTANCE_VALUE
      "']/oi:name</selected></references>";
  result->complete = 0;
#else
  result->data_xml = "<data xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"/>";
  result->complete = 0;
#endif
  return 1;
}

const DangPluginV5 kPlugin{{{{{
    DANG_PLUGIN_ABI_V5, DANG_INSTANCE_PROVIDER_NAME, nullptr, SourceCount,
    SourceAt, DependencyCount, DependencyAt, Prepare, Success, Success,
    Success, Release, nullptr}, nullptr}, nullptr}, ActionCount, ActionAt,
    ApplyAction, ApplyAction}, OperationalV2};

}  // namespace

extern "C" const DangPluginV5* dang_plugin_init_v5() { return &kPlugin; }
