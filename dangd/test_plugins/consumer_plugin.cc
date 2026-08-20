// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "dangd/test_plugins/plugin_test_support.h"

#include <cstring>
#include <string>

namespace {

constexpr char kModel[] = R"yang(module dangd-test-consumer {
  yang-version 1.1;
  namespace "urn:dangd:test:consumer";
  prefix consumer;
  revision "2026-08-20" { description "Plugin integration test model."; }
  container consumer-settings { leaf enabled { type boolean; default true; } }
})yang";
constexpr const char* kDependencies[] = {"dangd-test-provider"};
struct Prepared { std::string before; std::string proposed; };

bool Contains(const Prepared* prepared, std::string_view value) {
  return prepared->proposed.find(value) != std::string::npos;
}
size_t SourceCount(void*) { return 1; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dangd-test-consumer", "2026-08-20", kModel,
             std::strlen(kModel), "test:consumer", DANG_YANG_IMPLEMENTED_V1,
             nullptr, 0};
  return 1;
}
size_t DependencyCount(void*) { return 1; }
const char* DependencyAt(void*, size_t index) {
  return index == 0 ? kDependencies[0] : nullptr;
}
int Prepare(void*, const DangTransactionV1* transaction, void** result,
            DangPluginErrorV1*) {
  dangd::test_plugin::Record("consumer.prepare");
  if (!transaction || !result) return 0;
  *result = new Prepared{transaction->before_xml, transaction->proposed_xml};
  return 1;
}
int Validate(void*, void* opaque, DangPluginErrorV1* error) {
  dangd::test_plugin::Record("consumer.validate");
  if (!Contains(static_cast<Prepared*>(opaque),
                "<mode>consumer-validate-fail</mode>")) return 1;
  if (error) {
    error->message = "provider mode is incompatible with the consumer";
    error->instance_path = "/provider:provider-settings/provider:mode";
  }
  return 0;
}
int Apply(void*, void* opaque, DangPluginErrorV1* error) {
  dangd::test_plugin::Record("consumer.apply");
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (dangd::test_plugin::Active("provider") != prepared->proposed) {
    if (error) error->message = "provider was not applied first";
    return 0;
  }
  if (Contains(prepared, "<mode>consumer-apply-fail</mode>") ||
      Contains(prepared, "<mode>consumer-apply-rollback-fail</mode>")) {
    if (error) {
      error->message = "simulated consumer hardware failure";
      error->instance_path = "/provider:provider-settings/provider:mode";
    }
    return 0;
  }
  dangd::test_plugin::SetActive("consumer", prepared->proposed);
  return 1;
}
int Rollback(void*, void* opaque, DangPluginErrorV1*) {
  dangd::test_plugin::Record("consumer.rollback");
  dangd::test_plugin::SetActive("consumer",
                               static_cast<Prepared*>(opaque)->before);
  return 1;
}
void Release(void*, void* opaque) {
  dangd::test_plugin::Record("consumer.release");
  delete static_cast<Prepared*>(opaque);
}

const DangPluginV1 kPlugin{
    DANG_PLUGIN_ABI_V1, "test-consumer", nullptr, SourceCount, SourceAt,
    DependencyCount, DependencyAt, Prepare, Validate, Apply, Rollback, Release,
    nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }
