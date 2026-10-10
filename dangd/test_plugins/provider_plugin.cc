// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugin_api.h"
#include "dangd/test_plugins/plugin_test_support.h"

#include <cstring>
#include <atomic>
#include <string>

namespace {

constexpr char kModel[] = R"yang(module dangd-test-provider {
  yang-version 1.1;
  namespace "urn:dangd:test:provider";
  prefix provider;
  revision "2026-08-20" { description "Plugin integration test model."; }
  identity provider-mode;
  container provider-settings {
    leaf mode { type string; default "normal"; }
    leaf mode-kind { type identityref { base provider-mode; } }
  }
  rpc provider-status { output { leaf status { type string; } } }
  notification provider-event { leaf status { type string; } }
})yang";

constexpr char kIdentityLibrary[] = R"yang(module dangd-test-provider-types {
  yang-version 1.1;
  namespace "urn:dangd:test:provider:types";
  prefix provider-types;
  import dangd-test-provider { prefix provider; }
  revision "2026-10-10" {
    description "Import-only identities used by schema composition tests.";
  }
  identity guarded { base provider:provider-mode; }
  container unexpected-data {
    description
      "A sentinel proving import-only data nodes are not implemented.";
  }
})yang";

std::atomic<bool> notification_pending{true};

struct Prepared { std::string before; std::string proposed; };

