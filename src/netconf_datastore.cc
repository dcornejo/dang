// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_datastore.h"

#include <algorithm>
#include <utility>

namespace yang::netconf {
namespace {

TransactionResult Failure(config::ValidationCode code, std::string message,
                          std::string tag, std::string path = {}) {
  config::ValidationFinding finding;
  finding.code = code;
  finding.state = config::FindingState::kInvalid;
  finding.message = std::move(message);
  finding.netconf_error_tag = std::move(tag);
  finding.instance_path = std::move(path);
  return {false, {std::move(finding)}, {}};
}

}  // namespace

DatastoreManager::DatastoreManager(
    const config::RuntimeSchema& schema, config::ConfigDocument running,
    std::optional<config::ConfigDocument> startup,
    RunningConfigBackend* backend)
    : schema_(schema), running_(std::move(running)), candidate_(running_),
      startup_(startup.value_or(running_)), backend_(backend) {}

TransactionResult DatastoreManager::ReplaceRunning(
    config::ConfigDocument replacement,
    std::vector<config::ChangeEvent> changes,
    BackendTransactionContext context) {
  if (backend_ != nullptr) {
    if (auto error = backend_->PrepareReplacement(
            schema_, running_, replacement, changes, context)) {
      backend_->AbortPreparedReplacement();
      return {false, {std::move(*error)}, {}};
    }
    if (auto error =
            backend_->Replace(schema_, running_, replacement, changes,
                              context)) {
      backend_->AbortPreparedReplacement();
      return {false, {std::move(*error)}, {}};
    }
    backend_replacement_pending_ = true;
  }
  running_ = std::move(replacement);
  return {true, {}, std::move(changes)};
}

DatastoreManager::StateSnapshot DatastoreManager::SnapshotLocked() const {
  return {running_, candidate_, startup_, rollback_running_,
          confirmation_deadline_, confirming_session_, persist_token_,
          rollback_context_};
}

PersistentDatastoreState DatastoreManager::PersistentStateLocked() const {
  return PersistentStateOf(SnapshotLocked());
}

PersistentDatastoreState DatastoreManager::PersistentStateOf(
    const StateSnapshot& snapshot) const {
  PersistentDatastoreState state;
  state.running_xml = snapshot.running.ToXml();
  state.candidate_xml = snapshot.candidate.ToXml();
  state.startup_xml = snapshot.startup.ToXml();
  if (snapshot.rollback_running)
    state.rollback_running_xml = snapshot.rollback_running->ToXml();
  if (snapshot.confirmation_deadline) {
    const auto remaining = *snapshot.confirmation_deadline - Clock::now();
    const auto expiry = std::chrono::system_clock::now() + remaining;
    state.confirmation_expiry_unix_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            expiry.time_since_epoch()).count();
  }
  state.confirming_session = snapshot.confirming_session;
  state.persist_token = snapshot.persist_token;
  state.rollback_externally_coordinated =
      snapshot.rollback_context.externally_coordinated;
  return state;
}

std::optional<config::ValidationFinding> DatastoreManager::RestoreLocked(
    const StateSnapshot& snapshot, BackendTransactionContext context) {
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, running_, snapshot.running);
  if (!changes.empty()) {
    TransactionResult restored =
        ReplaceRunning(snapshot.running, std::move(changes), context);
    if (!restored.ok) {
      return restored.errors.empty()
          ? std::optional<config::ValidationFinding>(
                Failure(config::ValidationCode::kInvalidValue,
                        "live configuration rollback failed", "operation-failed")
                    .errors.front())
          : std::optional<config::ValidationFinding>(restored.errors.front());
    }
  } else {
    running_ = snapshot.running;
  }
  candidate_ = snapshot.candidate;
  startup_ = snapshot.startup;
  rollback_running_ = snapshot.rollback_running;
  confirmation_deadline_ = snapshot.confirmation_deadline;
  confirming_session_ = snapshot.confirming_session;
  persist_token_ = snapshot.persist_token;
  rollback_context_ = snapshot.rollback_context;
  return std::nullopt;
}

