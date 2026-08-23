// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/configuration_backend.h"
#include "yang/xml_security.h"

#include <sstream>
#include <utility>

#include <pugixml.hpp>

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
    return "Changed " + change.instance_path + " from " +
           Quote(change.before) + " to " + Quote(change.after) + ".";
  }
  if (change.kind == ChangeKind::kMoved) {
    return "Moved " + change.instance_path + " from " +
           change.before.value_or("an unknown position") + " to " +
           change.after.value_or("an unknown position") + ".";
  }
  return "Replaced the configuration subtree at " + change.instance_path +
         ".";
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
      const std::string attribute = "xmlns:" + std::string(name.substr(0, colon));
      namespace_uri = child.attribute(attribute.c_str()).as_string();
    }
    if (namespace_uri != kNamespace) continue;
    std::ostringstream output;
    child.print(output, "  ", pugi::format_raw);
    return output.str();
  }
  return {};
}

}  // namespace

std::optional<yang::config::ValidationFinding>
EnglishConfigurationBackend::PrepareReplacement(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument& before,
    const yang::config::ConfigDocument& after,
    std::span<const yang::config::ChangeEvent> changes) {
  prepared_nacm_.reset();
  if (plugins_) {
    if (auto error = plugins_->Prepare(schema, before, after, changes))
      return error;
  }
  if (managed_nacm_) {
    const std::string xml = ManagedNacmXml(after);
    if (xml.empty()) {
      yang::netconf::NacmPolicy defaults;
      if (nacm_) defaults.PreserveRuntimeStateFrom(*nacm_);
      prepared_nacm_ = std::move(defaults);
      return std::nullopt;
    }
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
      if (plugins_) plugins_->Abort();
      return finding;
    }
    if (nacm_) loaded.policy->PreserveRuntimeStateFrom(*nacm_);
    prepared_nacm_ = std::move(*loaded.policy);
  }
  return std::nullopt;
}

std::optional<yang::config::ValidationFinding>
EnglishConfigurationBackend::Replace(
    const yang::config::RuntimeSchema& schema,
    const yang::config::ConfigDocument&,
    const yang::config::ConfigDocument& after,
    std::span<const yang::config::ChangeEvent> changes) {
  if (plugins_) {
    PluginApplyResult applied = plugins_->Apply(schema, after);
    if (applied.error) return applied.error;
    if (applied.applied) {
      std::lock_guard lock(mutex_);
      for (const auto& change : changes) deltas_.push_back(Describe(change));
      working_ = std::move(*applied.applied);
      if (prepared_nacm_ && nacm_) *nacm_ = std::move(*prepared_nacm_);
      prepared_nacm_.reset();
      return std::nullopt;
    }
  }
  std::lock_guard lock(mutex_);
  for (const auto& change : changes) deltas_.push_back(Describe(change));
  working_ = after;
  if (prepared_nacm_ && nacm_) *nacm_ = std::move(*prepared_nacm_);
  prepared_nacm_.reset();
  return std::nullopt;
}

void EnglishConfigurationBackend::AbortPreparedReplacement() noexcept {
  if (plugins_) plugins_->Abort();
  prepared_nacm_.reset();
}

yang::config::ConfigDocument EnglishConfigurationBackend::Working() const {
  std::lock_guard lock(mutex_);
  return working_;
}

std::vector<std::string> EnglishConfigurationBackend::DrainDeltas() {
  std::lock_guard lock(mutex_);
  std::vector<std::string> result = std::move(deltas_);
  deltas_.clear();
  return result;
}

}  // namespace dangd
