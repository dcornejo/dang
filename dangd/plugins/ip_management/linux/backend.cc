// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/platform_backend.h"

#include "dangd/plugins/ip_management/command_runner.h"
#include "dangd/plugins/ip_management/platform_config.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace dangd::ip_management {
namespace {

const InterfaceConfig* Find(const std::vector<InterfaceConfig>& values,
                            const std::string& name) {
  const auto found = std::ranges::find(values, name, &InterfaceConfig::name);
  return found == values.end() ? nullptr : &*found;
}

class LinuxBackend final : public PlatformBackend {
 public:
  bool Reconcile(std::string_view before_xml, std::string_view desired_xml,
                 std::string* error) override {
    std::vector<InterfaceConfig> before;
    std::vector<InterfaceConfig> desired;
    if (!ParsePlatformConfig(before_xml, &before, error) ||
        !ParsePlatformConfig(desired_xml, &desired, error)) return false;

    // Remove stale addresses before adding replacements. `ip address` uses
    // rtnetlink and waits for the kernel acknowledgement before returning.
    for (const InterfaceConfig& old_interface : before) {
      const InterfaceConfig* replacement = Find(desired, old_interface.name);
      for (const AddressConfig& address : old_interface.addresses) {
        if (replacement && std::ranges::find(replacement->addresses, address) !=
                               replacement->addresses.end()) continue;
        if (!RunCommand({"/sbin/ip", address.ipv6 ? "-6" : "-4", "address",
                         "del", address.address + "/" +
                             std::to_string(address.prefix_length),
                         "dev", old_interface.name}, error)) return false;
      }
    }
    for (const InterfaceConfig& interface : desired) {
      const InterfaceConfig* old = Find(before, interface.name);
      for (const AddressConfig& address : interface.addresses) {
        if (old && std::ranges::find(old->addresses, address) !=
                       old->addresses.end()) continue;
        if (!RunCommand({"/sbin/ip", address.ipv6 ? "-6" : "-4", "address",
                         "add", address.address + "/" +
                             std::to_string(address.prefix_length),
                         "dev", interface.name}, error)) return false;
      }
      if (interface.enabled &&
          (!old || old->enabled != interface.enabled) &&
          !RunCommand({"/sbin/ip", "link", "set", "dev", interface.name,
                       *interface.enabled ? "up" : "down"}, error)) return false;
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<PlatformBackend> MakePlatformBackend() {
  return std::make_unique<LinuxBackend>();
}

}  // namespace dangd::ip_management
