// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_tls.h"

#include <array>
#include <memory>
#include <pugixml.hpp>
#include <sstream>
#include <string_view>
#include <utility>

#include "yang/xml_security.h"

namespace dangd {
namespace {

constexpr std::string_view kConfirmedCommitCapability =
    "urn:ietf:params:netconf:capability:confirmed-commit:1.1";
constexpr std::string_view kCandidateCapability =
    "urn:ietf:params:netconf:capability:candidate:1.0";
constexpr std::string_view kValidateCapability =
    "urn:ietf:params:netconf:capability:validate:1.1";
constexpr std::string_view kNetconfNamespace =
    "urn:ietf:params:xml:ns:netconf:base:1.0";

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
    if (attribute)
      return attribute.as_string();
  }
  return {};
}

std::string ConfirmationRpc(const std::string &persistent_commit_id) {
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
  if (!yang::ParseUntrustedXml(reply, &parsed).ok)
    return false;
  pugi::xml_node result;
  for (const pugi::xml_node child : parsed.document_element().children()) {
    if (child.type() != pugi::node_element)
      continue;
    if (result)
      return false;
    result = child;
  }
  return result && LocalName(result.name()) == "ok" &&
         Namespace(result) == kNetconfNamespace;
}

std::optional<std::string> RpcErrorTag(std::string_view reply) {
  pugi::xml_document parsed;
  if (!yang::ParseUntrustedXml(reply, &parsed).ok)
    return std::nullopt;
  pugi::xml_node error;
  for (const pugi::xml_node child : parsed.document_element().children()) {
    if (child.type() != pugi::node_element)
      continue;
    if (LocalName(child.name()) != "rpc-error" ||
        Namespace(child) != kNetconfNamespace || error)
      return std::nullopt;
    error = child;
  }
  if (!error)
    return std::nullopt;
  for (const pugi::xml_node child : error.children()) {
    if (child.type() == pugi::node_element &&
        LocalName(child.name()) == "error-tag" &&
        Namespace(child) == kNetconfNamespace)
      return child.text().as_string();
  }
  return std::nullopt;
}

