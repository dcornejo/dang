// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_PERSISTENCE_H_
#define YANG_NETCONF_PERSISTENCE_H_

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "yang/netconf_datastore.h"

namespace yang::netconf {

struct PersistenceResult {
  bool ok = false;
  std::optional<std::string> error;
};

/** Durable-save milestones exposed for deterministic fault injection. */
enum class SnapshotSaveStage {
  kTemporaryWritten,
  kTemporarySynchronized,
  kSnapshotReplaced,
  kDirectorySynchronized,
};

/** Optional callback; returning false simulates interruption at that stage. */
using SnapshotSaveCheckpoint =
    std::function<bool(SnapshotSaveStage stage)>;

/** Saves a private versioned JSON snapshot using atomic replacement. */
[[nodiscard]] PersistenceResult SaveDatastoreSnapshot(
    const std::filesystem::path& path, const DatastoreManager& datastores,
    const SnapshotSaveCheckpoint& checkpoint = {});
/** Saves an already captured state without reentering its datastore manager. */
[[nodiscard]] PersistenceResult SaveDatastoreSnapshot(
    const std::filesystem::path& path, const PersistentDatastoreState& state,
    const SnapshotSaveCheckpoint& checkpoint = {});
/** Loads a private regular snapshot owned by this process and validates it. */
[[nodiscard]] PersistenceResult LoadDatastoreSnapshot(
    const std::filesystem::path& path, DatastoreManager& datastores,
    DatastoreManager::RestoreBackend backend =
        DatastoreManager::RestoreBackend::kApply);
/** Parses and restores an in-memory versioned JSON snapshot. */
[[nodiscard]] PersistenceResult LoadDatastoreSnapshotJson(
    std::string_view json, DatastoreManager& datastores,
    DatastoreManager::RestoreBackend backend =
        DatastoreManager::RestoreBackend::kApply);

}  // namespace yang::netconf

#endif  // YANG_NETCONF_PERSISTENCE_H_
