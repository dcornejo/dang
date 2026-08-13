// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/netconf_persistence.h"

#include "yang/resource_limits.h"

#include <fstream>
#include <iterator>
#include <random>
#include <system_error>

#include <nlohmann/json.hpp>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace yang::netconf {
namespace {

constexpr int kSnapshotVersion = 1;

nlohmann::json ToJson(const PersistentDatastoreState& state) {
  return {{"version", kSnapshotVersion},
          {"running", state.running_xml},
          {"candidate", state.candidate_xml},
          {"startup", state.startup_xml},
          {"rollback-running", state.rollback_running_xml},
          {"confirmation-expiry-unix-seconds",
           state.confirmation_expiry_unix_seconds},
          {"confirming-session", state.confirming_session},
          {"persist-token", state.persist_token}};
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

}  // namespace

PersistenceResult SaveDatastoreSnapshot(
    const std::filesystem::path& path, const DatastoreManager& datastores,
    const SnapshotSaveCheckpoint& checkpoint) {
  if (path.empty()) return {false, "snapshot path is empty"};
  std::filesystem::path temporary = path;
  temporary += ".tmp-" + std::to_string(std::random_device{}());
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output) return {false, "cannot open temporary datastore snapshot"};
  output << ToJson(datastores.ExportPersistentState()).dump(2) << '\n';
  output.flush();
  if (!output) {
    output.close();
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return {false, "cannot write temporary datastore snapshot"};
  }
  output.close();
  if (checkpoint && !checkpoint(SnapshotSaveStage::kTemporaryWritten)) {
    return {false, "snapshot save interrupted after temporary write"};
  }
  if (!SyncPath(temporary)) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return {false, "cannot synchronize temporary datastore snapshot"};
  }
  if (checkpoint && !checkpoint(SnapshotSaveStage::kTemporarySynchronized)) {
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
                                        DatastoreManager& datastores) {
  std::error_code size_error;
  const std::uintmax_t size = std::filesystem::file_size(path, size_error);
  if (!size_error && size > DefaultResourceLimits().maximum_snapshot_bytes) {
    return {false, "datastore snapshot exceeds the byte limit"};
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) return {false, "cannot open datastore snapshot"};
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  return LoadDatastoreSnapshotJson(contents, datastores);
}

PersistenceResult LoadDatastoreSnapshotJson(std::string_view contents,
                                            DatastoreManager& datastores) {
  if (contents.size() > DefaultResourceLimits().maximum_snapshot_bytes) {
    return {false, "datastore snapshot exceeds the byte limit"};
  }
  try {
    const nlohmann::json json = nlohmann::json::parse(contents);
    const auto state = FromJson(json);
    if (!state) return {false, "unsupported or malformed datastore snapshot"};
    TransactionResult restored = datastores.RestorePersistentState(*state);
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
