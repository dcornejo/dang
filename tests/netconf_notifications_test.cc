// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include <gtest/gtest.h>

#include "yang/netconf_notifications.h"

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
}  // namespace
}  // namespace yang::netconf
