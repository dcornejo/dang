// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_recovery_config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <set>
#include <string_view>

#include "dangd/peer_identity.h"

namespace dangd {
namespace {

constexpr int kConfigurationVersion = 2;
constexpr std::size_t kMaximumConfigurationBytes = 1024 * 1024;
constexpr std::size_t kMaximumPathBytes = 4096;
constexpr std::uint32_t kMaximumTimeoutMilliseconds = 10 * 60 * 1000;

std::optional<std::string> ReadPrivateFile(const std::filesystem::path& path,
                                           std::string* contents) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return "cannot open peer recovery configuration";
  struct stat status{};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      (status.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      status.st_uid != geteuid()) {
    close(descriptor);
    return "peer recovery configuration is not a private owned regular file";
  }
  char buffer[4096];
  while (true) {
    const ssize_t count = read(descriptor, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) {
      close(descriptor);
      return "cannot read peer recovery configuration";
    }
    if (count == 0) break;
    if (contents->size() + static_cast<std::size_t>(count) >
        kMaximumConfigurationBytes) {
      close(descriptor);
      return "peer recovery configuration exceeds the byte limit";
    }
    contents->append(buffer, static_cast<std::size_t>(count));
  }
  if (close(descriptor) != 0) return "cannot close peer recovery configuration";
  return std::nullopt;
}

bool HasOnlyKeys(const nlohmann::json& value,
                 const std::set<std::string_view>& allowed) {
  if (!value.is_object()) return false;
  for (auto member = value.begin(); member != value.end(); ++member)
    if (!allowed.contains(member.key())) return false;
  return true;
}

bool IsBoundedText(const std::string& value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum &&
         value.find('\0') == std::string::npos;
}

bool IsHost(const std::string& value) {
  if (!IsBoundedText(value, 253)) return false;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte <= 0x20 || byte >= 0x7f) return false;
  }
  return true;
}

std::optional<std::filesystem::path> CredentialPath(
    const nlohmann::json& peer, std::string_view key,
    const std::filesystem::path& directory) {
  const auto found = peer.find(std::string(key));
  if (found == peer.end() || !found->is_string()) return std::nullopt;
  const std::string value = found->get<std::string>();
  if (!IsBoundedText(value, kMaximumPathBytes)) return std::nullopt;
  std::filesystem::path path(value);
  if (path.is_relative()) path = directory / path;
  return path.lexically_normal();
}

}  // namespace

std::optional<std::vector<PeerRecoveryTarget>> LoadPeerRecoveryConfig(
    const std::filesystem::path& path, std::string* error) {
  if (error == nullptr) return std::nullopt;
  error->clear();
  if (path.empty()) {
    *error = "peer recovery configuration path is empty";
    return std::nullopt;
  }
  std::string contents;
  if (const auto read_error = ReadPrivateFile(path, &contents)) {
    *error = *read_error;
    return std::nullopt;
  }

  try {
    const nlohmann::json root = nlohmann::json::parse(contents);
    static const std::set<std::string_view> root_keys = {"version", "peers"};
    if (!HasOnlyKeys(root, root_keys) || !root.contains("version") ||
        !root["version"].is_number_integer() ||
        root["version"].get<int>() != kConfigurationVersion ||
        !root.contains("peers") || !root["peers"].is_array() ||
        root["peers"].empty() || root["peers"].size() > 256) {
      *error = "peer recovery configuration has an invalid envelope";
      return std::nullopt;
    }

    static const std::set<std::string_view> peer_keys = {
        "group-id",    "participant-id", "host",         "port",
        "certificate", "private-key",    "trust-anchor", "timeout-ms"};
    std::set<std::string> identities;
    std::vector<PeerRecoveryTarget> targets;
    targets.reserve(root["peers"].size());
    const std::filesystem::path directory = path.parent_path();
    for (const nlohmann::json& peer : root["peers"]) {
      if (!HasOnlyKeys(peer, peer_keys) || !peer.contains("group-id") ||
          !peer["group-id"].is_string() ||
          !peer.contains("participant-id") ||
          !peer["participant-id"].is_string() || !peer.contains("host") ||
          !peer["host"].is_string() || !peer.contains("port") ||
          !peer["port"].is_number_unsigned()) {
        *error = "peer recovery target has invalid fields";
        return std::nullopt;
      }
      const std::string group_id = peer["group-id"].get<std::string>();
      const std::string participant_id =
          peer["participant-id"].get<std::string>();
      const std::string host = peer["host"].get<std::string>();
      const std::uint64_t port = peer["port"].get<std::uint64_t>();
      const auto certificate = CredentialPath(peer, "certificate", directory);
      const auto private_key = CredentialPath(peer, "private-key", directory);
      const auto trust_anchor = CredentialPath(peer, "trust-anchor", directory);
      const PeerRecoveryTarget identity{.group_id = group_id,
                                        .participant_id = participant_id};
      if (!IsValidPeerIdentityComponent(group_id) ||
          !IsValidPeerIdentityComponent(participant_id) ||
          !identities.insert(PeerRecoveryTargetId(identity)).second ||
          !IsHost(host) || port == 0 || port > UINT16_MAX || !certificate ||
          !private_key || !trust_anchor) {
        *error =
            "peer recovery target identity, endpoint, or credential path "
            "is invalid";
        return std::nullopt;
      }
      std::uint32_t timeout = 10'000;
      if (const auto found = peer.find("timeout-ms"); found != peer.end()) {
        if (!found->is_number_unsigned()) {
          *error = "peer recovery target timeout is invalid";
          return std::nullopt;
        }
        const std::uint64_t parsed = found->get<std::uint64_t>();
        if (parsed == 0 || parsed > kMaximumTimeoutMilliseconds) {
          *error = "peer recovery target timeout is outside the safe range";
          return std::nullopt;
        }
        timeout = static_cast<std::uint32_t>(parsed);
      }
      targets.push_back({.group_id = group_id,
                         .participant_id = participant_id,
                         .transport = {.host = host,
                                       .port = static_cast<std::uint16_t>(port),
                                       .certificate = *certificate,
                                       .private_key = *private_key,
                                       .trust_anchor = *trust_anchor,
                                       .timeout_milliseconds = timeout}});
    }
    return targets;
  } catch (const nlohmann::json::exception&) {
    *error = "peer recovery configuration is not valid JSON";
    return std::nullopt;
  }
}

std::string PeerRecoveryTargetId(const PeerRecoveryTarget& target) {
  return PeerIdentity(target.group_id, target.participant_id);
}

}  // namespace dangd
