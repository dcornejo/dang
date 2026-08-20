// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "dangd/test_plugins/plugin_test_support.h"

#include <cstring>
#include <string>

namespace {

constexpr char kModel[] = R"yang(module dangd-test-provider {
  yang-version 1.1;
  namespace "urn:dangd:test:provider";
  prefix provider;
  revision "2026-08-20" { description "Plugin integration test model."; }
  container provider-settings {
    leaf mode { type string; default "normal"; }
  }
})yang";

struct Prepared { std::string before; std::string proposed; };

size_t SourceCount(void*) { return 1; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-provider", "2026-08-20", kModel,
             std::strlen(kModel), "test:provider", DANG_YANG_IMPLEMENTED_V1,
             nullptr, 0};
  return 1;
}
int Prepare(void*, const DangTransactionV1* transaction, void** result,
            DangPluginErrorV1*) {
  dangd::test_plugin::Record("provider.prepare");
  if (!transaction || !result) return 0;
  *result = new Prepared{transaction->before_xml, transaction->proposed_xml};
  return 1;
}
int Validate(void*, void*, DangPluginErrorV1*) {
  dangd::test_plugin::Record("provider.validate");
  return 1;
}
int Apply(void*, void* opaque, DangPluginErrorV1*) {
  dangd::test_plugin::Record("provider.apply");
  dangd::test_plugin::SetActive(
      "provider", static_cast<Prepared*>(opaque)->proposed);
  return 1;
}
int Rollback(void*, void* opaque, DangPluginErrorV1* error) {
  dangd::test_plugin::Record("provider.rollback");
  if (static_cast<Prepared*>(opaque)->proposed.find(
          "<mode>consumer-apply-rollback-fail</mode>") != std::string::npos) {
    if (error) error->message = "simulated provider rollback failure";
    return 0;
  }
  dangd::test_plugin::SetActive("provider",
                               static_cast<Prepared*>(opaque)->before);
  return 1;
}
void Release(void*, void* opaque) {
  dangd::test_plugin::Record("provider.release");
  delete static_cast<Prepared*>(opaque);
}

const DangPluginV1 kPlugin{DANG_PLUGIN_ABI_V1, "test-provider", nullptr,
                           SourceCount, SourceAt, nullptr, nullptr, Prepare,
                           Validate, Apply, Rollback, Release, nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }
