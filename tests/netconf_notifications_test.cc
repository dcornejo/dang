// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/module_resolver.h"
#include "yang/netconf_notifications.h"
#include "yang/source_file.h"

namespace yang::netconf {
namespace {
using namespace std::chrono_literals;

TEST(NetconfNotificationsTest, ParsesRfc3339OffsetsAndFractions) {
  const auto utc = ParseNotificationTime("2026-08-13T12:34:56.25Z");
  const auto offset = ParseNotificationTime("2026-08-13T02:34:56.25-10:00");
  ASSERT_TRUE(utc);
  ASSERT_TRUE(offset);
  EXPECT_EQ(*utc, *offset);
  EXPECT_FALSE(ParseNotificationTime("2026-13-13T12:34:56Z"));
  EXPECT_FALSE(ParseNotificationTime("2026-08-13T12:34:56"));
  EXPECT_EQ(FormatNotificationTime(*utc), "2026-08-13T12:34:56Z");
}

TEST(NetconfNotificationsTest, ReplaysFiltersAndCompletesAtStopTime) {
  NotificationManager manager;
  ASSERT_TRUE(manager.AddStream({"NETCONF", true, 8}));
  const auto now = ParseNotificationTime("2026-08-13T12:00:00Z").value();
  ASSERT_TRUE(manager.Publish("NETCONF", "events", "alarm",
      "<alarm xmlns=\"urn:events\"><severity>major</severity></alarm>",
      now - 2s));
  ASSERT_TRUE(manager.Publish("NETCONF", "events", "link-up",
      "<link-up xmlns=\"urn:events\"/>", now - 1s));
  SubscriptionRequest request;
  request.session_id = 10;
  request.username = "alice";
  request.start_time = now - 5s;
  request.stop_time = now + 5s;
  request.filter_xml =
      "<filter xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<alarm xmlns=\"urn:events\"/></filter>";
  ASSERT_TRUE(manager.Subscribe(std::move(request), now).ok);
  const std::vector<std::string> replay = manager.Drain(10, now);
  ASSERT_EQ(replay.size(), 2U);
  EXPECT_NE(replay[0].find("<alarm"), std::string::npos);
  EXPECT_NE(replay[1].find("replayComplete"), std::string::npos);
  ASSERT_TRUE(manager.Publish("NETCONF", "events", "alarm",
      "<alarm xmlns=\"urn:events\"/>", now + 1s));
  EXPECT_EQ(manager.Drain(10, now + 1s).size(), 1U);
  const auto complete = manager.Drain(10, now + 6s);
  ASSERT_EQ(complete.size(), 1U);
  EXPECT_NE(complete[0].find("notificationComplete"), std::string::npos);
  EXPECT_TRUE(manager.Drain(10, now + 7s).empty());
}

TEST(NetconfNotificationsTest, RejectsInvalidRequestsAndReplayOverflow) {
  NotificationManager manager(nullptr, 1, 4096);
  ASSERT_TRUE(manager.AddStream({"NETCONF", true, 8}));
  const auto now = std::chrono::system_clock::now();
  SubscriptionRequest stop_only;
  stop_only.session_id = 1;
  stop_only.username = "alice";
  stop_only.stop_time = now + 1s;
  EXPECT_EQ(manager.Subscribe(std::move(stop_only), now).error_tag,
            "missing-element");
  SubscriptionRequest invalid_filter;
  invalid_filter.session_id = 2;
  invalid_filter.username = "alice";
  invalid_filter.filter_xml = "<filter type=\"xpath\"/>";
  EXPECT_EQ(manager.Subscribe(std::move(invalid_filter), now).error_tag,
            "invalid-value");
  ASSERT_TRUE(manager.Publish("NETCONF", "events", "one",
                              "<one xmlns=\"urn:events\"/>", now - 2s));
  SubscriptionRequest replay;
  replay.session_id = 3;
  replay.username = "alice";
  replay.start_time = now - 3s;
  EXPECT_EQ(manager.Subscribe(std::move(replay), now).error_tag,
            "resource-denied");
}

TEST(NetconfNotificationsTest, AppliesNacmBeforeQueueing) {
  NacmPolicy policy;
  policy.set_read_default(AccessAction::kDeny);
  NotificationManager manager(&policy);
  ASSERT_TRUE(manager.AddStream({}));
  SubscriptionRequest request;
  request.session_id = 4;
  request.username = "guest";
  ASSERT_TRUE(manager.Subscribe(std::move(request)).ok);
  ASSERT_TRUE(manager.Publish("NETCONF", "events", "alarm",
                              "<alarm xmlns=\"urn:events\"/>"));
  EXPECT_TRUE(manager.Drain(4).empty());
  EXPECT_EQ(policy.counters().denied_notifications, 1U);
}

TEST(NetconfNotificationsTest, UsesOnePolicySnapshotForReplayAndLiveFanout) {
  const auto make_policy = [](std::string permitted_group) {
    NacmPolicy result;
    result.set_read_default(AccessAction::kDeny);
    result.AddUserToGroup("alice", "alpha");
    result.AddUserToGroup("bob", "beta");
    NacmRule rule;
    rule.name = "permit-" + permitted_group;
    rule.groups = {std::move(permitted_group)};
    rule.module_name = "events";
    rule.notification_name = "alarm";
    rule.operations = AccessMask(AccessOperation::kRead);
    rule.action = AccessAction::kPermit;
    result.AddRule(std::move(rule));
    return result;
  };
  NacmPolicy policy = make_policy("alpha");
  NacmPolicy alpha = policy;
  NacmPolicy beta = make_policy("beta");
  beta.PreserveRuntimeStateFrom(policy);
  NotificationManager manager(&policy, 4096, 16 * 1024 * 1024);
  ASSERT_TRUE(manager.AddStream({"NETCONF", true, 64}));
  for (const auto& [session, user] :
       std::vector<std::pair<std::uint32_t, std::string>>{{1, "alice"},
                                                          {2, "bob"}}) {
    SubscriptionRequest request;
    request.session_id = session;
    request.username = user;
    ASSERT_TRUE(manager.Subscribe(std::move(request)).ok);
  }

  std::atomic<bool> start = false;
  std::atomic<unsigned> inconsistent_fanouts = 0;
  std::thread replacement([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    for (unsigned generation = 0; generation < 4000; ++generation) {
      policy = generation % 2 == 0 ? beta : alpha;
      std::this_thread::yield();
    }
  });
  start.store(true, std::memory_order_release);
  const auto now = std::chrono::system_clock::now();
  for (unsigned event = 0; event < 1000; ++event) {
    const bool published = manager.Publish(
        "NETCONF", "events", "alarm",
        "<alarm xmlns=\"urn:events\"/>", now);
    EXPECT_TRUE(published);
    if (!published) {
      inconsistent_fanouts.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const std::size_t delivered =
        manager.Drain(1, now).size() + manager.Drain(2, now).size();
    if (delivered != 1)
      inconsistent_fanouts.fetch_add(1, std::memory_order_relaxed);
  }
  replacement.join();
  EXPECT_EQ(inconsistent_fanouts.load(), 0U);

  manager.RemoveSession(1);
  manager.RemoveSession(2);
  std::atomic<unsigned> inconsistent_replays = 0;
  std::thread replay_replacement([&] {
    for (unsigned generation = 0; generation < 4000; ++generation) {
      policy = generation % 2 == 0 ? beta : alpha;
      std::this_thread::yield();
    }
  });
  for (std::uint32_t session = 10; session < 210; ++session) {
    SubscriptionRequest request;
    request.session_id = session;
    request.username = "alice";
    request.start_time = now - 1h;
    const SubscriptionResult subscribed =
        manager.Subscribe(std::move(request), now);
    EXPECT_TRUE(subscribed.ok) << subscribed.error;
    if (!subscribed.ok) {
      inconsistent_replays.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    const std::size_t replayed = manager.Drain(session, now).size();
    // One replayComplete marker is always present. A complete policy snapshot
    // either admits all 64 retained events or denies every one of them.
    if (replayed != 1 && replayed != 65)
      inconsistent_replays.fetch_add(1, std::memory_order_relaxed);
    manager.RemoveSession(session);
  }
  replay_replacement.join();
  EXPECT_EQ(inconsistent_replays.load(), 0U);
}

TEST(NetconfNotificationsTest, DerivesAssociatedNotificationAncestorsFromSchema) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("events.yang", R"yang(module events {
    yang-version 1.1; namespace "urn:events"; prefix e;
    container interfaces {
      list interface { key name; leaf name { type string; }
        notification link-change;
      }
    }
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  const config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);

  NacmPolicy policy;
  policy.set_read_default(AccessAction::kDeny);
  policy.AddUserToGroup("alice", "operators");
  policy.AddRule({"read-eth0", "operators", "",
                  "/{urn:events}interfaces/{urn:events}interface"
                  "[{urn:events}name='eth0']",
                  AccessMask(AccessOperation::kRead),
                  AccessAction::kPermit, "events"});
  policy.AddRule({"hide-other-interfaces", "operators", "",
                  "/{urn:events}interfaces/{urn:events}interface",
                  AccessMask(AccessOperation::kRead),
                  AccessAction::kDeny, "events"});
  policy.AddRule({"read-interfaces", "operators", "",
                  "/{urn:events}interfaces",
                  AccessMask(AccessOperation::kRead),
                  AccessAction::kPermit, "events"});
  NacmRule notification;
  notification.name = "read-link-change";
  notification.groups = {"operators"};
  notification.module_name = "events";
  notification.notification_name = "link-change";
  notification.operations = AccessMask(AccessOperation::kRead);
  notification.action = AccessAction::kPermit;
  policy.AddRule(std::move(notification));

  NotificationManager manager(&policy, 1024, 16 * 1024 * 1024, &schema);
  manager.SetInstanceDataProvider([] {
    return R"xml(<data><interfaces xmlns="urn:events">
      <interface><name>eth0</name></interface>
      <interface><name>eth1</name></interface>
    </interfaces></data>)xml";
  });
  ASSERT_TRUE(manager.AddStream({}));
  SubscriptionRequest request;
  request.session_id = 5;
  request.username = "alice";
  ASSERT_TRUE(manager.Subscribe(std::move(request)).ok);
  EXPECT_TRUE(manager.Publish(
      "NETCONF", "events", "link-change",
      "<link-change xmlns=\"urn:events\"/>",
      std::chrono::system_clock::now(), false,
      "/{urn:events}interfaces/{urn:events}interface"
      "[{urn:events}name='eth0']/{urn:events}link-change"));
  EXPECT_EQ(manager.Drain(5).size(), 1U);

  EXPECT_TRUE(manager.Publish(
      "NETCONF", "events", "link-change",
      "<link-change xmlns=\"urn:events\"/>",
      std::chrono::system_clock::now(), false,
      "/{urn:events}interfaces/{urn:events}interface"
      "[{urn:events}name='eth1']/{urn:events}link-change"));
  EXPECT_TRUE(manager.Drain(5).empty());
  EXPECT_FALSE(manager.Publish(
      "NETCONF", "events", "link-change",
      "<link-change xmlns=\"urn:events\"/>",
      std::chrono::system_clock::now(), false,
      "/{urn:events}interfaces/{urn:events}interface"
      "[{urn:events}name='eth2']/{urn:events}link-change"));
  EXPECT_FALSE(manager.Publish(
      "NETCONF", "events", "link-change",
      "<link-change xmlns=\"urn:events\"/>",
      std::chrono::system_clock::now(), false,
      "/{urn:events}interfaces/{urn:events}interface/"
      "{urn:events}link-change"));
  EXPECT_FALSE(manager.Publish(
      "NETCONF", "events", "link-change",
      "<different-event xmlns=\"urn:events\"/>",
      std::chrono::system_clock::now(), false,
      "/{urn:events}interfaces/{urn:events}interface"
      "[{urn:events}name='eth0']/{urn:events}link-change"));
  EXPECT_TRUE(manager.Drain(5).empty());
}

TEST(NetconfNotificationsTest, BindsTopLevelPublicationToModeledIdentity) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("events.yang", R"yang(module events {
    yang-version 1.1; namespace "urn:events"; prefix e;
    notification alarm;
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  const config::RuntimeSchema schema =
      config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  NotificationManager manager(nullptr, 1024, 16 * 1024 * 1024, &schema);
  ASSERT_TRUE(manager.AddStream({}));

  EXPECT_TRUE(manager.Publish("NETCONF", "events", "alarm",
                              "<alarm xmlns=\"urn:events\"/>"));
  EXPECT_FALSE(manager.Publish("NETCONF", "events", "different",
                               "<alarm xmlns=\"urn:events\"/>"));
  EXPECT_FALSE(manager.Publish("NETCONF", "other", "alarm",
                               "<alarm xmlns=\"urn:events\"/>"));
  EXPECT_FALSE(manager.Publish("NETCONF", "events", "alarm",
                               "<alarm xmlns=\"urn:other\"/>"));
}
}  // namespace
}  // namespace yang::netconf
