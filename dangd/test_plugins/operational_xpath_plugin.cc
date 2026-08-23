// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>

#ifndef DANG_XPATH_PROVIDER_NAME
#error "DANG_XPATH_PROVIDER_NAME must identify this test provider"
#endif

namespace {

#ifdef DANG_XPATH_MODEL_OWNER
constexpr char kModel[] = R"yang(module dangd-test-operational-xpath {
  yang-version 1.1;
  namespace "urn:dangd:test:operational-xpath";
  prefix ox;
  revision 2026-08-23;
  leaf mode { config false; type string; }
  leaf must-state {
    config false;
    type string;
    must "/ox:mode = 'ready'";
  }
  leaf when-state {
    config false;
    type string;
    when "/ox:mode = 'ready'";
  }
})yang";
#endif

size_t SourceCount(void*) {
#ifdef DANG_XPATH_MODEL_OWNER
  return 1;
#else
  return 0;
#endif
}

int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
#ifdef DANG_XPATH_MODEL_OWNER
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-operational-xpath", "2026-08-23", kModel,
             std::strlen(kModel), "plugin:test-operational-xpath",
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

int Operational(void*, DangOperationalDataV1* result, DangPluginErrorV1*) {
  if (!result) return 0;
#ifdef DANG_XPATH_MODEL_OWNER
  result->data_xml =
      "<mode xmlns=\"urn:dangd:test:operational-xpath\">blocked</mode>";
#elif defined(DANG_XPATH_MUST_PROVIDER)
  result->data_xml =
      "<must-state xmlns=\"urn:dangd:test:operational-xpath\">active"
      "</must-state>";
#else
  result->data_xml =
      "<when-state xmlns=\"urn:dangd:test:operational-xpath\">active"
      "</when-state>";
#endif
  return 1;
}

const DangPluginV3 kPlugin{{{
    DANG_PLUGIN_ABI_V3, DANG_XPATH_PROVIDER_NAME, nullptr, SourceCount,
    SourceAt, DependencyCount, DependencyAt, Prepare, Success, Success,
    Success, Release, nullptr}, nullptr}, Operational};

}  // namespace

extern "C" const DangPluginV3* dang_plugin_init_v3() { return &kPlugin; }
