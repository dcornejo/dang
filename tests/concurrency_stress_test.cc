// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/config_validation.h"
#include "yang/module_resolver.h"
#include "yang/nacm.h"
#include "yang/netconf_datastore.h"
#include "yang/netconf_notifications.h"
#include "yang/netconf_session_registry.h"
#include "yang/source_file.h"

namespace yang::netconf {
namespace {

struct StressFixture {
  config::RuntimeSchema schema;
  config::ConfigDocument document;
  config::EditDocument edit;
};

std::optional<StressFixture> BuildStressFixture() {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("stress.yang", R"yang(module stress {
    yang-version 1.1; namespace "urn:stress"; prefix s;
    leaf value { type uint32; mandatory true; }
  })yang", diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto document = config::ParseDatastoreXml(
      schema, "<value xmlns=\"urn:stress\">1</value>").document;
  auto edit = config::ParseEditXml(
      schema, "<value xmlns=\"urn:stress\">2</value>").document;
  if (!document || !edit) return std::nullopt;
  return StressFixture{std::move(schema), std::move(*document),
                       std::move(*edit)};
}

TEST(ConcurrencyStressTest, SharedSubsystemsRemainConsistent) {
  auto fixture = BuildStressFixture();
  ASSERT_TRUE(fixture);
  DatastoreManager stores(fixture->schema, fixture->document);
  NacmPolicy nacm;
  nacm.set_read_default(AccessAction::kDeny);
  nacm.set_write_default(AccessAction::kDeny);
  NotificationManager notifications(&nacm, 4096, 4 * 1024 * 1024);
  ASSERT_TRUE(notifications.AddStream({"NETCONF", true, 4096}));
  SessionRegistry sessions;
  constexpr std::uint32_t kThreadCount = 8;
  constexpr std::uint32_t kIterations = 250;
  for (std::uint32_t id = 1; id <= kThreadCount; ++id) {
    ASSERT_TRUE(sessions.Register(id, "user" + std::to_string(id)));
    SubscriptionRequest request;
    request.session_id = id;
    request.username = "user" + std::to_string(id);
    ASSERT_TRUE(notifications.Subscribe(std::move(request)).ok);
  }

  std::atomic<bool> failed = false;
  std::vector<std::thread> workers;
  for (std::uint32_t id = 1; id <= kThreadCount; ++id) {
    workers.emplace_back([&, id] {
      config::ConfigValidator validator;
      for (std::uint32_t iteration = 0; iteration < kIterations; ++iteration) {
        if (!validator.Validate({fixture->schema, fixture->document}).valid) {
          failed = true;
        }
        if (!stores.EditConfig(
                {std::to_string(id), Datastore::kCandidate,
                 {fixture->edit}}).ok) {
          failed = true;
        }
        if (stores.Read(Datastore::kCandidate).size() == 0) failed = true;
        if (nacm.AuthorizeData("guest", "stress", AccessOperation::kUpdate,
                               "/{urn:stress}value")) {
          failed = true;
        }
        if (!notifications.Publish(
                "NETCONF", "stress", "changed",
                "<changed xmlns=\"urn:stress\"/>")) {
          failed = true;
        }
        if (!sessions.Find(id) || sessions.List().size() != kThreadCount) {
          failed = true;
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();

  EXPECT_FALSE(failed.load());
  EXPECT_EQ(nacm.counters().denied_data_writes,
            kThreadCount * kIterations);
  EXPECT_EQ(nacm.counters().denied_notifications,
            kThreadCount * kIterations * kThreadCount);
  EXPECT_TRUE(stores.Validate(Datastore::kCandidate).ok);
  for (std::uint32_t id = 1; id <= kThreadCount; ++id) {
    EXPECT_TRUE(notifications.Drain(id).empty());
    EXPECT_TRUE(sessions.Unregister(id));
  }
}

}  // namespace
}  // namespace yang::netconf
