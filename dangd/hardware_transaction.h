// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_HARDWARE_TRANSACTION_H_
#define DANGD_HARDWARE_TRANSACTION_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace dangd {

/** Generic activation semantics used to derive safe hardware ordering. */
enum class HardwareActionClass { kNormal, kActivate, kDeactivate };

/** One retained, reversible operation in a hardware transaction. */
struct HardwareAction {
  std::string id;
  std::string instance_path;
  HardwareActionClass action_class = HardwareActionClass::kNormal;
  std::vector<std::string> dependencies;
  std::function<std::optional<std::string>()> apply;
  std::function<std::optional<std::string>()> rollback;
};

/** Result of planning or applying a complete hardware transaction. */
struct HardwareTransactionResult {
  bool ok = false;
  std::string message;
  std::string instance_path;
  std::vector<std::string> rollback_failures;
  std::vector<std::string> execution_order;
};

/** Builds and executes deterministic dependency-ordered hardware plans. */
class HardwareTransactionPlanner {
 public:
  /**
   * Validates identifiers/dependencies and derives generic safety edges.
   *
   * Deactivation precedes every non-deactivation action, activation follows
   * every non-activation action, parents precede descendants on creation, and
   * descendants precede parents on deletion when plugins express those
   * operations using normal/activation classifications and instance paths.
   */
  [[nodiscard]] HardwareTransactionResult Plan(
      std::vector<HardwareAction> actions);
  /** Applies a successful retained plan and compensates failure in reverse. */
  [[nodiscard]] HardwareTransactionResult Apply();
  /** Drops a retained plan without performing hardware work. */
  void Abort() noexcept;

 private:
  std::vector<HardwareAction> actions_;
  std::vector<std::size_t> order_;
};

}  // namespace dangd

#endif  // DANGD_HARDWARE_TRANSACTION_H_
