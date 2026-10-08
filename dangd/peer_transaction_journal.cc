// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_journal.h"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dangd {
namespace {

constexpr int kJournalVersion = 2;
constexpr std::size_t kMaximumJournalBytes = 1024 * 1024;

std::optional<std::string> ValidateState(const PeerJournalState &state) {
  if (state.decision != PeerJournalDecision::kPrepared &&
      state.decision != PeerJournalDecision::kCommit)
    return "peer journal contains an invalid decision";
  if (state.transaction_id.empty() || state.transaction_id.size() > 256)
    return "peer journal transaction identity is empty or too long";
  if (state.proposal_digest.empty() || state.proposal_digest.size() > 256)
    return "peer journal proposal digest is empty or too long";
  if (state.participants.empty() || state.participants.size() > 256)
    return "peer journal requires between one and 256 remote participants";
  std::set<std::string> ids;
  std::size_t primary_count = 0;
  for (const PeerJournalParticipant &participant : state.participants) {
    if (participant.id.empty() || participant.id.size() > 256 ||
        !ids.insert(participant.id).second)
      return "peer journal participant identities must be unique and bounded";
    if (participant.persistent_commit_id.empty() ||
        participant.persistent_commit_id.size() > 256)
      return "peer journal persistent commit identities must be bounded";
    if (participant.role != PeerTransactionRole::kPrimary &&
        participant.role != PeerTransactionRole::kStandby)
      return "peer journal contains an invalid participant role";
    if (participant.role == PeerTransactionRole::kPrimary)
      ++primary_count;
  }
  // The group's primary may be the local, unjournaled participant. The file
  // records only remote confirmed commits, so zero remote primaries is valid.
  if (primary_count > 1)
    return "peer journal has more than one primary participant";
  if (state.decision == PeerJournalDecision::kPrepared &&
      std::ranges::any_of(state.participants,
                          [](const PeerJournalParticipant &participant) {
                            return participant.confirmed;
                          }))
    return "prepared peer journal cannot contain confirmations";
  return std::nullopt;
}

nlohmann::json ToJson(const PeerJournalState &state) {
  nlohmann::json participants = nlohmann::json::array();
  for (const PeerJournalParticipant &participant : state.participants) {
    participants.push_back(
        {{"id", participant.id},
         {"role", participant.role == PeerTransactionRole::kPrimary
                      ? "primary"
                      : "standby"},
         {"persistent-commit-id", participant.persistent_commit_id},
         {"confirmed", participant.confirmed}});
  }
  return {{"version", kJournalVersion},
          {"decision", state.decision == PeerJournalDecision::kCommit
                           ? "commit"
                           : "prepared"},
          {"transaction-id", state.transaction_id},
          {"proposal-digest", state.proposal_digest},
          {"participants", std::move(participants)}};
}

std::optional<PeerJournalState> FromJson(const nlohmann::json &json) {
  try {
    const int version = json.value("version", 0);
    const std::string decision = json.value("decision", "");
    if (!json.is_object() || (version != 1 && version != kJournalVersion) ||
        (decision != "prepared" && decision != "commit") ||
        (version == 1 && decision != "commit") ||
        !json.at("participants").is_array())
      return std::nullopt;
    PeerJournalState state;
    state.transaction_id = json.at("transaction-id").get<std::string>();
    state.proposal_digest = json.at("proposal-digest").get<std::string>();
    state.decision = decision == "commit" ? PeerJournalDecision::kCommit
                                           : PeerJournalDecision::kPrepared;
    for (const nlohmann::json &value : json.at("participants")) {
      const std::string role = value.at("role").get<std::string>();
      if (role != "primary" && role != "standby")
        return std::nullopt;
      state.participants.push_back(
          {.id = value.at("id").get<std::string>(),
           .role = role == "primary" ? PeerTransactionRole::kPrimary
                                     : PeerTransactionRole::kStandby,
           .persistent_commit_id =
               value.at("persistent-commit-id").get<std::string>(),
           .confirmed = value.at("confirmed").get<bool>()});
    }
    if (ValidateState(state))
      return std::nullopt;
    return state;
  } catch (const nlohmann::json::exception &) {
    return std::nullopt;
  }
}

bool SyncPath(const std::filesystem::path &path, bool directory = false) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor =
      open(path.c_str(), directory ? O_RDONLY : (O_RDONLY | O_CLOEXEC));
  if (descriptor < 0)
    return false;
  const bool ok = fsync(descriptor) == 0;
  close(descriptor);
  return ok;
#else
  (void)path;
  (void)directory;
  return true;
#endif
}

