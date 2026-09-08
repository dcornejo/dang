// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr char kModel[] = R"yang(module dangd-test-concurrent-operational {
  yang-version 1.1;
  namespace "urn:dangd:test:concurrent-operational";
  prefix co;
  revision 2026-08-23;
  container callback-state {
    config false;
    leaf maximum-concurrency { type uint32; mandatory true; }
  }
})yang";

std::atomic<unsigned int> in_flight{0};
std::atomic<unsigned int> maximum_concurrency{0};

size_t SourceCount(void*) { return 1; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-concurrent-operational", "2026-08-23", kModel,
             std::strlen(kModel), "plugin:test-concurrent-operational",
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

int Operational(void*, DangOperationalDataV2* result, DangPluginErrorV1*) {
  if (!result) return 0;
  const unsigned int current = in_flight.fetch_add(1) + 1;
  unsigned int observed = maximum_concurrency.load();
  while (observed < current &&
         !maximum_concurrency.compare_exchange_weak(observed, current)) {
  }
  // Keep callbacks overlapped long enough for the end-to-end stress test to
  // prove that dangd does not accidentally serialize provider retrievals. The
  // window accommodates coarse scheduler time slices on small FreeBSD guests.
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  static thread_local std::string xml;
  xml = "<callback-state xmlns=\"urn:dangd:test:concurrent-operational\">"
        "<maximum-concurrency>" +
        std::to_string(maximum_concurrency.load()) +
        "</maximum-concurrency></callback-state>";
  in_flight.fetch_sub(1);
  *result = {xml.c_str(), 1};
  return 1;
}

const DangPluginV5 kPlugin{{{{{
    DANG_PLUGIN_ABI_V5, "test-concurrent-operational", nullptr, SourceCount,
    SourceAt, DependencyCount, DependencyAt, Prepare, Success, Success,
    Success, Release, nullptr}, nullptr}, nullptr}, ActionCount, ActionAt,
    ApplyAction, ApplyAction}, Operational};

}  // namespace

extern "C" const DangPluginV5* dang_plugin_init_v5() { return &kPlugin; }