size_t SourceCount(void*) { return 2; }
int SourceAt(void*, size_t index, DangYangSourceV1* source,
             DangPluginErrorV1*) {
  if (!source) return 0;
  if (index == 0) {
    *source = {"dangd-test-provider", "2026-08-20", kModel,
               std::strlen(kModel), "test:provider", DANG_YANG_IMPLEMENTED_V1,
               nullptr, 0};
    return 1;
  }
  if (index == 1) {
    *source = {"dangd-test-provider-types", "2026-10-10", kIdentityLibrary,
               std::strlen(kIdentityLibrary), "test:provider-types",
               DANG_YANG_IMPORT_ONLY_V1, nullptr, 0};
    return 1;
  }
  return 0;
}
size_t ResourceDomainCount(void*) { return 1; }
const char* ResourceDomainAt(void*, size_t index) {
  return index == 0 ? "routing" : nullptr;
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
int Invoke(void*, const DangOperationV1* operation,
           DangOperationResultV1* result, DangPluginErrorV1*) {
  if (!operation || !result ||
      std::string_view(operation->operation_name) != "provider-status") return 0;
  result->output_xml =
      "<status xmlns=\"urn:dangd:test:provider\">ready</status>";
  return 1;
}
int OperationalV2(void*, DangOperationalDataV2* result, DangPluginErrorV1*) {
  if (!result) return 0;
  *result = {"<data/>", 1};
  return 1;
}
size_t HardwareActionCount(void*, void*) { return 1; }
int HardwareActionAt(void*, void*, size_t index, DangHardwareActionV1* action,
                     DangPluginErrorV1*) {
  if (index != 0 || !action) return 0;
  *action = {"transaction", "", DANG_HARDWARE_NORMAL_V1, nullptr, 0};
  return 1;
}
int ApplyHardwareAction(void* context, void* prepared, const char*,
                        DangPluginErrorV1* error) {
  return Apply(context, prepared, error);
}
int RollbackHardwareAction(void* context, void* prepared, const char*,
                           DangPluginErrorV1* error) {
  return Rollback(context, prepared, error);
}
int ReconcileApplied(void*, void* opaque, const char* current_xml,
                     DangAppliedConfigurationV1* result,
                     DangPluginErrorV1*) {
  if (!opaque || !current_xml || !result) return 0;
  static thread_local std::string applied;
  static thread_local DangConfigurationOutcomeV1 outcomes[3];
  applied = current_xml;
  if (applied.find("<mode>backend-invalid-report</mode>") !=
      std::string::npos) {
    *result = {"<config>", nullptr, 0};
    return 1;
  }
  const std::string requested = "<mode>backend-transform</mode>";
  const std::size_t position = applied.find(requested);
  if (position == std::string::npos) {
    *result = {applied.c_str(), nullptr, 0};
    return 1;
  }
  applied.replace(
      position, requested.size(),
      "<mode xmlns:or=\"urn:ietf:params:xml:ns:yang:ietf-origin\" "
      "or:origin=\"or:system\">device-normalized</mode>");
  constexpr const char* kPath =
      "/{urn:dangd:test:provider}provider-settings/mode";
  outcomes[0] = {kPath, DANG_CONFIGURATION_TRANSFORMED_V1,
                 "hardware normalized the requested mode"};
  outcomes[1] = {
      "/{urn:dangd:test:provider}provider-settings/rejected-example",
      DANG_CONFIGURATION_REJECTED_V1, "unsupported optional setting"};
  outcomes[2] = {
      "/{urn:dangd:test:provider}provider-settings/delayed-example",
      DANG_CONFIGURATION_DELAYED_V1, "awaiting asynchronous convergence"};
  *result = {applied.c_str(), outcomes, 3};
  return 1;
}

int NextNotification(void*, DangNotificationV1* event, DangPluginErrorV1*) {
  if (!event || !notification_pending.exchange(false)) return 0;
  *event = {"NETCONF", "dangd-test-provider", "provider-event",
            "<provider-event xmlns=\"urn:dangd:test:provider\">"
            "<status>ready</status></provider-event>",
            "", 0};
  return 1;
}

size_t PeerCandidateCount(void*, void* opaque) {
  const auto* prepared = static_cast<const Prepared*>(opaque);
  if (!prepared || prepared->proposed.find("<mode>peer-plan-") ==
                       std::string::npos)
    return 0;
  return prepared->proposed.find("<mode>peer-plan-multi</mode>") !=
                 std::string::npos
      ? 4
      : 2;
}

int PeerCandidateAt(void*, void* opaque, size_t index,
                    DangPeerCandidateV1* candidate, DangPluginErrorV1*) {
  const auto* prepared = static_cast<const Prepared*>(opaque);
  const bool multiple_groups = prepared &&
      prepared->proposed.find("<mode>peer-plan-multi</mode>") !=
          std::string::npos;
  if (!candidate || index >= (multiple_groups ? 4U : 2U)) return 0;
  constexpr const char* kConfiguration =
      "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<provider-settings xmlns=\"urn:dangd:test:provider\">"
      "<mode>normal</mode></provider-settings></config>";
  const bool invalid = prepared &&
      prepared->proposed.find("<mode>peer-plan-invalid</mode>") !=
          std::string::npos;
  const bool primary = index % 2 == 0;
  *candidate = {invalid ? "invalid/group"
                        : multiple_groups && index >= 2 ? "second-group"
                                                        : "test-group",
                primary ? "primary" : "standby",
                false,
                primary ? DANG_PEER_PRIMARY_V1 : DANG_PEER_STANDBY_V1,
                60, "dangd-test-provider", kConfiguration,
                "{\"expected_status\":\"ready\"}"};
  return 1;
}

int VerifyPeer(void*, void*, const DangPeerVerificationV1* verification,
               DangPluginErrorV1* error) {
  if (verification &&
      std::string_view(verification->operational_reply_xml).find("waiting") !=
          std::string_view::npos) {
    if (error) error->message = "peer is still converging";
    return -1;
  }
  if (!verification ||
      std::string_view(verification->verification_context_json) !=
          "{\"expected_status\":\"ready\"}" ||
      std::string_view(verification->operational_reply_xml).find("ready") ==
          std::string_view::npos) {
    if (error) error->message = "peer did not report ready";
    return 0;
  }
  return 1;
}

DangPluginV9 MakePlugin() {
  DangPluginV9 plugin{};
  plugin.v8.v7.v6.v5.v4.v3.v2.v1 =
      {DANG_PLUGIN_ABI_V9, "test-provider", nullptr, SourceCount, SourceAt,
       nullptr, nullptr, Prepare, Validate, Apply, Rollback, Release, nullptr};
  plugin.v8.v7.v6.v5.v4.v3.v2.invoke = Invoke;
  plugin.v8.v7.v6.v5.v4.hardware_action_count = HardwareActionCount;
  plugin.v8.v7.v6.v5.v4.hardware_action_at = HardwareActionAt;
  plugin.v8.v7.v6.v5.v4.apply_hardware_action = ApplyHardwareAction;
  plugin.v8.v7.v6.v5.v4.rollback_hardware_action = RollbackHardwareAction;
  plugin.v8.v7.v6.v5.get_operational_data_v2 = OperationalV2;
  plugin.v8.v7.v6.reconcile_applied_configuration = ReconcileApplied;
  plugin.v8.v7.resource_domain_count = ResourceDomainCount;
  plugin.v8.v7.resource_domain_at = ResourceDomainAt;
  plugin.v8.next_notification = NextNotification;
  plugin.peer_candidate_count = PeerCandidateCount;
  plugin.peer_candidate_at = PeerCandidateAt;
  plugin.verify_peer = VerifyPeer;
  return plugin;
}
const DangPluginV9 kPlugin = MakePlugin();

}  // namespace

extern "C" const DangPluginV9* dang_plugin_init_v9() { return &kPlugin; }
extern "C" const DangPluginV8* dang_plugin_init_v8() { return &kPlugin.v8; }
