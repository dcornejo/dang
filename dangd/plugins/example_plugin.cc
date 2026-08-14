// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"

#include <cstring>
#include <string>

namespace {

constexpr char kModel[] = R"yang(module dangd-example-plugin {
  yang-version 1.1;
  namespace "urn:dangd:example-plugin";
  prefix dep;
  revision "2026-08-13" { description "Initial reference plugin."; }
  container plugin-settings {
    description "Configuration owned by the reference dangd plugin.";
    leaf mode {
      type string;
      description "A demonstration mode; the value reject fails validation.";
    }
  }
})yang";

struct Prepared {
  std::string before;
  std::string proposed;
};

std::string active_configuration;

size_t SourceCount(void*) { return 1; }

int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (index != 0 || !source) return 0;
  *source = {"dangd-example-plugin", "2026-08-13", kModel,
             std::strlen(kModel), "plugin:dangd-example-plugin",
             DANG_YANG_IMPLEMENTED_V1, nullptr, 0};
  return 1;
}

size_t DependencyCount(void*) { return 0; }
const char* DependencyAt(void*, size_t) { return nullptr; }

int Prepare(void*, const DangTransactionV1* transaction, void** result,
            DangPluginErrorV1*) {
  if (!transaction || !result) return 0;
  *result = new Prepared{transaction->before_xml, transaction->proposed_xml};
  return 1;
}

int Validate(void*, void* opaque, DangPluginErrorV1* error) {
  const auto* prepared = static_cast<Prepared*>(opaque);
  if (prepared->proposed.find("<mode>reject</mode>") == std::string::npos)
    return 1;
  if (error) {
    error->message = "mode 'reject' is not supported by the reference plugin";
    error->instance_path = "/dep:plugin-settings/dep:mode";
  }
  return 0;
}

int Apply(void*, void* opaque, DangPluginErrorV1*) {
  active_configuration = static_cast<Prepared*>(opaque)->proposed;
  return 1;
}

int Rollback(void*, void* opaque, DangPluginErrorV1*) {
  active_configuration = static_cast<Prepared*>(opaque)->before;
  return 1;
}

void Release(void*, void* opaque) { delete static_cast<Prepared*>(opaque); }

const DangPluginV1 kPlugin{
    DANG_PLUGIN_ABI_V1,
    "dangd-example-plugin",
    nullptr,
    SourceCount,
    SourceAt,
    DependencyCount,
    DependencyAt,
    Prepare,
    Validate,
    Apply,
    Rollback,
    Release,
    nullptr};

}  // namespace

extern "C" const DangPluginV1* dang_plugin_init_v1() { return &kPlugin; }

extern "C" const char* dang_example_active_configuration() {
  return active_configuration.c_str();
}