TransactionResult DatastoreManager::FinishMutation(
    const StateSnapshot& before, TransactionResult result,
    BackendTransactionContext context) {
  const PersistentDatastoreState after = PersistentStateLocked();
  const PersistentDatastoreState prior = PersistentStateOf(before);
  if (persistent_state_committer_ && prior != after) {
    if (auto persistence_error = persistent_state_committer_(prior, after)) {
      if (backend_replacement_pending_ && backend_ != nullptr) {
        backend_->AbortPreparedReplacement();
        backend_replacement_pending_ = false;
      }
      if (auto rollback_error = RestoreLocked(before, context)) {
        persistence_error->message += "; live rollback also failed: " +
                                      rollback_error->message;
      }
      if (auto finalize_error = CommitBackendReplacement()) {
        persistence_error->message += "; rollback finalization also failed: " +
                                      finalize_error->message;
      }
      return {false, {std::move(*persistence_error)}, {}};
    }
  }
  if (auto finalize_error = CommitBackendReplacement()) {
    result.ok = false;
    result.errors.push_back(std::move(*finalize_error));
    result.changes.clear();
  }
  return result;
}

std::optional<config::ValidationFinding>
DatastoreManager::CommitBackendReplacement() {
  if (!backend_replacement_pending_ || backend_ == nullptr)
    return std::nullopt;
  backend_replacement_pending_ = false;
  return backend_->CommitPreparedReplacement();
}

void DatastoreManager::SetPersistentStateCommitter(
    PersistentStateCommitter committer) {
  std::lock_guard lock(mutex_);
  persistent_state_committer_ = std::move(committer);
}

config::ConfigDocument DatastoreManager::Read(Datastore datastore) const {
  std::lock_guard lock(mutex_);
  return Get(datastore);
}

TransactionResult DatastoreManager::Lock(Datastore datastore,
                                         std::string_view session) {
  std::lock_guard lock(mutex_);
  if (datastore == Datastore::kIntended ||
      datastore == Datastore::kOperational)
    return Failure(config::ValidationCode::kInvalidValue,
                   "the selected datastore is read-only", "invalid-value");
  if (session.empty())
    return Failure(config::ValidationCode::kInvalidValue,
                   "a non-empty NETCONF session identifier is required",
                   "invalid-value");
  const auto found = locks_.find(datastore);
  if (found != locks_.end() && found->second != session)
    return Failure(config::ValidationCode::kInvalidValue,
                   "datastore is locked by another session", "lock-denied");
  locks_[datastore] = std::string(session);
  return {true, {}, {}};
}

TransactionResult DatastoreManager::Unlock(Datastore datastore,
                                           std::string_view session) {
  std::lock_guard lock(mutex_);
  const auto found = locks_.find(datastore);
  if (found == locks_.end() || found->second != session)
    return Failure(config::ValidationCode::kInvalidValue,
                   "session does not own the datastore lock", "lock-denied");
  locks_.erase(found);
  return {true, {}, {}};
}

TransactionResult DatastoreManager::CheckWriteAccess(
    Datastore datastore, std::string_view session) const {
  if (datastore == Datastore::kIntended ||
      datastore == Datastore::kOperational)
    return Failure(config::ValidationCode::kInvalidValue,
                   "the intended datastore is read-only",
                   "operation-not-supported");
  const auto found = locks_.find(datastore);
  if (found != locks_.end() && found->second != session)
    return Failure(config::ValidationCode::kInvalidValue,
                   "datastore is locked by another session", "lock-denied");
  return {true, {}, {}};
}

TransactionResult DatastoreManager::ValidateDocument(
    const config::ConfigDocument& document) const {
  config::ConfigValidator validator;
  config::ValidationResult checked = validator.Validate({schema_, document});
  return {checked.valid && checked.complete, std::move(checked.findings), {}};
}

TransactionResult DatastoreManager::Validate(
    Datastore datastore, BackendTransactionContext context) {
  std::lock_guard lock(mutex_);
  TransactionResult result = ValidateDocument(Get(datastore));
  if (!result.ok || backend_ == nullptr) return result;
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, running_, Get(datastore));
  if (auto error = backend_->PrepareReplacement(
          schema_, running_, Get(datastore), changes, context)) {
    result.ok = false;
    result.errors.push_back(std::move(*error));
  }
  backend_->AbortPreparedReplacement();
  return result;
}

