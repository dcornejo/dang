// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_persistence.h"

#include "yang/resource_limits.h"

#include <cerrno>
#include <fstream>
#include <iterator>
#include <random>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace yang::netconf {
namespace {

constexpr int kSnapshotVersion = 1;

template <typename Value>
nlohmann::json OptionalJson(const std::optional<Value>& value) {
  return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json ToJson(const PersistentDatastoreState& state) {
  return {{"version", kSnapshotVersion},
          {"running", state.running_xml},
          {"candidate", state.candidate_xml},
          {"startup", state.startup_xml},
          {"rollback-running", OptionalJson(state.rollback_running_xml)},
          {"confirmation-expiry-unix-seconds",
           OptionalJson(state.confirmation_expiry_unix_seconds)},
          {"confirming-session", OptionalJson(state.confirming_session)},
          {"persist-token", OptionalJson(state.persist_token)}};
}

std::optional<PersistentDatastoreState> FromJson(const nlohmann::json& json) {
  if (!json.is_object() || json.value("version", 0) != kSnapshotVersion ||
      !json.contains("running") || !json.contains("candidate") ||
      !json.contains("startup")) return std::nullopt;
  try {
    PersistentDatastoreState state;
    state.running_xml = json.at("running").get<std::string>();
    state.candidate_xml = json.at("candidate").get<std::string>();
    state.startup_xml = json.at("startup").get<std::string>();
    if (json.contains("rollback-running") && !json.at("rollback-running").is_null())
      state.rollback_running_xml = json.at("rollback-running").get<std::string>();
    if (json.contains("confirmation-expiry-unix-seconds") &&
        !json.at("confirmation-expiry-unix-seconds").is_null()) {
      state.confirmation_expiry_unix_seconds =
          json.at("confirmation-expiry-unix-seconds").get<std::int64_t>();
    }
    if (json.contains("confirming-session") &&
        !json.at("confirming-session").is_null())
      state.confirming_session = json.at("confirming-session").get<std::string>();
    if (json.contains("persist-token") && !json.at("persist-token").is_null())
      state.persist_token = json.at("persist-token").get<std::string>();
    return state;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

bool SyncPath(const std::filesystem::path& path, bool directory = false) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor =
      open(path.c_str(), directory ? O_RDONLY : (O_RDONLY | O_CLOEXEC));
  if (descriptor < 0) return false;
  const bool ok = fsync(descriptor) == 0;
  close(descriptor);
  return ok;
#else
  (void)path;
  (void)directory;
  return true;
#endif
}

bool WritePrivateFile(const std::filesystem::path& path,
                      std::string_view contents) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                               O_CLOEXEC | O_NOFOLLOW,
                              S_IRUSR | S_IWUSR);
  if (descriptor < 0) return false;
  const auto discard = [&path, descriptor]() {
    close(descriptor);
    unlink(path.c_str());
  };
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t written =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) {
      discard();
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
  if (!output) return false;
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  return static_cast<bool>(output);
#endif
}

std::optional<std::string> ReadPrivateFile(const std::filesystem::path& path,
                                           std::size_t maximum_bytes,
                                           std::string* contents) {
#if defined(__unix__) || defined(__APPLE__)
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return "cannot open datastore snapshot";
  struct stat status {};
  if (fstat(descriptor, &status) != 0) {
    close(descriptor);
    return "cannot inspect datastore snapshot";
  }
  if (!S_ISREG(status.st_mode)) {
    close(descriptor);
    return "datastore snapshot is not a regular file";
  }
  if ((status.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    close(descriptor);
    return "datastore snapshot permissions must be 0600 or stricter";
  }
  if (status.st_uid != geteuid()) {
    close(descriptor);
    return "datastore snapshot is not owned by the effective user";
  }
  char buffer[8192];
  while (true) {
    const ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      close(descriptor);
      return "cannot read datastore snapshot";
    }
    if (count == 0) break;
    if (contents->size() + static_cast<std::size_t>(count) > maximum_bytes) {
      close(descriptor);
      return "datastore snapshot exceeds the byte limit";
    }
    contents->append(buffer, static_cast<std::size_t>(count));
  }
  if (close(descriptor) != 0) return "cannot close datastore snapshot";
#else
  std::error_code size_error;
  const std::uintmax_t size = std::filesystem::file_size(path, size_error);
  if (!size_error && size > maximum_bytes)
    return "datastore snapshot exceeds the byte limit";
  std::ifstream input(path, std::ios::binary);
  if (!input) return "cannot open datastore snapshot";
  contents->assign(std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>());
#endif
  return std::nullopt;
}

}  // namespace

