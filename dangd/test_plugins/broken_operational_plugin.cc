// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>
#include <csignal>
#include <chrono>
#include <string>
#include <thread>

namespace {

constexpr char kModel[] = R"yang(module dangd-test-broken-operational {
  yang-version 1.1;
  namespace "urn:dangd:test:broken-operational";
  prefix bo;
  revision 2026-08-23;
  leaf counter { config false; type uint16; }
})yang";

size_t SourceCount(void*) { return 1; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-broken-operational", "2026-08-23", kModel,
             std::strlen(kModel), "plugin:test-broken-operational",
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
int Operational(void*, DangOperationalDataV1* result, DangPluginErrorV1*) {
  if (!result) return 0;
#ifdef DANG_HANG_OPERATIONAL
  std::this_thread::sleep_for(std::chrono::hours(1));
  return 0;
#elif defined(DANG_CRASH_OPERATIONAL)
  std::raise(SIGABRT);
  return 0;
#elif defined(DANG_OVERSIZED_OPERATIONAL)
  static const std::string oversized(16 * 1024 * 1024 + 1, 'x');
  result->data_xml = oversized.c_str();
#else
  result->data_xml =
      "<counter xmlns=\"urn:dangd:test:broken-operational\">invalid</counter>";
#endif
  return 1;
}

const DangPluginV3 kPlugin{{{
    DANG_PLUGIN_ABI_V3, "test-broken-operational", nullptr, SourceCount,
    SourceAt, DependencyCount, DependencyAt, Prepare, Success, Success,
    Success, Release, nullptr}, nullptr}, Operational};

}  // namespace

extern "C" const DangPluginV3* dang_plugin_init_v3() { return &kPlugin; }
