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

std::vector<std::string> AddressCommand(const InterfaceConfig& interface,
                                        const AddressConfig& address,
                                        bool add) {
  // FreeBSD's ifconfig accepts CIDR notation for both families. `alias` and
  // `-alias` change one address without disturbing other addresses on a link.
  return {"/sbin/ifconfig", interface.name, address.ipv6 ? "inet6" : "inet",
          address.address + "/" + std::to_string(address.prefix_length),
          add ? "alias" : "-alias"};
}

class FreeBsdBackend final : public PlatformBackend {
 public:
  bool Reconcile(std::string_view before_xml, std::string_view desired_xml,
                 std::string* error) override {
    std::vector<InterfaceConfig> before;
    std::vector<InterfaceConfig> desired;
    if (!ParsePlatformConfig(before_xml, &before, error) ||
        !ParsePlatformConfig(desired_xml, &desired, error)) return false;
    for (const InterfaceConfig& old_interface : before) {
      const InterfaceConfig* replacement = Find(desired, old_interface.name);
      for (const AddressConfig& address : old_interface.addresses) {
        if (replacement && std::ranges::find(replacement->addresses, address) !=
                               replacement->addresses.end()) continue;
        if (!RunCommand(AddressCommand(old_interface, address, false), error))
          return false;
      }
    }
    for (const InterfaceConfig& interface : desired) {
      const InterfaceConfig* old = Find(before, interface.name);
      for (const AddressConfig& address : interface.addresses) {
        if (old && std::ranges::find(old->addresses, address) !=
                       old->addresses.end()) continue;
        if (!RunCommand(AddressCommand(interface, address, true), error))
          return false;
      }
      if (interface.enabled &&
          (!old || old->enabled != interface.enabled) &&
          !RunCommand({"/sbin/ifconfig", interface.name,
                       *interface.enabled ? "up" : "down"}, error)) return false;
    }
    return true;
  }
};

}  // namespace

std::unique_ptr<PlatformBackend> MakePlatformBackend() {
  return std::make_unique<FreeBsdBackend>();
}

}  // namespace dangd::ip_management