TransactionResult DatastoreManager::EditConfig(
    const EditConfigRequest& request) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (TransactionResult access = CheckWriteAccess(request.target, request.session);
      !access.ok) return access;
  const config::ConfigDocument original = Get(request.target);
  config::ConfigDocument working = original;
  TransactionResult result{true, {}, {}};
  config::ConfigEditor editor;
  for (const config::EditDocument& edit : request.edits) {
    const bool validate_each = request.test_option != TestOption::kSet;
    config::EditResult applied = editor.Apply(
        {schema_, working, edit, request.default_operation, nullptr,
         validate_each});
    if (!applied.candidate) {
      result.ok = false;
      result.errors.insert(result.errors.end(), applied.errors.begin(),
                           applied.errors.end());
      if (request.error_option == ErrorOption::kRollbackOnError) {
        result.changes.clear();
        working = original;
        break;
      }
      if (request.error_option == ErrorOption::kStopOnError) break;
      continue;
    }
    std::vector<config::ChangeEvent> exact_changes =
        config::DiffConfigDocuments(schema_, working, *applied.candidate);
    if (request.authorize_change) {
      const auto requires_authorization = [&](const config::ChangeEvent& change) {
        if (change.kind != config::ChangeKind::kDeleted) return true;
        return std::ranges::none_of(
            applied.implicit_changes,
            [&](const config::ChangeEvent& implicit) {
              return change.instance_path == implicit.instance_path ||
                  (change.instance_path.starts_with(implicit.instance_path) &&
                   change.instance_path.size() > implicit.instance_path.size() &&
                   change.instance_path[implicit.instance_path.size()] == '/');
            });
      };
      const auto denied = std::ranges::find_if(
          exact_changes, [&](const config::ChangeEvent& change) {
            return requires_authorization(change) &&
                   !request.authorize_change(change);
          });
      if (denied != exact_changes.end()) {
        result.ok = false;
        config::ValidationFinding finding;
        finding.code = config::ValidationCode::kInvalidValue;
        finding.state = config::FindingState::kInvalid;
        finding.message = "access to the proposed datastore change is denied";
        finding.instance_path = denied->instance_path;
        finding.netconf_error_tag = "access-denied";
        result.errors.push_back(std::move(finding));
        if (request.error_option == ErrorOption::kRollbackOnError) {
          result.changes.clear();
          working = original;
          break;
        }
        if (request.error_option == ErrorOption::kStopOnError) break;
        continue;
      }
    }
    working = std::move(*applied.candidate);
    result.changes.insert(result.changes.end(), exact_changes.begin(),
                          exact_changes.end());
  }
  if (request.test_option == TestOption::kTestThenSet && result.ok) {
    TransactionResult checked = ValidateDocument(working);
    if (!checked.ok) {
      result.ok = false;
      result.errors = std::move(checked.errors);
      result.changes.clear();
      working = original;
    }
  }
  if (request.test_option == TestOption::kTestOnly && result.ok && backend_) {
    std::vector<config::ChangeEvent> proposed_changes =
        config::DiffConfigDocuments(schema_, original, working);
    if (auto error = backend_->PrepareReplacement(
            schema_, original, working, proposed_changes,
            request.backend_context)) {
      result.ok = false;
      result.errors.push_back(std::move(*error));
    }
    backend_->AbortPreparedReplacement();
  }
  if (request.test_option != TestOption::kTestOnly &&
      (result.ok || request.error_option != ErrorOption::kRollbackOnError)) {
    if (request.target == Datastore::kRunning) {
      std::vector<config::ChangeEvent> final_changes =
          config::DiffConfigDocuments(schema_, original, working);
      TransactionResult replaced =
          ReplaceRunning(std::move(working), std::move(final_changes),
                         request.backend_context);
      if (!replaced.ok) return replaced;
    } else {
      Mutable(request.target) = std::move(working);
    }
  }
  std::ranges::sort(result.changes, {}, &config::ChangeEvent::instance_path);
  if (request.test_option == TestOption::kTestOnly) return result;
  return FinishMutation(before, std::move(result), request.backend_context);
}

