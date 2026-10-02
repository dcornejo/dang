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
enum class Datastore {
  kRunning,
  kCandidate,
  kStartup,
  kIntended,
  /** Read-only selector whose state is supplied outside ConfigDocument. */
  kOperational,
};
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

/**
 * Host-authenticated context for a running-configuration transaction.
 *
 * `externally_coordinated` means another dangd instance already owns the
 * distributed transaction. Backends must still validate and apply the local
 * change, but must not discover or start another distributed transaction.
 * This assertion is transport policy and must never be accepted from RPC XML.
 */
struct BackendTransactionContext {
  bool externally_coordinated = false;
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
  /** Trusted host context forwarded only when the running tree is evaluated. */
  BackendTransactionContext backend_context;
};

/** Parameters for a confirmed commit. */
struct ConfirmedCommitOptions {
  std::chrono::seconds timeout = std::chrono::seconds(600);
  std::optional<std::string> persist;
};

/**
 * Opaque recovery identity supplied by a running-configuration backend.
 *
 * The datastore persists this value without interpreting it.  The host may
 * use it to correlate the published configuration with an external journal
 * after a process or machine failure.  Plugins never read or write this
 * metadata directly.
 */
struct BackendRecoveryState {
  std::string kind;
  std::string transaction_id;
  std::string proposal_digest;
  bool operator==(const BackendRecoveryState&) const = default;
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
  /** Context required if a pending confirmed commit must be rolled back. */
  bool rollback_externally_coordinated = false;
  /** Backend-owned correlation record for an unfinished durable transition. */
  std::optional<BackendRecoveryState> backend_recovery;
  bool operator==(const PersistentDatastoreState&) const = default;
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
      std::span<const config::ChangeEvent>,
      BackendTransactionContext = {}) {
    return std::nullopt;
  }
  [[nodiscard]] virtual std::optional<config::ValidationFinding> Replace(
      const config::RuntimeSchema& schema,
      const config::ConfigDocument& before,
      const config::ConfigDocument& after,
      std::span<const config::ChangeEvent> changes,
      BackendTransactionContext context = {}) = 0;
  /**
   * Returns recovery metadata created by the most recent successful
   * PrepareReplacement(). A non-null value is validated before Replace() and
   * persisted with the new running tree before CommitPreparedReplacement().
   */
  [[nodiscard]] virtual std::optional<BackendRecoveryState>
  PreparedReplacementRecoveryState() const {
    return std::nullopt;
  }
  /**
   * Finalizes a successful replacement after its datastore state is durable.
   * A backend may retain reversible external work until this notification.
   * An error means the replacement is durable but still requires recovery.
   */
  [[nodiscard]] virtual std::optional<config::ValidationFinding>
  CommitPreparedReplacement() {
    return std::nullopt;
  }
  /**
   * Cancels retained preparation or reversible replacement work.
   * After a successful Replace(), this is followed by a compensating Replace()
   * when datastore persistence fails.
   */
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
  /**
   * Controls whether snapshot restore updates the configured backend. A host
   * using kDefer must activate the restored running tree before exposing it.
   */
  enum class RestoreBackend { kApply, kDefer };
  using Clock = std::chrono::steady_clock;
  /**
   * Publishes one persistent-state transition. A failure must leave durable
   * storage containing `before`; the manager then restores its live state.
   */
  using PersistentStateCommitter = std::function<
      std::optional<config::ValidationFinding>(
          const PersistentDatastoreState& before,
          const PersistentDatastoreState& after)>;

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
  [[nodiscard]] TransactionResult Validate(
      Datastore datastore, BackendTransactionContext context = {});
  [[nodiscard]] TransactionResult Commit(
      std::string_view session,
      std::optional<ConfirmedCommitOptions> confirmed = std::nullopt,
      std::function<bool(const config::ChangeEvent&)> authorize_change = {},
      BackendTransactionContext context = {});
  [[nodiscard]] TransactionResult ConfirmCommit(
      std::string_view session,
      std::optional<std::string_view> persist_id = std::nullopt);
  /** Applies a follow-up confirmed commit in an existing persistent sequence. */
  [[nodiscard]] TransactionResult ContinueConfirmedCommit(
      std::string_view session, std::string_view persist_id,
      std::chrono::seconds timeout = std::chrono::seconds(600),
      BackendTransactionContext context = {});
  [[nodiscard]] TransactionResult CancelCommit(
      std::string_view session,
      std::optional<std::string_view> persist_id = std::nullopt,
      BackendTransactionContext context = {});
  [[nodiscard]] TransactionResult DiscardChanges(std::string_view session);
  [[nodiscard]] TransactionResult CopyConfig(std::string_view session,
                                             Datastore source,
                                             Datastore target,
      std::function<bool(const config::ChangeEvent&)> authorize_change = {},
      BackendTransactionContext context = {});
  /** Replaces a datastore with one complete, already parsed configuration. */
  [[nodiscard]] TransactionResult CopyConfig(
      std::string_view session, const config::ConfigDocument& source,
      Datastore target,
      std::function<bool(const config::ChangeEvent&)> authorize_change = {},
      BackendTransactionContext context = {});
  [[nodiscard]] TransactionResult DeleteConfig(std::string_view session,
                                               Datastore target);
  /** Releases locks and cancels that session's non-persistent confirmed commit. */
  void CloseSession(std::string_view session);
  /** Rolls back an expired confirmed commit; returns true when one expired. */
  bool ProcessTimeouts(Clock::time_point now = Clock::now());
  [[nodiscard]] PersistentDatastoreState ExportPersistentState() const;
  /** Atomically replaces unlocked state after parsing and validation. */
  [[nodiscard]] TransactionResult RestorePersistentState(
      const PersistentDatastoreState& state,
      RestoreBackend backend = RestoreBackend::kApply);
  /** Installs the durability participant used by subsequent mutations. */
  void SetPersistentStateCommitter(PersistentStateCommitter committer);

 private:
  struct StateSnapshot {
    config::ConfigDocument running;
    config::ConfigDocument candidate;
    config::ConfigDocument startup;
    std::optional<config::ConfigDocument> rollback_running;
    std::optional<Clock::time_point> confirmation_deadline;
    std::optional<std::string> confirming_session;
    std::optional<std::string> persist_token;
    BackendTransactionContext rollback_context;
    std::optional<BackendRecoveryState> backend_recovery;
  };
  [[nodiscard]] config::ConfigDocument& Mutable(Datastore datastore);
  [[nodiscard]] const config::ConfigDocument& Get(Datastore datastore) const;
  [[nodiscard]] TransactionResult CheckWriteAccess(
      Datastore datastore, std::string_view session) const;
  [[nodiscard]] TransactionResult ValidateDocument(
      const config::ConfigDocument& document) const;
  [[nodiscard]] TransactionResult ReplaceRunning(
      config::ConfigDocument replacement,
      std::vector<config::ChangeEvent> changes,
      BackendTransactionContext context = {});
  [[nodiscard]] StateSnapshot SnapshotLocked() const;
  [[nodiscard]] PersistentDatastoreState PersistentStateLocked() const;
  [[nodiscard]] PersistentDatastoreState PersistentStateOf(
      const StateSnapshot& snapshot) const;
  [[nodiscard]] TransactionResult FinishMutation(
      const StateSnapshot& before, TransactionResult result,
      BackendTransactionContext context = {});
  [[nodiscard]] std::optional<config::ValidationFinding> RestoreLocked(
      const StateSnapshot& snapshot, BackendTransactionContext context = {});
  [[nodiscard]] std::optional<config::ValidationFinding>
  CommitBackendReplacement();

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
  BackendTransactionContext rollback_context_;
  std::optional<BackendRecoveryState> backend_recovery_;
  bool backend_replacement_pending_ = false;
  RunningConfigBackend* backend_ = nullptr;
  PersistentStateCommitter persistent_state_committer_;
};

}  // namespace yang::netconf

#endif  // YANG_NETCONF_DATASTORE_H_
