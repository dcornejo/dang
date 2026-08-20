// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>

namespace {

constexpr char kModel[] = R"yang(module dangd-test-deviation {
  yang-version 1.1;
  namespace "urn:dangd:test:deviation";
  prefix dtd;
  import appliance { prefix appliance; }
  revision 2026-08-20;
  deviation "/appliance:system/appliance:hostname" {
    deviate add { units "host-label"; }
  }
})yang";

struct Prepared {};
size_t SourceCount(void*) { return 1; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-deviation", "2026-08-20", kModel,
             std::strlen(kModel), "test:deviation", DANG_YANG_DEVIATION_V1,
             nullptr, 0};
  return 1;
}
int Prepare(void*, const DangTransactionV1*, void** result,
            DangPluginErrorV1*) {
  if (!result) return 0;
  *result = new Prepared;
  return 1;
}
int Success(void*, void*, DangPluginErrorV1*) { return 1; }
void Release(void*, void* prepared) { delete static_cast<Prepared*>(prepared); }
const DangPluginV1 kPlugin{DANG_PLUGIN_ABI_V1, "test-deviation", nullptr,
                           SourceCount, SourceAt, nullptr, nullptr, Prepare,
                           Success, Success, Success, Release, nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }
