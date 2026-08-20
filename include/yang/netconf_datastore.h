// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_DATASTORE_H_
#define YANG_NETCONF_DATASTORE_H_

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "yang/config_edit.h"

namespace yang::netconf {

/** Conventional configuration datastore, including RFC 8342 intended. */
enum class Datastore { kRunning, kCandidate, kStartup, kIntended };
/** RFC 6241 :validate test behavior for edit-config. */
enum class TestOption { kTestThenSet, kSet, kTestOnly };
/** RFC 6241 behavior after an edit error. */
enum class ErrorOption { kStopOnError, kContinueOnError, kRollbackOnError };

/** Result of one datastore operation. */
struct TransactionResult {
  bool ok = false;
  std::vector<config::ValidationFinding> errors;
  std::vector<config::ChangeEvent> changes;
};

/** One edit-config transaction, optionally containing multiple edit payloads. */
struct EditConfigRequest {
  std::string session;
  Datastore target = Datastore::kRunning;
  std::vector<config::EditDocument> edits;
  config::EditOperation default_operation = config::EditOperation::kMerge;
  TestOption test_option = TestOption::kTestThenSet;
  ErrorOption error_option = ErrorOption::kStopOnError;
  /** Optional transaction-boundary authorization for each proposed change. */
  std::function<bool(const config::ChangeEvent&)> authorize_change;
};

/** Parameters for a confirmed commit. */
struct ConfirmedCommitOptions {
  std::chrono::seconds timeout = std::chrono::seconds(600);
  std::optional<std::string> persist;
};

/** Serializable datastore and confirmed-commit recovery state. */
struct PersistentDatastoreState {
  std::string running_xml;
  std::string candidate_xml;
  std::string startup_xml;
  std::optional<std::string> rollback_running_xml;
  std::optional<std::int64_t> confirmation_expiry_unix_seconds;
  std::optional<std::string> confirming_session;
  std::optional<std::string> persist_token;
};

/**
 * Receives each atomic replacement of the effective running configuration.
 * Implementations are called under the datastore lock and must not reenter it.
 */
class RunningConfigBackend {
 public:
  virtual ~RunningConfigBackend() = default;
  [[nodiscard]] virtual std::optional<config::ValidationFinding>
  PrepareReplacement(
      const config::RuntimeSchema&,
      const config::ConfigDocument&,
      const config::ConfigDocument&,
      std::span<const config::ChangeEvent>) {
    return std::nullopt;
  }
  [[nodiscard]] virtual std::optional<config::ValidationFinding> Replace(
      const config::RuntimeSchema& schema,
      const config::ConfigDocument& before,
      const config::ConfigDocument& after,
      std::span<const config::ChangeEvent> changes) = 0;
  virtual void AbortPreparedReplacement() noexcept {}
};

/**
 * In-memory RFC 6241 datastore transaction manager.
 *
 * All methods are serialized. Documents returned by Read are immutable copies.
 * Call ProcessTimeouts from the host event loop to expire confirmed commits.
 */
class DatastoreManager {
 public:
  using Clock = std::chrono::steady_clock;

  DatastoreManager(const config::RuntimeSchema& schema,
                   config::ConfigDocument running,
                   std::optional<config::ConfigDocument> startup = std::nullopt,
                   RunningConfigBackend* backend = nullptr);

  [[nodiscard]] const config::RuntimeSchema& schema() const noexcept {
    return schema_;
  }
  [[nodiscard]] config::ConfigDocument Read(Datastore datastore) const;
  [[nodiscard]] TransactionResult Lock(Datastore datastore,
                                       std::string_view session);
  [[nodiscard]] TransactionResult Unlock(Datastore datastore,
                                         std::string_view session);
  [[nodiscard]] TransactionResult EditConfig(const EditConfigRequest& request);
  [[nodiscard]] TransactionResult Validate(Datastore datastore);
  [[nodiscard]] TransactionResult Commit(
      std::string_view session,
      std::optional<ConfirmedCommitOptions> confirmed = std::nullopt,
      std::function<bool(const config::ChangeEvent&)> authorize_change = {});
  [[nodiscard]] TransactionResult ConfirmCommit(
      std::string_view session,
      std::optional<std::string_view> persist_id = std::nullopt);
  /** Applies a follow-up confirmed commit in an existing persistent sequence. */
  [[nodiscard]] TransactionResult ContinueConfirmedCommit(
      std::string_view session, std::string_view persist_id,
      std::chrono::seconds timeout = std::chrono::seconds(600));
  [[nodiscard]] TransactionResult CancelCommit(
      std::string_view session,
      std::optional<std::string_view> persist_id = std::nullopt);
  [[nodiscard]] TransactionResult DiscardChanges(std::string_view session);
  [[nodiscard]] TransactionResult CopyConfig(std::string_view session,
                                             Datastore source,
                                             Datastore target,
      std::function<bool(const config::ChangeEvent&)> authorize_change = {});
  [[nodiscard]] TransactionResult DeleteConfig(std::string_view session,
                                               Datastore target);
  /** Releases locks and cancels that session's non-persistent confirmed commit. */
  void CloseSession(std::string_view session);
  /** Rolls back an expired confirmed commit; returns true when one expired. */
  bool ProcessTimeouts(Clock::time_point now = Clock::now());
  [[nodiscard]] PersistentDatastoreState ExportPersistentState() const;
  /** Atomically replaces unlocked state after parsing and validation. */
  [[nodiscard]] TransactionResult RestorePersistentState(
      const PersistentDatastoreState& state);

 private:
  [[nodiscard]] config::ConfigDocument& Mutable(Datastore datastore);
  [[nodiscard]] const config::ConfigDocument& Get(Datastore datastore) const;
  [[nodiscard]] TransactionResult CheckWriteAccess(
      Datastore datastore, std::string_view session) const;
  [[nodiscard]] TransactionResult ValidateDocument(
      const config::ConfigDocument& document) const;
  [[nodiscard]] TransactionResult ReplaceRunning(
      config::ConfigDocument replacement,
      std::vector<config::ChangeEvent> changes);

  const config::RuntimeSchema& schema_;
  mutable std::mutex mutex_;
  config::ConfigDocument running_;
  config::ConfigDocument candidate_;
  config::ConfigDocument startup_;
  std::map<Datastore, std::string> locks_;
  std::optional<config::ConfigDocument> rollback_running_;
  std::optional<Clock::time_point> confirmation_deadline_;
  std::optional<std::string> confirming_session_;
  std::optional<std::string> persist_token_;
  RunningConfigBackend* backend_ = nullptr;
};

}  // namespace yang::netconf

#endif  // YANG_NETCONF_DATASTORE_H_
