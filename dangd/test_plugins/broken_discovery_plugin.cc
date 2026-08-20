// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>

namespace {

constexpr char kModel[] = R"yang(module leaked-test-model {
  yang-version 1.1; namespace "urn:dangd:test:leaked"; prefix leaked;
})yang";
size_t SourceCount(void*) { return 2; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1* error) {
  if (index == 0 && source) {
    *source = {"leaked-test-model", nullptr, kModel, std::strlen(kModel),
               "test:leaked", DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
    return 1;
  }
  if (error) error->message = "simulated discovery failure";
  return 0;
}
int Prepare(void*, const DangTransactionV1*, void**, DangPluginErrorV1*) {
  return 0;
}
int Callback(void*, void*, DangPluginErrorV1*) { return 0; }
void Release(void*, void*) {}
const DangPluginV1 kPlugin{DANG_PLUGIN_ABI_V1, "broken-discovery", nullptr,
                           SourceCount, SourceAt, nullptr, nullptr, Prepare,
                           Callback, Callback, Callback, Release, nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }
