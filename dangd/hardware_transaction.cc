// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/hardware_transaction.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <utility>

namespace dangd {
namespace {

bool IsAncestor(std::string_view ancestor, std::string_view descendant) {
  return ancestor.size() < descendant.size() &&
      descendant.starts_with(ancestor) && descendant[ancestor.size()] == '/';
}

void AddDependency(HardwareAction* action, std::string_view dependency) {
  if (action->id == dependency ||
      std::ranges::find(action->dependencies, dependency) !=
          action->dependencies.end()) {
    return;
  }
  action->dependencies.emplace_back(dependency);
}

}  // namespace

HardwareTransactionResult HardwareTransactionPlanner::Plan(
    std::vector<HardwareAction> actions) {
  Abort();
  std::unordered_map<std::string, std::size_t> indexes;
  for (std::size_t index = 0; index < actions.size(); ++index) {
    if (actions[index].id.empty() || !actions[index].apply ||
        !actions[index].rollback ||
        !indexes.emplace(actions[index].id, index).second) {
      return {.ok = false,
              .message = "hardware action identifiers and callbacks must be "
                         "nonempty and unique"};
    }
  }

  for (std::size_t left = 0; left < actions.size(); ++left) {
    for (std::size_t right = 0; right < actions.size(); ++right) {
      if (left == right) continue;
      if (actions[left].action_class == HardwareActionClass::kDeactivate &&
          actions[right].action_class != HardwareActionClass::kDeactivate) {
        AddDependency(&actions[right], actions[left].id);
      }
      if (actions[right].action_class == HardwareActionClass::kActivate &&
          actions[left].action_class != HardwareActionClass::kActivate) {
        AddDependency(&actions[right], actions[left].id);
      }
      if (IsAncestor(actions[left].instance_path,
                     actions[right].instance_path)) {
        if (actions[left].action_class != HardwareActionClass::kDeactivate &&
            actions[right].action_class !=
                HardwareActionClass::kDeactivate) {
          AddDependency(&actions[right], actions[left].id);
        } else if (actions[left].action_class ==
                       HardwareActionClass::kDeactivate &&
                   actions[right].action_class ==
                       HardwareActionClass::kDeactivate) {
          AddDependency(&actions[left], actions[right].id);
        }
      }
    }
  }

  std::vector<std::size_t> indegree(actions.size(), 0);
  std::vector<std::vector<std::size_t>> dependents(actions.size());
  for (std::size_t index = 0; index < actions.size(); ++index) {
    std::set<std::string> unique;
    for (const std::string& dependency : actions[index].dependencies) {
      const auto found = indexes.find(dependency);
      if (found == indexes.end()) {
        return {.ok = false,
                .message = "hardware action " + actions[index].id +
                           " requires missing action " + dependency,
                .instance_path = actions[index].instance_path};
      }
      if (!unique.insert(dependency).second) continue;
      ++indegree[index];
      dependents[found->second].push_back(index);
    }
  }
  std::set<std::pair<std::string, std::size_t>> ready;
  for (std::size_t index = 0; index < actions.size(); ++index)
    if (indegree[index] == 0) ready.emplace(actions[index].id, index);
  std::vector<std::size_t> order;
  while (!ready.empty()) {
    const std::size_t index = ready.begin()->second;
    ready.erase(ready.begin());
    order.push_back(index);
    for (const std::size_t dependent : dependents[index]) {
      if (--indegree[dependent] == 0)
        ready.emplace(actions[dependent].id, dependent);
    }
  }
  if (order.size() != actions.size())
    return {.ok = false,
            .message = "hardware action dependencies contain a cycle"};

  actions_ = std::move(actions);
  order_ = std::move(order);
  HardwareTransactionResult result;
  result.ok = true;
  for (const std::size_t index : order_)
    result.execution_order.push_back(actions_[index].id);
  return result;
}

HardwareTransactionResult HardwareTransactionPlanner::Apply() {
  HardwareTransactionResult result;
  for (const std::size_t index : order_)
    result.execution_order.push_back(actions_[index].id);
  std::vector<std::size_t> applied;
  for (const std::size_t index : order_) {
    if (const auto error = actions_[index].apply()) {
      result.message = "hardware action " + actions_[index].id +
          " failed: " + *error;
      result.instance_path = actions_[index].instance_path;
      for (auto rollback = applied.rbegin(); rollback != applied.rend();
           ++rollback) {
        if (const auto rollback_error = actions_[*rollback].rollback()) {
          result.rollback_failures.push_back(
              actions_[*rollback].id + ": " + *rollback_error);
          result.remnants.push_back({actions_[*rollback].id,
                                     actions_[*rollback].instance_path,
                                     *rollback_error});
        }
      }
      Abort();
      return result;
    }
    applied.push_back(index);
  }
  result.ok = true;
  Abort();
  return result;
}

void HardwareTransactionPlanner::Abort() noexcept {
  actions_.clear();
  order_.clear();
}

}  // namespace dangd