TransactionResult DatastoreManager::Commit(
    std::string_view session,
    std::optional<ConfirmedCommitOptions> confirmed,
    std::function<bool(const config::ChangeEvent&)> authorize_change,
    BackendTransactionContext context) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (TransactionResult access = CheckWriteAccess(Datastore::kCandidate, session);
      !access.ok) return access;
  if (TransactionResult access = CheckWriteAccess(Datastore::kRunning, session);
      !access.ok) return access;
  TransactionResult checked = ValidateDocument(candidate_);
  if (!checked.ok) return checked;
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, running_, candidate_);
  if (authorize_change) {
    const auto denied = std::ranges::find_if(
        changes, [&](const config::ChangeEvent& change) {
          return !authorize_change(change);
        });
    if (denied != changes.end()) {
      return Failure(config::ValidationCode::kInvalidValue,
                     "access to the proposed commit change is denied",
                     "access-denied", denied->instance_path);
    }
  }
  if (confirmed && confirmed->timeout <= std::chrono::seconds::zero())
    return Failure(config::ValidationCode::kInvalidValue,
                   "confirmed-commit timeout must be positive",
                   "invalid-value");
  std::optional<config::ConfigDocument> rollback_base = rollback_running_;
  if (confirmed && !rollback_base) rollback_base = running_;
  TransactionResult replaced =
      ReplaceRunning(candidate_, std::move(changes), context);
  if (!replaced.ok) return replaced;
  if (confirmed) {
    rollback_running_ = std::move(rollback_base);
    confirmation_deadline_ = Clock::now() + confirmed->timeout;
    confirming_session_ = std::string(session);
    persist_token_ = confirmed->persist;
    rollback_context_ = context;
  } else {
    rollback_running_.reset();
    confirmation_deadline_.reset();
    confirming_session_.reset();
    persist_token_.reset();
    rollback_context_ = {};
  }
  return FinishMutation(before, std::move(replaced), context);
}

TransactionResult DatastoreManager::ConfirmCommit(
    std::string_view session, std::optional<std::string_view> persist_id) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (!rollback_running_)
    return Failure(config::ValidationCode::kInvalidValue,
                   "there is no pending confirmed commit", "operation-failed");
  const bool authorized = persist_token_
      ? persist_id && *persist_id == *persist_token_
      : confirming_session_ && *confirming_session_ == session && !persist_id;
  if (!authorized)
    return Failure(config::ValidationCode::kInvalidValue,
                   "confirmed-commit token or session does not match",
                   "invalid-value");
  rollback_running_.reset();
  confirmation_deadline_.reset();
  confirming_session_.reset();
  persist_token_.reset();
  rollback_context_ = {};
  candidate_ = running_;
  return FinishMutation(before, {true, {}, {}});
}

TransactionResult DatastoreManager::ContinueConfirmedCommit(
    std::string_view session, std::string_view persist_id,
    std::chrono::seconds timeout, BackendTransactionContext context) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (!rollback_running_ || !persist_token_ || *persist_token_ != persist_id)
    return Failure(config::ValidationCode::kInvalidValue,
                   "confirmed-commit persist-id does not match",
                   "invalid-value");
  if (timeout <= std::chrono::seconds::zero())
    return Failure(config::ValidationCode::kInvalidValue,
                   "confirmed-commit timeout must be positive",
                   "invalid-value");
  if (TransactionResult access = CheckWriteAccess(Datastore::kCandidate, session);
      !access.ok) return access;
  if (TransactionResult access = CheckWriteAccess(Datastore::kRunning, session);
      !access.ok) return access;
  TransactionResult checked = ValidateDocument(candidate_);
  if (!checked.ok) return checked;
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, running_, candidate_);
  if (context.externally_coordinated !=
      rollback_context_.externally_coordinated) {
    return Failure(config::ValidationCode::kInvalidValue,
                   "confirmed-commit coordination context does not match",
                   "access-denied");
  }
  TransactionResult replaced =
      ReplaceRunning(candidate_, changes, rollback_context_);
  if (!replaced.ok) return replaced;
  confirmation_deadline_ = Clock::now() + timeout;
  confirming_session_ = std::string(session);
  return FinishMutation(before, {true, {}, std::move(changes)},
                        rollback_context_);
}

TransactionResult DatastoreManager::CancelCommit(
    std::string_view session, std::optional<std::string_view> persist_id,
    BackendTransactionContext context) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (!rollback_running_)
    return Failure(config::ValidationCode::kInvalidValue,
                   "there is no pending confirmed commit", "operation-failed");
  const bool authorized = persist_token_
      ? persist_id && *persist_id == *persist_token_
      : confirming_session_ && *confirming_session_ == session && !persist_id;
  if (!authorized)
    return Failure(config::ValidationCode::kInvalidValue,
                   "confirmed-commit token or session does not match",
                   "invalid-value");
  if (context.externally_coordinated !=
      rollback_context_.externally_coordinated) {
    return Failure(config::ValidationCode::kInvalidValue,
                   "confirmed-commit coordination context does not match",
                   "access-denied");
  }
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, running_, *rollback_running_);
  const BackendTransactionContext rollback_context = rollback_context_;
  TransactionResult replaced =
      ReplaceRunning(*rollback_running_, changes, rollback_context);
  if (!replaced.ok) return replaced;
  candidate_ = running_;
  rollback_running_.reset();
  confirmation_deadline_.reset();
  confirming_session_.reset();
  persist_token_.reset();
  rollback_context_ = {};
  return FinishMutation(before, {true, {}, std::move(changes)},
                        rollback_context);
}