bool WritePrivateFile(const std::filesystem::path &path,
                      std::string_view contents) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor =
      open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
           S_IRUSR | S_IWUSR);
  if (descriptor < 0)
    return false;
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      close(descriptor);
      unlink(path.c_str());
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  if (close(descriptor) != 0) {
    unlink(path.c_str());
    return false;
  }
  return true;
#else
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
  return static_cast<bool>(output);
#endif
}

std::optional<std::string> ReadPrivateFile(const std::filesystem::path &path,
                                           std::string *contents) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    return "cannot open peer transaction journal";
  struct stat status{};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      status.st_uid != geteuid()) {
    close(descriptor);
    return "peer transaction journal is not a private owned regular file";
  }
  char buffer[4096];
  while (true) {
    const ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      close(descriptor);
      return "cannot read peer transaction journal";
    }
    if (count == 0)
      break;
    if (contents->size() + static_cast<std::size_t>(count) >
        kMaximumJournalBytes) {
      close(descriptor);
      return "peer transaction journal exceeds the byte limit";
    }
    contents->append(buffer, static_cast<std::size_t>(count));
  }
  if (close(descriptor) != 0)
    return "cannot close peer transaction journal";
#else
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return "cannot open peer transaction journal";
  contents->assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
  if (contents->size() > kMaximumJournalBytes)
    return "peer transaction journal exceeds the byte limit";
#endif
  return std::nullopt;
}

} // namespace

PeerTransactionFileJournal::PeerTransactionFileJournal(
    std::filesystem::path path, PeerJournalState state,
    PeerJournalSaveCheckpoint checkpoint)
    : path_(std::move(path)), state_(std::move(state)),
      checkpoint_(std::move(checkpoint)) {}

std::unique_ptr<PeerTransactionFileJournal>
PeerTransactionFileJournal::Create(const std::filesystem::path &path,
                                   PeerJournalState state, std::string *error,
                                   PeerJournalSaveCheckpoint checkpoint) {
  if (path.empty()) {
    *error = "peer transaction journal path is empty";
    return nullptr;
  }
  state.decision = PeerJournalDecision::kPrepared;
  std::ranges::sort(state.participants, {}, &PeerJournalParticipant::id);
  if (const auto invalid = ValidateState(state)) {
    *error = *invalid;
    return nullptr;
  }
  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(path, status_error);
  if (status_error && status_error != std::errc::no_such_file_or_directory) {
    *error = "cannot inspect peer transaction journal path";
    return nullptr;
  }
  if (!status_error && std::filesystem::exists(status)) {
    *error = "an unresolved peer transaction journal already exists";
    return nullptr;
  }
  auto journal = std::unique_ptr<PeerTransactionFileJournal>(
      new PeerTransactionFileJournal(path, std::move(state),
                                     std::move(checkpoint)));
  if (const SaveResult saved = journal->Save(); saved.error) {
    *error = *saved.error;
    return nullptr;
  }
  return journal;
}

std::unique_ptr<PeerTransactionFileJournal>
PeerTransactionFileJournal::Load(const std::filesystem::path &path,
                                 std::string *error,
                                 PeerJournalSaveCheckpoint checkpoint) {
  std::string contents;
  if (const auto read_error = ReadPrivateFile(path, &contents)) {
    *error = *read_error;
    return nullptr;
  }
  try {
    const auto state = FromJson(nlohmann::json::parse(contents));
    if (!state) {
      *error = "peer transaction journal is malformed or unsupported";
      return nullptr;
    }
    auto journal = std::unique_ptr<PeerTransactionFileJournal>(
        new PeerTransactionFileJournal(path, *state, std::move(checkpoint)));
    journal->decision_recorded_ =
        state->decision == PeerJournalDecision::kCommit;
    return journal;
  } catch (const nlohmann::json::exception &) {
    *error = "peer transaction journal is not valid JSON";
    return nullptr;
  }
}

PeerTransactionJournal PeerTransactionFileJournal::Callbacks() {
  return {
      .record_abort = [this] { return RecordAbort(); },
      .record_commit_decision =
          [this](const std::vector<std::string> &ids) {
            return RecordDecision(ids);
          },
      .record_confirmation =
          [this](const std::string &id) { return RecordConfirmation(id); },
      .record_complete = [this] { return RecordComplete(); },
  };
}

