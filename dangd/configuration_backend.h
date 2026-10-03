// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_CONFIGURATION_BACKEND_H_
#define DANGD_CONFIGURATION_BACKEND_H_

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "dangd/peer_recovery_config.h"
#include "dangd/peer_transaction_controller.h"
#include "dangd/peer_transaction_plan.h"
#include "dangd/plugin_manager.h"
#include "yang/nacm.h"
#include "yang/netconf_datastore.h"

namespace dangd {

/** In-memory backend that replaces its working configuration and describes it.
 */
class EnglishConfigurationBackend final
    : public yang::netconf::RunningConfigBackend {
 public:
  /** Initializes the backend with the already validated running document. */
  EnglishConfigurationBackend(
      yang::config::ConfigDocument initial, PluginRuntime* plugins,
      yang::netconf::NacmPolicy* nacm, bool managed_nacm,
      std::vector<PeerRecoveryTarget> peer_targets = {},
      std::optional<std::filesystem::path> peer_transaction_journal =
          std::nullopt,
      bool durable_state_configured = false,
      PeerParticipantFactory participant_factory =
          MakeTlsTransactionParticipant,
      PeerPersistentIdFactory persistent_id_factory = {},
      std::chrono::milliseconds peer_transaction_timeout =
          std::chrono::seconds(30));

  /** Activates one complete startup tree as a single plugin transaction. */
  [[nodiscard]] std::optional<yang::config::ValidationFinding> Initialize(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& configuration,
      yang::netconf::BackendTransactionContext context = {});

  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  PrepareReplacement(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& before,
      const yang::config::ConfigDocument& after,
      std::span<const yang::config::ChangeEvent> changes,
      yang::netconf::BackendTransactionContext context = {}) override;

  /** Records exact changes in English and atomically replaces the working copy.
   */
  [[nodiscard]] std::optional<yang::config::ValidationFinding> Replace(
      const yang::config::RuntimeSchema& schema,
      const yang::config::ConfigDocument& before,
      const yang::config::ConfigDocument& after,
      std::span<const yang::config::ChangeEvent> changes,
      yang::netconf::BackendTransactionContext context = {}) override;
  void AbortPreparedReplacement() noexcept override;
  [[nodiscard]] std::optional<yang::netconf::BackendRecoveryState>
  PreparedReplacementRecoveryState() const override;
  [[nodiscard]] std::optional<yang::config::ValidationFinding>
  CommitPreparedReplacement() override;

  /** Returns an immutable snapshot of the backend working configuration. */
  [[nodiscard]] yang::config::ConfigDocument Working() const;
  /** Returns applied XML while retaining validated instance metadata. */
  [[nodiscard]] std::string WorkingXml() const;
  /** Returns and clears all descriptions accumulated since the preceding call.
   */
  [[nodiscard]] std::vector<std::string> DrainDeltas();

 private:
  mutable std::mutex mutex_;
  PluginRuntime* plugins_ = nullptr;
  yang::netconf::NacmPolicy* nacm_ = nullptr;
  bool managed_nacm_ = false;
  bool durable_state_configured_ = false;
  std::optional<yang::netconf::NacmPolicy> prepared_nacm_;
  /** Schema-validated generic peer plan retained with plugin preparations. */
  std::vector<ComposedPeerTransactionGroup> prepared_peer_groups_;
  /** Core-owned authenticated endpoints keyed by generic contract identity. */
  std::map<std::string, PeerRecoveryTarget, std::less<>> peer_targets_;
  /** Generic controller created only from host-owned endpoint configuration. */
  std::unique_ptr<PeerTransactionController> peer_controller_;
  /** Private crash-safe path shared by live coordination and startup recovery.
   */
  std::optional<std::filesystem::path> peer_transaction_journal_;
  /** Live verified remote work retained until local state is durable. */
  std::unique_ptr<PreparedPeerTransactionHandle> prepared_peer_transaction_;
  /** Opaque marker copied into the local snapshot before finalization. */
  std::optional<yang::netconf::BackendRecoveryState> prepared_recovery_state_;
  std::string working_xml_;
  yang::config::ConfigDocument working_;
  std::vector<std::string> deltas_;
};

}  // namespace dangd

#endif  // DANGD_CONFIGURATION_BACKEND_H_
