// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>

namespace {

constexpr char kModel[] = R"yang(module dangd-test-complete-operational {
  yang-version 1.1;
  namespace "urn:dangd:test:complete-operational";
  prefix co;
  revision 2026-08-23;
  container state {
    config false;
    leaf target { type string; }
    leaf target-ref { type leafref { path "../target"; } }
  }
})yang";

size_t SourceCount(void*) { return 1; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-complete-operational", "2026-08-23", kModel,
             std::strlen(kModel), "plugin:test-complete-operational",
             DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
  return 1;
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
  result->data_xml =
      "<state xmlns=\"urn:dangd:test:complete-operational\">"
      "<target-ref>missing</target-ref></state>";
  result->complete = 1;
  return 1;
}

const DangPluginV5 kPlugin{{{{{
    DANG_PLUGIN_ABI_V5, "test-complete-operational", nullptr, SourceCount,
    SourceAt, DependencyCount, DependencyAt, Prepare, Success, Success,
    Success, Release, nullptr}, nullptr}, nullptr}, ActionCount, ActionAt,
    ApplyAction, ApplyAction}, OperationalV2};

}  // namespace

extern "C" const DangPluginV5* dang_plugin_init_v5() { return &kPlugin; }
