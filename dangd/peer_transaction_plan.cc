// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_plan.h"

#include <algorithm>
#include <map>
#include <pugixml.hpp>
#include <set>
#include <sstream>
#include <utility>

#include "dangd/peer_identity.h"
#include "yang/xml_security.h"

namespace dangd {
namespace {

constexpr std::string_view kNetconfNamespace =
    "urn:ietf:params:xml:ns:netconf:base:1.0";

yang::config::ValidationFinding Failure(std::string provider,
                                        std::string message) {
  yang::config::ValidationFinding finding;
  finding.message = std::move(message);
  finding.module_name = std::move(provider);
  finding.netconf_error_tag = "operation-failed";
  finding.netconf_error_app_tag = "peer-plan-invalid";
  return finding;
}

struct ParticipantBuilder {
  std::uint32_t role = 0;
  std::uint32_t timeout = 0;
  pugi::xml_document document;
  pugi::xml_node root;
  std::set<std::string> modules;
  std::map<std::string, std::string, std::less<>> verifiers;

  ParticipantBuilder() {
    root = document.append_child("config");
    root.append_attribute("xmlns") = kNetconfNamespace.data();
  }
};

}  // namespace

ComposePeerTransactionResult ComposePeerTransactionPlan(
    const yang::config::RuntimeSchema& schema,
    const std::vector<PluginPeerCandidate>& contributions) {
  ComposePeerTransactionResult result;
  using ParticipantKey = std::pair<std::string, std::string>;
  std::map<ParticipantKey, ParticipantBuilder> participants;
  std::map<std::string, std::set<std::string>, std::less<>> group_modules;
  for (const PluginPeerCandidate& contribution : contributions) {
    if (!IsValidPeerIdentityComponent(contribution.group_id) ||
        !IsValidPeerIdentityComponent(contribution.participant_id) ||
        contribution.provider.empty() || contribution.module_name.empty()) {
      result.error = Failure(contribution.provider,
                             "peer plan contains an invalid stable identity");
      return result;
    }
    const ParticipantKey key{contribution.group_id,
                             contribution.participant_id};
    ParticipantBuilder& participant = participants[key];
    if (participant.role == 0) {
      participant.role = contribution.role;
      participant.timeout = contribution.confirmed_timeout_seconds;
    } else if (participant.role != contribution.role ||
               participant.timeout != contribution.confirmed_timeout_seconds) {
      result.error =
          Failure(contribution.provider,
                  "peer contributors disagree on participant role or timeout");
      return result;
    }
    if (!participant.modules.emplace(contribution.module_name).second) {
      result.error =
          Failure(contribution.provider,
                  "more than one plugin supplied the same peer module image");
      return result;
    }
    const auto [verifier, inserted] = participant.verifiers.emplace(
        contribution.provider, contribution.verification_context_json);
    if (!inserted &&
        verifier->second != contribution.verification_context_json) {
      result.error =
          Failure(contribution.provider,
                  "plugin supplied inconsistent peer verification context");
      return result;
    }

    auto parsed = yang::config::ParseDatastoreXml(
        schema, contribution.configuration_xml,
        {.coverage = yang::config::Coverage::kSelected});
    if (!parsed.document) {
      result.error =
          parsed.findings.empty()
              ? Failure(contribution.provider, "peer module image is invalid")
              : parsed.findings.front();
      result.error->message =
          "plugin " + contribution.provider +
          " returned an invalid peer module image: " + result.error->message;
      result.error->module_name = contribution.provider;
      result.error->netconf_error_app_tag = "peer-plan-invalid";
      return result;
    }
    for (const auto root_id : parsed.document->roots()) {
      const std::string& actual_module =
          schema.Get(parsed.document->Get(root_id).schema).module_name;
      if (actual_module != contribution.module_name) {
        result.error = Failure(
            contribution.provider,
            "peer module image contains data owned by " + actual_module);
        return result;
      }
    }
    pugi::xml_document fragment;
    const yang::UntrustedXmlResult xml =
        yang::ParseUntrustedXml(contribution.configuration_xml, &fragment);
    if (!xml.ok) {
      result.error = Failure(contribution.provider, xml.message);
      return result;
    }
    const pugi::xml_node fragment_root = fragment.document_element();
    for (const pugi::xml_node child : fragment_root.children())
      if (child.type() == pugi::node_element)
        participant.root.append_copy(child);
    group_modules[contribution.group_id].insert(contribution.module_name);
  }

  std::map<std::string, std::vector<ParticipantKey>, std::less<>> groups;
  for (const auto& [key, participant] : participants)
    groups[key.first].push_back(key);
  for (const auto& [group_id, keys] : groups) {
    if (keys.size() < 2) {
      result.error =
          Failure(group_id, "peer transaction group requires two participants");
      return result;
    }
    std::size_t primaries = 0;
    ComposedPeerTransactionGroup group{.group_id = group_id};
    for (const ParticipantKey& key : keys) {
      ParticipantBuilder& builder = participants.at(key);
      if (builder.modules != group_modules.at(group_id)) {
        result.error = Failure(
            group_id,
            "every peer participant must receive the same complete module set");
        return result;
      }
      if (builder.role == DANG_PEER_PRIMARY_V1) ++primaries;
      std::ostringstream serialized;
      builder.document.save(serialized, "", pugi::format_raw);
      std::string candidate = serialized.str();
      auto complete = yang::config::ParseDatastoreXml(schema, candidate);
      if (!complete.document) {
        result.error =
            complete.findings.empty()
                ? Failure(group_id, "composed peer candidate is invalid")
                : complete.findings.front();
        result.error->message = "composed peer candidate " + key.second +
                                " is invalid: " + result.error->message;
        result.error->netconf_error_app_tag = "peer-plan-invalid";
        return result;
      }
      PeerPlanParticipant output{
          key.second, builder.role, builder.timeout, std::move(candidate), {}};
      for (auto& [provider, context] : builder.verifiers)
        output.verifiers.push_back({provider, context});
      group.participants.push_back(std::move(output));
    }
    if (primaries != 1) {
      result.error = Failure(
          group_id, "peer transaction group requires exactly one primary");
      return result;
    }
    result.groups.push_back(std::move(group));
  }
  return result;
}

}  // namespace dangd
