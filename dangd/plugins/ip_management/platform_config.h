// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_CONFIG_H_
#define DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_CONFIG_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dangd::ip_management {

struct AddressConfig {
  std::string address;
  unsigned prefix_length = 0;
  bool ipv6 = false;
  auto operator<=>(const AddressConfig&) const = default;
};

struct InterfaceConfig {
  std::string name;
  std::optional<bool> enabled;
  std::vector<AddressConfig> addresses;
};

// Extracts the subset of RFC 8343/8344 configuration implemented by the native
// examples. Unknown model nodes remain the responsibility of other plugins.
bool ParsePlatformConfig(std::string_view xml,
                         std::vector<InterfaceConfig>* interfaces,
                         std::string* error);

}  // namespace dangd::ip_management

#endif  // DANGD_PLUGINS_IP_MANAGEMENT_PLATFORM_CONFIG_H_