PeerTransactionDecisionResult PeerTransactionFileJournal::RecordDecision(
    const std::vector<std::string> &participant_ids) {
  std::vector<std::string> expected;
  for (const PeerJournalParticipant &participant : state_.participants)
    expected.push_back(participant.id);
  if (participant_ids != expected)
    return {
        .status = PeerTransactionDecisionStatus::kNotCommitted,
        .error =
            "peer transaction journal participant set does not match decision"};
  if (decision_recorded_)
    return {.status = PeerTransactionDecisionStatus::kCommitted};
  state_.decision = PeerJournalDecision::kCommit;
  const SaveResult saved = Save();
  if (saved.error) {
    if (!saved.replacement_may_be_visible)
      state_.decision = PeerJournalDecision::kPrepared;
    return {.status = saved.replacement_may_be_visible
                          ? PeerTransactionDecisionStatus::kOutcomeUnknown
                          : PeerTransactionDecisionStatus::kNotCommitted,
            .error = *saved.error};
  }
  decision_recorded_ = true;
  return {.status = PeerTransactionDecisionStatus::kCommitted};
}

std::optional<std::string> PeerTransactionFileJournal::RecordAbort() {
  if (decision_recorded_ || state_.decision == PeerJournalDecision::kCommit)
    return "peer transaction already has a durable commit decision";
  std::error_code error;
  std::filesystem::remove(path_, error);
  if (error)
    return "cannot remove prepared peer transaction journal";
  const std::filesystem::path parent = path_.parent_path().empty()
                                           ? std::filesystem::path(".")
                                           : path_.parent_path();
  if (!SyncPath(parent, true))
    return "prepared peer journal was removed but its directory cannot be "
           "synchronized";
  return std::nullopt;
}

std::optional<std::string> PeerTransactionFileJournal::RecordConfirmation(
    const std::string &participant_id) {
  if (!decision_recorded_)
    return "peer transaction decision is not durable";
  auto participant = std::ranges::find(state_.participants, participant_id,
                                       &PeerJournalParticipant::id);
  if (participant == state_.participants.end())
    return "peer transaction confirmation names an unknown participant";
  if (participant->confirmed)
    return std::nullopt;
  participant->confirmed = true;
  if (const SaveResult saved = Save(); saved.error) {
    participant->confirmed = false;
    return saved.error;
  }
  return std::nullopt;
}

std::optional<std::string> PeerTransactionFileJournal::RecordComplete() {
  if (!decision_recorded_)
    return "peer transaction decision is not durable";
  if (std::ranges::any_of(state_.participants,
                          [](const PeerJournalParticipant &participant) {
                            return !participant.confirmed;
                          }))
    return "peer transaction confirmations are incomplete";
  std::error_code error;
  std::filesystem::remove(path_, error);
  if (error)
    return "cannot remove completed peer transaction journal";
  const std::filesystem::path parent = path_.parent_path().empty()
                                           ? std::filesystem::path(".")
                                           : path_.parent_path();
  if (!SyncPath(parent, true))
    return "peer transaction journal was removed but its directory cannot be "
           "synchronized";
  return std::nullopt;
}

PeerTransactionFileJournal::SaveResult PeerTransactionFileJournal::Save() {
  const std::string contents = ToJson(state_).dump(2) + '\n';
  std::filesystem::path temporary;
  bool created = false;
  for (unsigned attempt = 0; attempt < 16 && !created; ++attempt) {
    temporary = path_;
    temporary += ".tmp-" + std::to_string(std::random_device{}());
    created = WritePrivateFile(temporary, contents);
  }
  if (!created)
    return {.error = "cannot write private temporary peer transaction journal"};
  const auto discard = [&temporary] {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  };
  if (checkpoint_ && !checkpoint_(PeerJournalSaveStage::kTemporaryWritten)) {
    discard();
    return {.error = "peer journal save interrupted after temporary write"};
  }
  if (!SyncPath(temporary)) {
    discard();
    return {.error = "cannot synchronize temporary peer transaction journal"};
  }
  if (checkpoint_ &&
      !checkpoint_(PeerJournalSaveStage::kTemporarySynchronized)) {
    discard();
    return {
        .error =
            "peer journal save interrupted after temporary synchronization"};
  }
  std::error_code error;
  std::filesystem::rename(temporary, path_, error);
  if (error) {
    discard();
    return {.error = "cannot atomically replace peer transaction journal"};
  }
  if (checkpoint_ && !checkpoint_(PeerJournalSaveStage::kJournalReplaced))
    return {.error = "peer journal save interrupted after atomic replacement",
            .replacement_may_be_visible = true};
  const std::filesystem::path parent = path_.parent_path().empty()
                                           ? std::filesystem::path(".")
                                           : path_.parent_path();
  if (!SyncPath(parent, true))
    return {.error = "peer journal was replaced but its directory cannot be "
                     "synchronized",
            .replacement_may_be_visible = true};
  if (checkpoint_ && !checkpoint_(PeerJournalSaveStage::kDirectorySynchronized))
    return {.error =
                "peer journal save interrupted after directory synchronization",
            .replacement_may_be_visible = true};
  return {};
}

} // namespace dangd
