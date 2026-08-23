// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/hardware_transaction.h"

#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace dangd {
namespace {

HardwareAction Action(std::string id, std::string path,
                      HardwareActionClass action_class,
                      std::vector<std::string>* events,
                      std::vector<std::string> dependencies = {}) {
  const std::string captured = id;
  return {std::move(id), std::move(path), action_class,
          std::move(dependencies),
          [events, captured] {
            events->push_back("apply " + captured);
            return std::optional<std::string>{};
          },
          [events, captured] {
            events->push_back("rollback " + captured);
            return std::optional<std::string>{};
          }};
}

TEST(HardwareTransactionPlannerTest,
     AppliesDeactivationFirstAndActivationLastWithDependencies) {
  std::vector<std::string> events;
  HardwareTransactionPlanner planner;
  std::vector<HardwareAction> actions;
  actions.push_back(Action("enable-interface", "/interfaces/interface[name='e0']/enabled",
                           HardwareActionClass::kActivate, &events));
  actions.push_back(Action("install-acl", "/acls/acl[name='edge']",
                           HardwareActionClass::kNormal, &events));
  actions.push_back(Action("remove-old-filter", "/acls/acl[name='old']",
                           HardwareActionClass::kDeactivate, &events));
  actions.push_back(Action("attach-filter", "/interfaces/interface[name='e0']/filter",
                           HardwareActionClass::kNormal, &events,
                           {"install-acl"}));
  const HardwareTransactionResult planned = planner.Plan(std::move(actions));
  ASSERT_TRUE(planned.ok) << planned.message;
  EXPECT_EQ(planned.execution_order,
            (std::vector<std::string>{"remove-old-filter", "install-acl",
                                      "attach-filter", "enable-interface"}));
  EXPECT_TRUE(planner.Apply().ok);
}

TEST(HardwareTransactionPlannerTest, RejectsMissingAndCyclicDependencies) {
  std::vector<std::string> events;
  HardwareTransactionPlanner missing;
  EXPECT_FALSE(missing.Plan({Action("a", "/a", HardwareActionClass::kNormal,
                                    &events, {"absent"})}).ok);
  HardwareTransactionPlanner cycle;
  std::vector<HardwareAction> actions;
  actions.push_back(Action("a", "/a", HardwareActionClass::kNormal, &events,
                           {"b"}));
  actions.push_back(Action("b", "/b", HardwareActionClass::kNormal, &events,
                           {"a"}));
  EXPECT_FALSE(cycle.Plan(std::move(actions)).ok);
}

TEST(HardwareTransactionPlannerTest,
     RollsBackCompletedActionsAndReportsStateDivergence) {
  std::vector<std::string> events;
  HardwareTransactionPlanner planner;
  HardwareAction first = Action("reserve", "/hardware/reservation",
                                HardwareActionClass::kNormal, &events);
  first.rollback = [&events] {
    events.push_back("rollback reserve");
    return std::optional<std::string>("reservation could not be restored");
  };
  HardwareAction failing = Action("program", "/hardware/program",
                                  HardwareActionClass::kNormal, &events,
                                  {"reserve"});
  failing.apply = [&events] {
    events.push_back("apply program");
    return std::optional<std::string>("device rejected operation");
  };
  ASSERT_TRUE(planner.Plan({std::move(first), std::move(failing)}).ok);
  const HardwareTransactionResult result = planner.Apply();
  EXPECT_FALSE(result.ok);
  ASSERT_EQ(result.rollback_failures.size(), 1U);
  EXPECT_NE(result.rollback_failures.front().find("could not be restored"),
            std::string::npos);
  EXPECT_EQ(events, (std::vector<std::string>{"apply reserve", "apply program",
                                               "rollback reserve"}));
}

}  // namespace
}  // namespace dangd