TransactionResult DatastoreManager::DiscardChanges(std::string_view session) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (TransactionResult access = CheckWriteAccess(Datastore::kCandidate, session);
      !access.ok) return access;
  candidate_ = running_;
  return FinishMutation(before, {true, {}, {}});
}

TransactionResult DatastoreManager::CopyConfig(std::string_view session,
                                               Datastore source,
                                               Datastore target,
    std::function<bool(const config::ChangeEvent&)> authorize_change,
    BackendTransactionContext context) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (source == target)
    return Failure(config::ValidationCode::kInvalidValue,
                   "copy-config source and target must be different",
                   "invalid-value");
  if (TransactionResult access = CheckWriteAccess(target, session); !access.ok)
    return access;
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, Get(target), Get(source));
  if (authorize_change) {
    const auto denied = std::ranges::find_if(
        changes, [&](const config::ChangeEvent& change) {
          return !authorize_change(change);
        });
    if (denied != changes.end()) {
      return Failure(config::ValidationCode::kInvalidValue,
                     "access to the proposed copy-config change is denied",
                     "access-denied", denied->instance_path);
    }
  }
  if (target == Datastore::kRunning) {
    TransactionResult replaced = ReplaceRunning(Get(source), changes, context);
    if (!replaced.ok) return replaced;
  } else {
    Mutable(target) = Get(source);
  }
  return FinishMutation(before, {true, {}, std::move(changes)}, context);
}

TransactionResult DatastoreManager::CopyConfig(
    std::string_view session, const config::ConfigDocument& source,
    Datastore target,
    std::function<bool(const config::ChangeEvent&)> authorize_change,
    BackendTransactionContext context) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (TransactionResult access = CheckWriteAccess(target, session); !access.ok)
    return access;
  TransactionResult validated = ValidateDocument(source);
  if (!validated.ok) return validated;
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, Get(target), source);
  if (authorize_change) {
    const auto denied = std::ranges::find_if(
        changes, [&](const config::ChangeEvent& change) {
          return !authorize_change(change);
        });
    if (denied != changes.end()) {
      return Failure(config::ValidationCode::kInvalidValue,
                     "access to the proposed copy-config change is denied",
                     "access-denied", denied->instance_path);
    }
  }
  if (target == Datastore::kRunning) {
    TransactionResult replaced = ReplaceRunning(source, changes, context);
    if (!replaced.ok) return replaced;
  } else {
    Mutable(target) = source;
  }
  return FinishMutation(before, {true, {}, std::move(changes)}, context);
}

TransactionResult DatastoreManager::DeleteConfig(std::string_view session,
                                                 Datastore target) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (target != Datastore::kStartup)
    return Failure(config::ValidationCode::kInvalidValue,
                   "only the startup datastore can be deleted",
                   "operation-not-supported");
  if (TransactionResult access = CheckWriteAccess(target, session); !access.ok)
    return access;
  startup_ = config::ConfigDocument();
  return FinishMutation(before, {true, {}, {}});
}

void DatastoreManager::CloseSession(std::string_view session) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  for (auto iterator = locks_.begin(); iterator != locks_.end();) {
    if (iterator->second == session) {
      iterator = locks_.erase(iterator);
    } else {
      ++iterator;
    }
  }
  if (rollback_running_ && !persist_token_ && confirming_session_ == session) {
    std::vector<config::ChangeEvent> changes =
        config::DiffConfigDocuments(schema_, running_, *rollback_running_);
    const BackendTransactionContext rollback_context = rollback_context_;
    if (!ReplaceRunning(*rollback_running_, std::move(changes),
                        rollback_context).ok) return;
    candidate_ = running_;
    rollback_running_.reset();
    confirmation_deadline_.reset();
    confirming_session_.reset();
    rollback_context_ = {};
    (void)FinishMutation(before, {true, {}, {}}, rollback_context);
  }
}

bool DatastoreManager::ProcessTimeouts(Clock::time_point now) {
  std::lock_guard lock(mutex_);
  const StateSnapshot before = SnapshotLocked();
  if (!confirmation_deadline_ || now < *confirmation_deadline_) return false;
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, running_, *rollback_running_);
  const BackendTransactionContext rollback_context = rollback_context_;
  if (!ReplaceRunning(*rollback_running_, std::move(changes),
                      rollback_context).ok) return false;
  candidate_ = running_;
  rollback_running_.reset();
  confirmation_deadline_.reset();
  confirming_session_.reset();
  persist_token_.reset();
  rollback_context_ = {};
  return FinishMutation(before, {true, {}, {}}, rollback_context).ok;
}

