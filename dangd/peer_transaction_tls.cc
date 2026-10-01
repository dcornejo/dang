// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_tls.h"

#include <array>
#include <pugixml.hpp>
#include <sstream>
#include <utility>

#include "yang/xml_security.h"

namespace dangd {
namespace {

constexpr std::string_view kConfirmedCommitCapability =
    "urn:ietf:params:netconf:capability:confirmed-commit:1.1";

std::string_view LocalName(std::string_view name) {
  const std::size_t separator = name.find(':');
  return separator == std::string_view::npos ? name
                                             : name.substr(separator + 1);
}

std::string Namespace(pugi::xml_node node) {
  const std::string_view name = node.name();
  const std::size_t separator = name.find(':');
  const std::string declaration =
      separator == std::string_view::npos
          ? "xmlns"
          : "xmlns:" + std::string(name.substr(0, separator));
  for (pugi::xml_node current = node; current; current = current.parent()) {
    const pugi::xml_attribute attribute =
        current.attribute(declaration.c_str());
    if (attribute) return attribute.as_string();
  }
  return {};
}

std::string ConfirmationRpc(const std::string& persistent_commit_id) {
  pugi::xml_document document;
  pugi::xml_node rpc = document.append_child("rpc");
  rpc.append_attribute("xmlns") = "urn:ietf:params:xml:ns:netconf:base:1.0";
  rpc.append_attribute("message-id") = "dangd-peer-recovery";
  pugi::xml_node commit = rpc.append_child("commit");
  commit.append_child("persist-id").text() = persistent_commit_id.c_str();
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

bool ReplyIsOk(std::string_view reply) {
  pugi::xml_document parsed;
  if (!yang::ParseUntrustedXml(reply, &parsed).ok) return false;
  pugi::xml_node result;
  for (const pugi::xml_node child : parsed.document_element().children()) {
    if (child.type() != pugi::node_element) continue;
    if (result) return false;
    result = child;
  }
  return result && LocalName(result.name()) == "ok" &&
         Namespace(result) == "urn:ietf:params:xml:ns:netconf:base:1.0";
}

}  // namespace

std::optional<std::string> ConfirmPersistentCommitOverTls(
    const TlsClientOptions& options, const std::string& persistent_commit_id) {
  if (persistent_commit_id.empty() || persistent_commit_id.size() > 256 ||
      persistent_commit_id.find('\0') != std::string::npos)
    return "persistent commit identity is invalid";
  std::string error;
  constexpr std::array<std::string_view, 1> required_capabilities = {
      kConfirmedCommitCapability};
  const auto exchanged =
      ExchangeTlsRpc(options, ConfirmationRpc(persistent_commit_id), &error,
                     required_capabilities);
  if (!exchanged) return error;
  if (!ReplyIsOk(exchanged->reply))
    return "peer rejected persistent commit confirmation";
  return std::nullopt;
}

PeerTransactionParticipant MakeTlsRecoveryParticipant(
    const PeerJournalParticipant& journal_participant,
    TlsClientOptions options) {
  const std::string persistent_commit_id =
      journal_participant.persistent_commit_id;
  return {
      .id = journal_participant.id,
      .role = journal_participant.role,
      .confirm =
          [options = std::move(options), persistent_commit_id] {
            return ConfirmPersistentCommitOverTls(options,
                                                  persistent_commit_id);
          },
      .release = [] {},
  };
}

}  // namespace dangd