PersistenceResult SaveDatastoreSnapshot(
    const std::filesystem::path& path, const DatastoreManager& datastores,
    const SnapshotSaveCheckpoint& checkpoint) {
  return SaveDatastoreSnapshot(path, datastores.ExportPersistentState(),
                               checkpoint);
}

PersistenceResult SaveDatastoreSnapshot(
    const std::filesystem::path& path, const PersistentDatastoreState& state,
    const SnapshotSaveCheckpoint& checkpoint) {
  if (path.empty()) return {false, "snapshot path is empty"};
  const std::string contents = ToJson(state).dump(2) + '\n';
  std::filesystem::path temporary;
  bool created = false;
  for (unsigned attempt = 0; attempt < 16 && !created; ++attempt) {
    temporary = path;
    temporary += ".tmp-" + std::to_string(std::random_device{}());
    created = WritePrivateFile(temporary, contents);
  }
  if (!created) return {false, "cannot write private temporary datastore snapshot"};
  const auto remove_temporary = [&temporary]() {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  };
  if (checkpoint && !checkpoint(SnapshotSaveStage::kTemporaryWritten)) {
    remove_temporary();
    return {false, "snapshot save interrupted after temporary write"};
  }
  if (!SyncPath(temporary)) {
    remove_temporary();
    return {false, "cannot synchronize temporary datastore snapshot"};
  }
  if (checkpoint && !checkpoint(SnapshotSaveStage::kTemporarySynchronized)) {
    remove_temporary();
    return {false, "snapshot save interrupted after temporary synchronization"};
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return {false, "cannot atomically replace datastore snapshot"};
  }
  if (checkpoint && !checkpoint(SnapshotSaveStage::kSnapshotReplaced)) {
    return {false, "snapshot save interrupted after atomic replacement"};
  }
  const std::filesystem::path parent =
      path.parent_path().empty() ? std::filesystem::path(".")
                                 : path.parent_path();
  if (!SyncPath(parent, true))
    return {false, "snapshot was replaced but its directory cannot be synchronized"};
  if (checkpoint && !checkpoint(SnapshotSaveStage::kDirectorySynchronized)) {
    return {false, "snapshot save interrupted after directory synchronization"};
  }
  return {true, std::nullopt};
}

PersistenceResult LoadDatastoreSnapshot(const std::filesystem::path& path,
                                        DatastoreManager& datastores,
                                        DatastoreManager::RestoreBackend backend) {
  std::string contents;
  if (const auto error = ReadPrivateFile(
          path, DefaultResourceLimits().maximum_snapshot_bytes, &contents))
    return {false, *error};
  return LoadDatastoreSnapshotJson(contents, datastores, backend);
}

PersistenceResult LoadDatastoreSnapshotJson(std::string_view contents,
                                            DatastoreManager& datastores,
                                            DatastoreManager::RestoreBackend backend) {
  if (contents.size() > DefaultResourceLimits().maximum_snapshot_bytes) {
    return {false, "datastore snapshot exceeds the byte limit"};
  }
  try {
    const nlohmann::json json = nlohmann::json::parse(contents);
    const auto state = FromJson(json);
    if (!state) return {false, "unsupported or malformed datastore snapshot"};
    TransactionResult restored =
        datastores.RestorePersistentState(*state, backend);
    if (!restored.ok) {
      return {false, restored.errors.empty()
                         ? "datastore snapshot restore failed"
                         : restored.errors.front().message};
    }
    return {true, std::nullopt};
  } catch (const nlohmann::json::exception&) {
    return {false, "datastore snapshot is not valid JSON"};
  }
}

}  // namespace yang::netconf
