// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/configuration_backend.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <pugixml.hpp>
#include <sstream>
#include <utility>

#include "dangd/peer_identity.h"
#include "yang/xml_security.h"

namespace dangd {
namespace {

std::string Quote(const std::optional<std::string>& value) {
  if (!value) return {};
  std::string result = "\"";
  for (const char character : *value) {
    if (character == '\\' || character == '"') result += '\\';
    result += character;
  }
  return result + "\"";
}

std::string Describe(const yang::config::ChangeEvent& change) {
  using yang::config::ChangeKind;
  if (change.kind == ChangeKind::kCreated) {
    std::string result = "Created " + change.instance_path;
    if (change.after) result += " with value " + Quote(change.after);
    return result + ".";
  }
  if (change.kind == ChangeKind::kDeleted) {
    std::string result = "Deleted " + change.instance_path;
    if (change.before) result += ", whose value was " + Quote(change.before);
    return result + ".";
  }
  if (change.kind == ChangeKind::kValueChanged) {
    return "Changed " + change.instance_path + " from " + Quote(change.before) +
           " to " + Quote(change.after) + ".";
  }
  if (change.kind == ChangeKind::kMoved) {
    return "Moved " + change.instance_path + " from " +
           change.before.value_or("an unknown position") + " to " +
           change.after.value_or("an unknown position") + ".";
  }
  return "Replaced the configuration subtree at " + change.instance_path + ".";
}

std::string ManagedNacmXml(const yang::config::ConfigDocument& document) {
  pugi::xml_document parsed;
  const std::string xml = document.ToXml();
  if (!yang::ParseUntrustedXml(xml, &parsed).ok) return {};
  constexpr std::string_view kNamespace =
      "urn:ietf:params:xml:ns:yang:ietf-netconf-acm";
  for (const pugi::xml_node child : parsed.document_element().children()) {
    const std::string_view name = child.name();
    const std::size_t colon = name.find(':');
    const std::string_view local =
        colon == std::string_view::npos ? name : name.substr(colon + 1);
    if (local != "nacm") continue;
    std::string namespace_uri;
    if (colon == std::string_view::npos) {
      namespace_uri = child.attribute("xmlns").as_string();
    } else {
      const std::string attribute =
          "xmlns:" + std::string(name.substr(0, colon));
      namespace_uri = child.attribute(attribute.c_str()).as_string();
    }
    if (namespace_uri != kNamespace) continue;
    std::ostringstream output;
    child.print(output, "  ", pugi::format_raw);
    return output.str();
  }
  return {};
}

std::optional<std::string> SecureTransactionId() {
  std::array<unsigned char, 32> bytes{};
  if (RAND_priv_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    return std::nullopt;
  constexpr char kHex[] = "0123456789abcdef";
  std::string result = "peer-";
  result.reserve(5 + bytes.size() * 2);
  for (const unsigned char byte : bytes) {
    result.push_back(kHex[byte >> 4]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

void AppendDigestField(std::string_view value, std::string* material) {
  *material += std::to_string(value.size());
  material->push_back(':');
  material->append(value);
}

std::optional<std::string> PeerProposalDigest(
    const ComposedPeerTransactionGroup& group) {
  std::string material;
  AppendDigestField("dangd-peer-proposal-v1", &material);
  AppendDigestField(group.group_id, &material);
  AppendDigestField(std::to_string(group.participants.size()), &material);
  for (const PeerPlanParticipant& participant : group.participants) {
    AppendDigestField("participant", &material);
    AppendDigestField(participant.participant_id, &material);
    AppendDigestField(std::to_string(participant.role), &material);
    AppendDigestField(std::to_string(participant.confirmed_timeout_seconds),
                      &material);
    AppendDigestField(participant.candidate_configuration, &material);
    AppendDigestField(std::to_string(participant.verifiers.size()), &material);
    for (const PeerPlanVerifier& verifier : participant.verifiers) {
      AppendDigestField("verifier", &material);
      AppendDigestField(verifier.provider, &material);
      AppendDigestField(verifier.verification_context_json, &material);
    }
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  if (EVP_Digest(material.data(), material.size(), digest.data(), &size,
                 EVP_sha256(), nullptr) != 1)
    return std::nullopt;
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(static_cast<std::size_t>(size) * 2);
  for (unsigned int index = 0; index < size; ++index) {
    result.push_back(kHex[digest[index] >> 4]);
    result.push_back(kHex[digest[index] & 0x0f]);
  }
  return result;
}

yang::config::ValidationFinding PeerFailure(std::string message,
                                            std::string app_tag) {
  yang::config::ValidationFinding finding;
  finding.code = yang::config::ValidationCode::kInvalidValue;
  finding.state = yang::config::FindingState::kInvalid;
  finding.message = std::move(message);
  finding.module_name = "dangd";
  finding.netconf_error_tag = "operation-failed";
  finding.netconf_error_app_tag = std::move(app_tag);
  return finding;
}

}  // namespace

EnglishConfigurationBackend::EnglishConfigurationBackend(
    yang::config::ConfigDocument initial, PluginRuntime* plugins,
    yang::netconf::NacmPolicy* nacm, bool managed_nacm,
    std::vector<PeerRecoveryTarget> peer_targets,
    std::optional<std::filesystem::path> peer_transaction_journal,
    bool durable_state_configured, PeerParticipantFactory participant_factory,
    PeerPersistentIdFactory persistent_id_factory)
    : plugins_(plugins),
      nacm_(nacm),
      managed_nacm_(managed_nacm),
      durable_state_configured_(durable_state_configured),
      peer_transaction_journal_(std::move(peer_transaction_journal)),
      working_xml_(initial.ToXml()),
      working_(std::move(initial)) {
  for (const PeerRecoveryTarget& target : peer_targets)
    peer_targets_.emplace(PeerRecoveryTargetId(target), target);
  if (peer_transaction_journal_) {
    peer_controller_ = std::make_unique<PeerTransactionController>(
        std::move(peer_targets),
        [this](const PluginPeerVerification& verification) {
          if (plugins_) return plugins_->VerifyPeer(verification);
          return std::optional<yang::config::ValidationFinding>(
              PeerFailure("peer verification has no plugin runtime",
                          "peer-verifier-unavailable"));
        },
        std::move(participant_factory), std::move(persistent_id_factory));
  }
}

std::optional<yang::config::ValidationFinding>
EnglishConfigurationBackend::Initialize(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& configuration,
    yang::netconf::BackendTransactionContext context) {
  auto empty = yang::config::ParseDatastoreXml(
      schema, "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\"/>");
  if (!empty.document) {
    yang::config::ValidationFinding finding;
    finding.code = yang::config::ValidationCode::kMalformedXml;
    finding.state = yang::config::FindingState::kInvalid;
    finding.message = "cannot construct empty startup configuration";
    finding.netconf_error_tag = "operation-failed";
    return finding;
  }
  const auto changes =
      yang::config::DiffConfigDocuments(schema, *empty.document, configuration);
  // Startup hydration applies one already-authoritative local snapshot. It is
  // not a new distributed change and must never originate a peer transaction.
  context.externally_coordinated = true;
  if (auto error = PrepareReplacement(schema, *empty.document, configuration,
                                      changes, context))
    return error;
  if (auto error =
          Replace(schema, *empty.document, configuration, changes, context)) {
    AbortPreparedReplacement();
    return error;
  }
  if (auto error = CommitPreparedReplacement()) return error;
  // Startup activation is not a user configuration edit.
  std::lock_guard lock(mutex_);
  deltas_.clear();
  return std::nullopt;
}

std::optional<yang::config::ValidationFinding>
EnglishConfigurationBackend::PrepareReplacement(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& before,
    const yang::config::ConfigDocument& after,
    std::span<const yang::config::ChangeEvent> changes,
    yang::netconf::BackendTransactionContext context) {
  if (prepared_peer_transaction_ && peer_controller_) {
    (void)peer_controller_->Abort(std::move(prepared_peer_transaction_));
  }
  prepared_recovery_state_.reset();
  prepared_nacm_.reset();
  prepared_peer_groups_.clear();
  if (plugins_) {
    if (auto error = plugins_->Prepare(schema, before, after, changes))
      return error;
    if (!context.externally_coordinated) {
      std::optional<yang::config::ValidationFinding> candidate_error;
      std::vector<PluginPeerCandidate> candidates =
          plugins_->PeerCandidates(&candidate_error);
      if (candidate_error) {
        plugins_->Abort();
        return candidate_error;
      }
      ComposePeerTransactionResult composed =
          ComposePeerTransactionPlan(schema, candidates);
      if (composed.error) {
        plugins_->Abort();
        return composed.error;
      }
      // One NETCONF transaction must never be split across independently
      // durable group journals. Until a multi-group journal exists, reject the
      // complete proposal before resolving endpoints or mutating any plugin.
      if (composed.groups.size() > 1) {
        yang::config::ValidationFinding finding;
        finding.code = yang::config::ValidationCode::kInvalidValue;
        finding.state = yang::config::FindingState::kInvalid;
        finding.message =
            "one configuration transaction may affect at most one peer group";
        finding.module_name = "dangd";
        finding.netconf_error_tag = "operation-failed";
        finding.netconf_error_app_tag = "peer-group-limit";
        plugins_->Abort();
        return finding;
      }
      for (const ComposedPeerTransactionGroup& group : composed.groups) {
        for (const PeerPlanParticipant& participant : group.participants) {
          const std::string identity =
              PeerIdentity(group.group_id, participant.participant_id);
          if (peer_targets_.contains(identity)) continue;
          yang::config::ValidationFinding finding;
          finding.code = yang::config::ValidationCode::kInvalidValue;
          finding.state = yang::config::FindingState::kInvalid;
          finding.message =
              "peer transaction target " + identity + " is not configured";
          finding.module_name = "dangd";
          finding.netconf_error_tag = "operation-failed";
          finding.netconf_error_app_tag = "peer-target-missing";
          plugins_->Abort();
          return finding;
        }
      }
      prepared_peer_groups_ = std::move(composed.groups);
    }
  }
  if (managed_nacm_) {
    const std::string xml = ManagedNacmXml(after);
    if (xml.empty()) {
      yang::netconf::NacmPolicy defaults;
      if (nacm_) defaults.PreserveRuntimeStateFrom(*nacm_);
      prepared_nacm_ = std::move(defaults);
    } else {
      auto loaded = yang::netconf::LoadNacmPolicy(xml);
      if (!loaded.policy) {
        yang::config::ValidationFinding finding;
        finding.code = yang::config::ValidationCode::kInvalidValue;
        finding.state = yang::config::FindingState::kInvalid;
        finding.message = loaded.errors.empty()
                              ? "NACM configuration cannot be compiled"
                              : loaded.errors.front();
        finding.instance_path =
            "/{urn:ietf:params:xml:ns:yang:ietf-netconf-acm}nacm";
        finding.module_name = "ietf-netconf-acm";
        finding.netconf_error_tag = "invalid-value";
        finding.netconf_error_app_tag = "invalid-nacm-policy";
        AbortPreparedReplacement();
        return finding;
      }
      if (nacm_) loaded.policy->PreserveRuntimeStateFrom(*nacm_);
      prepared_nacm_ = std::move(*loaded.policy);
    }
  }
  if (!prepared_peer_groups_.empty()) {
    if (!durable_state_configured_) {
      if (plugins_) plugins_->Abort();
      prepared_nacm_.reset();
      prepared_peer_groups_.clear();
      return PeerFailure(
          "peer transactions require a persistent datastore state file",
          "peer-state-persistence-required");
    }
    if (!peer_controller_ || !peer_transaction_journal_) {
      if (plugins_) plugins_->Abort();
      prepared_nacm_.reset();
      prepared_peer_groups_.clear();
      return PeerFailure("peer transaction coordination is not configured",
                         "peer-controller-unavailable");
    }
    const auto transaction_id = SecureTransactionId();
    const auto proposal_digest =
        PeerProposalDigest(prepared_peer_groups_.front());
    if (!transaction_id || !proposal_digest) {
      if (plugins_) plugins_->Abort();
      prepared_nacm_.reset();
      prepared_peer_groups_.clear();
      return PeerFailure("cannot create secure peer transaction metadata",
                         "peer-metadata-failed");
    }
    PeerTransactionControllerPrepareResult prepared = peer_controller_->Prepare(
        prepared_peer_groups_.front(), *peer_transaction_journal_,
        *transaction_id, *proposal_digest);
    if (!prepared.ok()) {
      if (plugins_) plugins_->Abort();
      prepared_nacm_.reset();
      prepared_peer_groups_.clear();
      return PeerFailure(std::move(prepared.result.message),
                         "peer-prepare-failed");
    }
    prepared_recovery_state_ = yang::netconf::BackendRecoveryState{
        .kind = "peer-transaction-v1",
        .transaction_id = prepared.prepared->transaction_id,
        .proposal_digest = prepared.prepared->proposal_digest};
    prepared_peer_transaction_ = std::move(prepared.prepared);
  }
  return std::nullopt;
}

std::optional<yang::config::ValidationFinding>
EnglishConfigurationBackend::Replace(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument&,
    const yang::config::ConfigDocument& after,
    std::span<const yang::config::ChangeEvent> changes,
    yang::netconf::BackendTransactionContext) {
  if (plugins_) {
    PluginApplyResult applied = plugins_->Apply(schema, after);
    if (applied.error) return applied.error;
    if (applied.applied) {
      std::lock_guard lock(mutex_);
      for (const auto& change : changes) deltas_.push_back(Describe(change));
      working_ = std::move(*applied.applied);
      working_xml_ = std::move(applied.applied_xml);
      if (prepared_nacm_ && nacm_) *nacm_ = std::move(*prepared_nacm_);
      prepared_nacm_.reset();
      return std::nullopt;
    }
  }
  std::lock_guard lock(mutex_);
  for (const auto& change : changes) deltas_.push_back(Describe(change));
  working_ = after;
  working_xml_ = after.ToXml();
  if (prepared_nacm_ && nacm_) *nacm_ = std::move(*prepared_nacm_);
  prepared_nacm_.reset();
  return std::nullopt;
}

void EnglishConfigurationBackend::AbortPreparedReplacement() noexcept {
  if (prepared_peer_transaction_ && peer_controller_) {
    (void)peer_controller_->Abort(std::move(prepared_peer_transaction_));
  }
  if (plugins_) plugins_->Abort();
  prepared_recovery_state_.reset();
  prepared_nacm_.reset();
  prepared_peer_groups_.clear();
}

std::optional<yang::netconf::BackendRecoveryState>
EnglishConfigurationBackend::PreparedReplacementRecoveryState() const {
  return prepared_recovery_state_;
}

std::optional<yang::config::ValidationFinding>
EnglishConfigurationBackend::CommitPreparedReplacement() {
  if (!prepared_peer_transaction_) {
    prepared_recovery_state_.reset();
    prepared_peer_groups_.clear();
    return std::nullopt;
  }
  PeerTransactionResult result =
      peer_controller_->Commit(std::move(prepared_peer_transaction_),
                               PeerDecisionFailurePolicy::kRetainPrepared);
  prepared_recovery_state_.reset();
  prepared_peer_groups_.clear();
  if (result.disposition == PeerTransactionDisposition::kCommitted)
    return std::nullopt;
  return PeerFailure(
      result.message.empty()
          ? "peer transaction requires startup recovery before more changes"
          : std::move(result.message),
      "peer-commit-pending");
}

yang::config::ConfigDocument EnglishConfigurationBackend::Working() const {
  std::lock_guard lock(mutex_);
  return working_;
}

std::string EnglishConfigurationBackend::WorkingXml() const {
  std::lock_guard lock(mutex_);
  return working_xml_;
}

std::vector<std::string> EnglishConfigurationBackend::DrainDeltas() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> result = std::move(deltas_);
  deltas_.clear();
  return result;
}

}  // namespace dangd