std::string Rpc(std::string_view message_id, std::string_view operation) {
  pugi::xml_document document;
  pugi::xml_node rpc = document.append_child("rpc");
  rpc.append_attribute("xmlns") = kNetconfNamespace.data();
  rpc.append_attribute("message-id") = std::string(message_id).c_str();
  rpc.append_buffer(operation.data(), operation.size());
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

std::string PersistentCommitOperation(std::string_view element,
                                      std::string_view persistent_commit_id) {
  pugi::xml_document document;
  pugi::xml_node operation =
      document.append_child(std::string(element).c_str());
  operation.append_child("persist-id").text() =
      std::string(persistent_commit_id).c_str();
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

std::string ConfirmedCommitOperation(std::uint32_t timeout_seconds,
                                     std::string_view persistent_commit_id) {
  pugi::xml_document document;
  pugi::xml_node commit = document.append_child("commit");
  commit.append_child("confirmed");
  commit.append_child("confirm-timeout").text() = timeout_seconds;
  commit.append_child("persist").text() =
      std::string(persistent_commit_id).c_str();
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

struct TlsTransactionState {
  explicit TlsTransactionState(TlsPeerTransactionOptions value)
      : options(std::move(value)) {}

  std::string NextMessageId(std::string_view operation) {
    return "dangd-peer-" + std::string(operation) + "-" +
           std::to_string(++message_sequence);
  }

  std::optional<std::string> Execute(std::string_view operation,
                                     std::string_view xml) {
    std::string error;
    const auto reply =
        session->Execute(Rpc(NextMessageId(operation), xml), &error);
    if (!reply)
      return error;
    if (!ReplyIsOk(*reply))
      return "peer rejected " + std::string(operation) +
             (RpcErrorTag(*reply) ? " (" + *RpcErrorTag(*reply) + ")" : "");
    return std::nullopt;
  }

  TlsPeerTransactionOptions options;
  std::unique_ptr<TlsRpcSession> session;
  std::uint64_t message_sequence = 0;
  bool locked = false;
  bool prepared = false;
  bool apply_attempted = false;
  bool applied = false;
  bool cancelled = false;
  bool confirmed = false;
};

std::optional<std::string>
ValidateOptions(const TlsPeerTransactionOptions &options,
                pugi::xml_document *candidate) {
  const auto invalid_identity = [](const std::string &value) {
    return value.empty() || value.size() > 256 ||
           value.find('\0') != std::string::npos;
  };
  if (invalid_identity(options.id))
    return "participant identity is invalid";
  if (invalid_identity(options.persistent_commit_id))
    return "persistent commit identity is invalid";
  if (options.confirmed_timeout_seconds == 0)
    return "confirmed commit timeout must be nonzero";
  if (!options.verify_replies)
    return "verification callback is missing";
  if (!yang::ParseUntrustedXml(options.candidate_configuration, candidate).ok)
    return "candidate configuration is not safe well-formed XML";
  const pugi::xml_node root = candidate->document_element();
  if (!root || LocalName(root.name()) != "config" ||
      Namespace(root) != kNetconfNamespace)
    return "candidate configuration must have the NETCONF config root";
  return std::nullopt;
}

std::string CopyCandidateRpc(std::string_view message_id,
                             const pugi::xml_document &candidate) {
  pugi::xml_document document;
  pugi::xml_node rpc = document.append_child("rpc");
  rpc.append_attribute("xmlns") = kNetconfNamespace.data();
  rpc.append_attribute("message-id") = std::string(message_id).c_str();
  pugi::xml_node copy = rpc.append_child("copy-config");
  copy.append_child("target").append_child("candidate");
  copy.append_child("source").append_copy(candidate.document_element());
  std::ostringstream output;
  document.print(output, "", pugi::format_raw);
  return output.str();
}

std::optional<std::string> ExecuteCancel(TlsTransactionState *state) {
  // A transport failure can hide a successful confirmed commit.  Attempt
  // cancellation after every apply attempt, not merely an acknowledged one.
  if (state->cancelled || !state->apply_attempted)
    return std::nullopt;
  const std::string operation = PersistentCommitOperation(
      "cancel-commit", state->options.persistent_commit_id);
  std::string error;
  auto execute = [&]() -> std::optional<std::string> {
    const auto reply = state->session->Execute(
        Rpc(state->NextMessageId("cancel"), operation), &error);
    if (!reply)
      return error;
    if (ReplyIsOk(*reply) || RpcErrorTag(*reply) == "invalid-value") {
      state->cancelled = true;
      state->apply_attempted = false;
      state->applied = false;
      return std::nullopt;
    }
    return "peer rejected cancel-commit" +
           (RpcErrorTag(*reply) ? " (" + *RpcErrorTag(*reply) + ")" : "");
  };
  if (state->session) {
    const auto result = execute();
    if (!result || error.empty())
      return result;
  }
  constexpr std::array<std::string_view, 1> required = {
      kConfirmedCommitCapability};
  state->session =
      TlsRpcSession::Connect(state->options.transport, &error, required);
  if (!state->session)
    return error;
  return execute();
}

} // namespace

PeerTransactionParticipant
MakeTlsTransactionParticipant(TlsPeerTransactionOptions options) {
  auto state = std::make_shared<TlsTransactionState>(std::move(options));
  const std::string id = state->options.id;
  const PeerTransactionRole role = state->options.role;
  return {
      .id = id,
      .role = role,
      .prepare = [state]() -> std::optional<std::string> {
        if (state->prepared)
          return std::nullopt;
        pugi::xml_document candidate;
        if (const auto error = ValidateOptions(state->options, &candidate))
          return error;
        std::string error;
        constexpr std::array<std::string_view, 3> required = {
            kCandidateCapability, kValidateCapability,
            kConfirmedCommitCapability};
        if (!state->session) {
          state->session = TlsRpcSession::Connect(state->options.transport,
                                                  &error, required);
          if (!state->session)
            return error;
        }
        if (!state->locked) {
          if (const auto result =
                  state->Execute("candidate lock",
                                 "<lock><target><candidate/></target></lock>"))
            return result;
          state->locked = true;
        }
        const std::string copy =
            CopyCandidateRpc(state->NextMessageId("copy"), candidate);
        const auto copied = state->session->Execute(copy, &error);
        if (!copied)
          return error;
        if (!ReplyIsOk(*copied))
          return "peer rejected complete candidate replacement" +
                 (RpcErrorTag(*copied) ? " (" + *RpcErrorTag(*copied) + ")"
                                       : "");
        if (const auto result = state->Execute(
                "candidate validation",
                "<validate><source><candidate/></source></validate>"))
          return result;
        state->prepared = true;
        return std::nullopt;
      },
      .apply_confirmed = [state]() -> std::optional<std::string> {
        if (state->applied)
          return std::nullopt;
        if (!state->prepared || !state->session)
          return "participant is not prepared";
        const std::string operation =
            ConfirmedCommitOperation(state->options.confirmed_timeout_seconds,
                                     state->options.persistent_commit_id);
        state->apply_attempted = true;
        if (const auto error = state->Execute("confirmed commit", operation))
          return error;
        state->applied = true;
        state->cancelled = false;
        return std::nullopt;
      },
      .verify = [state]() -> std::optional<std::string> {
        if (!state->applied || !state->session)
          return "participant has no applied confirmed commit";
        std::string error;
        const auto running = state->session->Execute(
            Rpc(state->NextMessageId("verify"),
                "<get-config><source><running/></source></get-config>"),
            &error);
        if (!running)
          return error;
        if (ReplyIsOk(*running) || RpcErrorTag(*running))
          return "peer rejected running configuration read" +
                 (RpcErrorTag(*running) ? " (" + *RpcErrorTag(*running) + ")"
                                        : "");
        const auto operational = state->session->Execute(
            Rpc(state->NextMessageId("health"), "<get/>"), &error);
        if (!operational)
          return error;
        if (ReplyIsOk(*operational) || RpcErrorTag(*operational))
          return "peer rejected operational health read" +
                 (RpcErrorTag(*operational)
                      ? " (" + *RpcErrorTag(*operational) + ")"
                      : "");
        return state->options.verify_replies(*running, *operational);
      },
      .confirm = [state]() -> std::optional<std::string> {
        if (state->confirmed)
          return std::nullopt;
        if (!state->applied || !state->session)
          return "participant has no applied confirmed commit";
        const std::string operation = PersistentCommitOperation(
            "commit", state->options.persistent_commit_id);
        if (const auto error = state->Execute("commit confirmation", operation))
          return error;
        state->confirmed = true;
        state->apply_attempted = false;
        state->applied = false;
        return std::nullopt;
      },
      .cancel = [state]() { return ExecuteCancel(state.get()); },
      .release =
          [state] {
            if (!state->session)
              return;
            if (state->locked) {
              (void)state->Execute(
                  "candidate unlock",
                  "<unlock><target><candidate/></target></unlock>");
              state->locked = false;
            }
            (void)state->Execute("close session", "<close-session/>");
            state->session->Close();
            state->session.reset();
          },
  };
}

std::optional<std::string>
ConfirmPersistentCommitOverTls(const TlsClientOptions &options,
                               const std::string &persistent_commit_id) {
  if (persistent_commit_id.empty() || persistent_commit_id.size() > 256 ||
      persistent_commit_id.find('\0') != std::string::npos)
    return "persistent commit identity is invalid";
  std::string error;
  constexpr std::array<std::string_view, 1> required_capabilities = {
      kConfirmedCommitCapability};
  const auto exchanged =
      ExchangeTlsRpc(options, ConfirmationRpc(persistent_commit_id), &error,
                     required_capabilities);
  if (!exchanged)
    return error;
  if (!ReplyIsOk(exchanged->reply))
    return "peer rejected persistent commit confirmation";
  return std::nullopt;
}

PeerTransactionParticipant
MakeTlsRecoveryParticipant(const PeerJournalParticipant &journal_participant,
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

} // namespace dangd