PersistentDatastoreState DatastoreManager::ExportPersistentState() const {
  std::lock_guard lock(mutex_);
  return PersistentStateLocked();
}

TransactionResult DatastoreManager::RestorePersistentState(
    const PersistentDatastoreState& state, RestoreBackend backend) {
  const auto parse = [&](std::string_view xml)
      -> std::optional<config::ConfigDocument> {
    if (xml.empty()) return config::ConfigDocument();
    return config::ParseDatastoreXml(schema_, xml).document;
  };
  auto running = parse(state.running_xml);
  auto candidate = parse(state.candidate_xml);
  auto startup = parse(state.startup_xml);
  auto rollback = state.rollback_running_xml
      ? parse(*state.rollback_running_xml)
      : std::optional<config::ConfigDocument>();
  if (!running || !candidate || !startup ||
      (state.rollback_running_xml && !rollback)) {
    return Failure(config::ValidationCode::kMalformedXml,
                   "persistent datastore snapshot cannot be parsed",
                   "operation-failed");
  }
  if (!ValidateDocument(*running).ok ||
      (!startup->roots().empty() && !ValidateDocument(*startup).ok) ||
      (rollback && !ValidateDocument(*rollback).ok)) {
    return Failure(config::ValidationCode::kInvalidValue,
                   "persistent datastore snapshot fails schema validation",
                   "operation-failed");
  }
  const bool pending = rollback.has_value();
  if (pending != state.confirmation_expiry_unix_seconds.has_value()) {
    return Failure(config::ValidationCode::kInvalidValue,
                   "persistent confirmed-commit state is incomplete",
                   "operation-failed");
  }
  if (!pending && state.rollback_externally_coordinated) {
    return Failure(config::ValidationCode::kInvalidValue,
                   "persistent rollback context has no confirmed commit",
                   "operation-failed");
  }
  std::lock_guard lock(mutex_);
  locks_.clear();
  config::ConfigDocument restored_running = std::move(*running);
  candidate_ = std::move(*candidate);
  startup_ = std::move(*startup);
  rollback_running_ = std::move(rollback);
  confirming_session_ = state.confirming_session;
  persist_token_ = state.persist_token;
  rollback_context_.externally_coordinated =
      state.rollback_externally_coordinated;
  const BackendTransactionContext activation_context = rollback_context_;
  confirmation_deadline_.reset();
  if (state.confirmation_expiry_unix_seconds) {
    const auto expiry = std::chrono::system_clock::time_point(
        std::chrono::seconds(*state.confirmation_expiry_unix_seconds));
    const auto remaining = expiry - std::chrono::system_clock::now();
    if (remaining <= std::chrono::system_clock::duration::zero()) {
      restored_running = *rollback_running_;
      candidate_ = restored_running;
      rollback_running_.reset();
      confirming_session_.reset();
      persist_token_.reset();
      rollback_context_ = {};
    } else {
      confirmation_deadline_ = Clock::now() +
          std::chrono::duration_cast<Clock::duration>(remaining);
    }
  }
  std::vector<config::ChangeEvent> changes =
      config::DiffConfigDocuments(schema_, running_, restored_running);
  if (backend == RestoreBackend::kApply) {
    TransactionResult replaced =
        ReplaceRunning(std::move(restored_running), std::move(changes),
                       activation_context);
    if (!replaced.ok) return replaced;
    if (auto error = CommitBackendReplacement())
      return {false, {std::move(*error)}, {}};
  } else {
    running_ = std::move(restored_running);
  }
  return {true, {}, {}};
}

config::ConfigDocument& DatastoreManager::Mutable(Datastore datastore) {
  if (datastore == Datastore::kRunning) return running_;
  if (datastore == Datastore::kCandidate) return candidate_;
  // kIntended is read-only and is rejected before reaching this helper.
  return startup_;
}

const config::ConfigDocument& DatastoreManager::Get(Datastore datastore) const {
  if (datastore == Datastore::kRunning) return running_;
  if (datastore == Datastore::kCandidate) return candidate_;
  if (datastore == Datastore::kIntended ||
      datastore == Datastore::kOperational) return running_;
  return startup_;
}

}  // namespace yang::netconf
