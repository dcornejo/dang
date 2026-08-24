// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/platform_backend.h"
#include "dangd/plugins/ip_management/platform_config.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <net/if.h>
#include <netlink/netlink.h>
#include <netlink/route/common.h>
#include <netlink/route/neigh.h>
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dangd::ip_management {
namespace {

const InterfaceConfig* Find(const std::vector<InterfaceConfig>& values,
                            const std::string& name) {
  const auto found = std::ranges::find(values, name, &InterfaceConfig::name);
  return found == values.end() ? nullptr : &*found;
}

bool CopyName(std::string_view name, char (&destination)[IFNAMSIZ],
              std::string* error) {
  if (name.empty() || name.size() >= sizeof(destination)) {
    if (error) *error = "interface name exceeds the FreeBSD kernel limit";
    return false;
  }
  std::memcpy(destination, name.data(), name.size());
  destination[name.size()] = '\0';
  return true;
}

class IoctlSocket {
 public:
  explicit IoctlSocket(int family) {
    descriptor_ = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (descriptor_ < 0) error_ = errno;
  }
  ~IoctlSocket() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  IoctlSocket(const IoctlSocket&) = delete;
  IoctlSocket& operator=(const IoctlSocket&) = delete;

  bool valid(std::string* error) const {
    if (descriptor_ >= 0) return true;
    if (error) *error = "cannot open interface ioctl socket: " +
                        std::string(std::strerror(error_));
    return false;
  }

  bool Call(unsigned long request, void* argument,
            std::string_view description, std::string* error) const {
    if (ioctl(descriptor_, request, argument) == 0) return true;
    if (error) *error = std::string(description) + ": " +
                        std::strerror(errno);
    return false;
  }

 private:
  int descriptor_ = -1;
  int error_ = 0;
};

sockaddr_in Ipv4Address(std::string_view text, std::string* error) {
  sockaddr_in result{};
  result.sin_len = sizeof(result);
  result.sin_family = AF_INET;
  const std::string copied(text);
  if (inet_pton(AF_INET, copied.c_str(), &result.sin_addr) != 1 && error)
    *error = "invalid IPv4 address " + copied;
  return result;
}

sockaddr_in Ipv4Mask(unsigned prefix) {
  sockaddr_in result{};
  result.sin_len = sizeof(result);
  result.sin_family = AF_INET;
  const std::uint32_t bits = prefix == 0 ? 0 :
      UINT32_MAX << static_cast<unsigned>(32 - prefix);
  result.sin_addr.s_addr = htonl(bits);
  return result;
}

sockaddr_in6 Ipv6Address(std::string_view text, std::string* error) {
  sockaddr_in6 result{};
  result.sin6_len = sizeof(result);
  result.sin6_family = AF_INET6;
  const std::string copied(text);
  if (inet_pton(AF_INET6, copied.c_str(), &result.sin6_addr) != 1 && error)
    *error = "invalid IPv6 address " + copied;
  return result;
}

sockaddr_in6 Ipv6Mask(unsigned prefix) {
  sockaddr_in6 result{};
  result.sin6_len = sizeof(result);
  result.sin6_family = AF_INET6;
  unsigned remaining = prefix;
  for (std::uint8_t& byte : result.sin6_addr.s6_addr) {
    if (remaining >= 8) {
      byte = UINT8_MAX;
      remaining -= 8;
    } else if (remaining != 0) {
      byte = static_cast<std::uint8_t>(UINT8_MAX << (8 - remaining));
      remaining = 0;
    }
  }
  return result;
}

bool AddressRequest(std::string_view interface, const AddressConfig& address,
                    bool add, std::string* error) {
  IoctlSocket socket(address.ipv6 ? AF_INET6 : AF_INET);
  if (!socket.valid(error)) return false;
  const std::string target = std::string(interface);
  if (address.ipv6) {
    if (add) {
      in6_aliasreq request{};
      if (!CopyName(interface, request.ifra_name, error)) return false;
      request.ifra_addr = Ipv6Address(address.address, error);
      request.ifra_prefixmask = Ipv6Mask(address.prefix_length);
      request.ifra_lifetime.ia6t_vltime = UINT32_MAX;
      request.ifra_lifetime.ia6t_pltime = UINT32_MAX;
      return socket.Call(SIOCAIFADDR_IN6, &request,
                         "add IPv6 address on " + target, error);
    }
    in6_ifreq request{};
    if (!CopyName(interface, request.ifr_name, error)) return false;
    request.ifr_addr = Ipv6Address(address.address, error);
    return socket.Call(SIOCDIFADDR_IN6, &request,
                       "delete IPv6 address on " + target, error);
  }
  if (add) {
    ifaliasreq request{};
    if (!CopyName(interface, request.ifra_name, error)) return false;
    const sockaddr_in value = Ipv4Address(address.address, error);
    const sockaddr_in mask = Ipv4Mask(address.prefix_length);
    std::memcpy(&request.ifra_addr, &value, sizeof(value));
    std::memcpy(&request.ifra_mask, &mask, sizeof(mask));
    return socket.Call(SIOCAIFADDR, &request,
                       "add IPv4 address on " + target, error);
  }
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return false;
  const sockaddr_in value = Ipv4Address(address.address, error);
  std::memcpy(&request.ifr_addr, &value, sizeof(value));
  return socket.Call(SIOCDIFADDR, &request,
                     "delete IPv4 address on " + target, error);
}

std::optional<std::array<std::byte, 6>> EthernetAddress(
    std::string_view text) {
  std::array<std::byte, 6> result{};
  unsigned values[6]{};
  char trailing = 0;
  const std::string copied(text);
  if (std::sscanf(copied.c_str(), "%x:%x:%x:%x:%x:%x%c", &values[0],
                  &values[1], &values[2], &values[3], &values[4], &values[5],
                  &trailing) != 6)
    return std::nullopt;
  for (std::size_t index = 0; index < result.size(); ++index) {
    if (values[index] > 255) return std::nullopt;
    result[index] = static_cast<std::byte>(values[index]);
  }
  return result;
}

bool AddNetlinkAttribute(nlmsghdr* header, std::size_t capacity,
                         std::uint16_t type, const void* data,
                         std::size_t size) {
  const std::size_t attribute_size = NLA_HDRLEN + size;
  const std::size_t offset = NLMSG_ALIGN(header->nlmsg_len);
  if (offset + NLA_ALIGN(attribute_size) > capacity) return false;
  auto* attribute = reinterpret_cast<nlattr*>(
      reinterpret_cast<std::byte*>(header) + offset);
  attribute->nla_type = type;
  attribute->nla_len = static_cast<std::uint16_t>(attribute_size);
  if (size) std::memcpy(attribute + 1, data, size);
  header->nlmsg_len = static_cast<std::uint32_t>(
      offset + NLA_ALIGN(attribute_size));
  return true;
}

bool NeighborRequest(std::string_view interface,
                     const NeighborConfig& neighbor, bool add,
                     std::string* error) {
  const std::string target(interface);
  const unsigned index = if_nametoindex(target.c_str());
  if (index == 0) {
    if (error) *error = "resolve interface " + target + ": " +
                        std::strerror(errno);
    return false;
  }
  const auto link_layer = EthernetAddress(neighbor.link_layer_address);
  if (!link_layer) {
    if (error) *error = "unsupported link-layer address " +
                        neighbor.link_layer_address + " on " + target;
    return false;
  }
  std::array<std::byte, sizeof(in6_addr)> destination{};
  const int family = neighbor.ipv6 ? AF_INET6 : AF_INET;
  if (inet_pton(family, neighbor.address.c_str(), destination.data()) != 1) {
    if (error) *error = "invalid neighbor address " + neighbor.address +
                        " on " + target;
    return false;
  }

  const int descriptor = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC,
                                NETLINK_ROUTE);
  if (descriptor < 0) {
    if (error) *error = "open FreeBSD route netlink socket: " +
                        std::string(std::strerror(errno));
    return false;
  }
  sockaddr_nl local{};
  local.nl_len = sizeof(local);
  local.nl_family = AF_NETLINK;
  if (bind(descriptor, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
    if (error) *error = "bind FreeBSD route netlink socket: " +
                        std::string(std::strerror(errno));
    close(descriptor);
    return false;
  }
  std::array<std::byte, 1024> request{};
  auto* header = reinterpret_cast<nlmsghdr*>(request.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
  header->nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
      (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
  header->nlmsg_seq = 1;
  auto* body = reinterpret_cast<ndmsg*>(NLMSG_DATA(header));
  body->ndm_family = static_cast<std::uint8_t>(family);
  body->ndm_ifindex = static_cast<std::int32_t>(index);
  body->ndm_state = NUD_PERMANENT;
  const std::size_t destination_size = neighbor.ipv6 ? sizeof(in6_addr) :
                                                        sizeof(in_addr);
  const bool attributes_added =
      AddNetlinkAttribute(header, request.size(), NDA_DST, destination.data(),
                          destination_size) &&
      (!add || AddNetlinkAttribute(header, request.size(), NDA_LLADDR,
                                    link_layer->data(), link_layer->size()));
  sockaddr_nl kernel{};
  kernel.nl_len = sizeof(kernel);
  kernel.nl_family = AF_NETLINK;
  if (!attributes_added ||
      sendto(descriptor, header, header->nlmsg_len, 0,
             reinterpret_cast<sockaddr*>(&kernel), sizeof(kernel)) < 0) {
    if (error) *error = "send FreeBSD neighbor request for " + neighbor.address +
                        " on " + target + ": " + std::strerror(errno);
    close(descriptor);
    return false;
  }
  std::array<std::byte, 4096> response{};
  ssize_t received = -1;
  do received = recv(descriptor, response.data(), response.size(), 0);
  while (received < 0 && errno == EINTR);
  const int receive_error = errno;
  close(descriptor);
  if (received < static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr)))) {
    if (error) *error = std::string(add ? "add" : "delete") + " neighbor " +
        neighbor.address + " on " + target + ": " +
        (received < 0 ? std::strerror(receive_error) :
                        "truncated netlink reply");
    return false;
  }
  const auto* reply_header =
      reinterpret_cast<const nlmsghdr*>(response.data());
  if (reply_header->nlmsg_type != NLMSG_ERROR ||
      reply_header->nlmsg_seq != header->nlmsg_seq) {
    if (error) *error = "unexpected FreeBSD neighbor acknowledgement for " +
                        neighbor.address + " on " + target;
    return false;
  }
  const auto* reply =
      reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(reply_header));
  if (reply->error == 0) return true;
  if (error) *error = std::string(add ? "add" : "delete") + " neighbor " +
      neighbor.address + " on " + target + ": " +
      std::strerror(reply->error);
  return false;
}

struct LinkSnapshot {
  bool enabled = false;
  unsigned mtu = 0;
};

std::optional<LinkSnapshot> ReadLink(std::string_view interface,
                                     std::string* error) {
  IoctlSocket socket(AF_INET);
  if (!socket.valid(error)) return std::nullopt;
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return std::nullopt;
  if (!socket.Call(SIOCGIFFLAGS, &request,
                   "read flags on " + std::string(interface), error))
    return std::nullopt;
  const bool enabled = (request.ifr_flags & IFF_UP) != 0;
  if (!socket.Call(SIOCGIFMTU, &request,
                   "read MTU on " + std::string(interface), error))
    return std::nullopt;
  return LinkSnapshot{enabled, static_cast<unsigned>(request.ifr_mtu)};
}

bool MtuRequest(std::string_view interface, unsigned mtu, std::string* error) {
  IoctlSocket socket(AF_INET);
  if (!socket.valid(error)) return false;
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return false;
  request.ifr_mtu = static_cast<int>(mtu);
  return socket.Call(SIOCSIFMTU, &request,
                     "set MTU on " + std::string(interface), error);
}

bool EnabledRequest(std::string_view interface, bool enabled,
                    std::string* error) {
  IoctlSocket socket(AF_INET);
  if (!socket.valid(error)) return false;
  ifreq request{};
  if (!CopyName(interface, request.ifr_name, error)) return false;
  if (!socket.Call(SIOCGIFFLAGS, &request,
                   "read flags on " + std::string(interface), error))
    return false;
  if (enabled) request.ifr_flags |= IFF_UP;
  else request.ifr_flags &= static_cast<short>(~IFF_UP);
  return socket.Call(SIOCSIFFLAGS, &request,
                     "set flags on " + std::string(interface), error);
}

std::optional<unsigned> Mtu(const InterfaceConfig& interface,
                            std::string* error) {
  if (interface.ipv4_mtu && interface.ipv6_mtu &&
      interface.ipv4_mtu != interface.ipv6_mtu) {
    if (error) *error = "FreeBSD requires equal IPv4 and IPv6 link MTUs on " +
                        interface.name;
    return std::nullopt;
  }
  const auto result = interface.ipv4_mtu ? interface.ipv4_mtu :
                                           interface.ipv6_mtu;
  if (result && *result > static_cast<unsigned>(INT_MAX)) {
    if (error) *error = "MTU exceeds the FreeBSD kernel integer range on " +
                        interface.name;
    return std::nullopt;
  }
  return result;
}

struct Operation {
  enum class Kind { kAddress, kEnabled, kMtu, kNeighbor } kind;
  std::string interface;
  std::optional<AddressConfig> address;
  std::optional<bool> enabled;
  std::optional<unsigned> mtu;
  bool add = false;
  std::optional<NeighborConfig> neighbor;

  bool Run(std::string* error) const {
    if (kind == Kind::kAddress)
      return AddressRequest(interface, *address, add, error);
    if (kind == Kind::kEnabled)
      return EnabledRequest(interface, *enabled, error);
    if (kind == Kind::kNeighbor)
      return NeighborRequest(interface, *neighbor, add, error);
    return MtuRequest(interface, *mtu, error);
  }
};

class FreeBsdBackend final : public PlatformBackend {
 public:
  bool Reconcile(std::string_view before_xml, std::string_view desired_xml,
                 std::string* error) override {
    if (before_xml == rollback_before_ && desired_xml == rollback_desired_) {
      const bool restored = RunOperations(rollback_operations_, error);
      if (restored) Commit();
      return restored;
    }
    std::vector<InterfaceConfig> before;
    std::vector<InterfaceConfig> desired;
    if (!ParsePlatformConfig(before_xml, &before, error) ||
        !ParsePlatformConfig(desired_xml, &desired, error))
      return false;
    std::vector<std::pair<Operation, Operation>> operations;
    for (const InterfaceConfig& old_interface : before) {
      const InterfaceConfig* replacement = Find(desired, old_interface.name);
      for (const NeighborConfig& value : old_interface.neighbors)
        if (!replacement || std::ranges::find(replacement->neighbors, value) ==
                                replacement->neighbors.end())
          operations.push_back({{Operation::Kind::kNeighbor,
                                 old_interface.name, {}, {}, {}, false, value},
                                {Operation::Kind::kNeighbor,
                                 old_interface.name, {}, {}, {}, true, value}});
      for (const AddressConfig& value : old_interface.addresses)
        if (!replacement || std::ranges::find(replacement->addresses, value) ==
                                replacement->addresses.end())
          operations.push_back({{Operation::Kind::kAddress,
                                 old_interface.name, value, {}, {}, false, {}},
                                {Operation::Kind::kAddress,
                                 old_interface.name, value, {}, {}, true, {}}});
    }
    for (const InterfaceConfig& interface : desired) {
      const InterfaceConfig* old = Find(before, interface.name);
      for (const AddressConfig& value : interface.addresses)
        if (!old || std::ranges::find(old->addresses, value) ==
                        old->addresses.end())
          operations.push_back({{Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, true, {}},
                                {Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, false, {}}});
      for (const NeighborConfig& value : interface.neighbors)
        if (!old || std::ranges::find(old->neighbors, value) ==
                        old->neighbors.end())
          operations.push_back({{Operation::Kind::kNeighbor, interface.name,
                                 {}, {}, {}, true, value},
                                {Operation::Kind::kNeighbor, interface.name,
                                 {}, {}, {}, false, value}});
      const auto desired_mtu = Mtu(interface, error);
      if ((interface.ipv4_mtu || interface.ipv6_mtu) && !desired_mtu)
        return false;
      const bool needs_link = interface.enabled || desired_mtu;
      const auto snapshot = needs_link ? ReadLink(interface.name, error) :
                                         std::optional<LinkSnapshot>{};
      if (needs_link && !snapshot) return false;
      const bool enabled_changed = interface.enabled &&
          snapshot->enabled != *interface.enabled;
      const bool mtu_changed = desired_mtu && snapshot->mtu != *desired_mtu;
      if (mtu_changed)
        operations.push_back(
            {{Operation::Kind::kMtu, interface.name, {}, {}, desired_mtu, false,
              {}},
             {Operation::Kind::kMtu, interface.name, {}, {}, snapshot->mtu,
              false, {}}});
      if (enabled_changed) {
        std::pair<Operation, Operation> change{
            {Operation::Kind::kEnabled, interface.name, {}, interface.enabled,
             {}, false, {}},
            {Operation::Kind::kEnabled, interface.name, {}, snapshot->enabled,
             {}, false, {}}};
        if (*interface.enabled) operations.push_back(std::move(change));
        else operations.insert(operations.begin(), std::move(change));
      }
    }
    if (!RunOperations(operations, error)) return false;
    rollback_before_ = std::string(desired_xml);
    rollback_desired_ = std::string(before_xml);
    rollback_operations_.clear();
    rollback_operations_.reserve(operations.size());
    for (auto operation = operations.rbegin(); operation != operations.rend();
         ++operation)
      rollback_operations_.push_back({operation->second, operation->first});
    return true;
  }

  void Commit() override {
    rollback_before_.clear();
    rollback_desired_.clear();
    rollback_operations_.clear();
  }

 private:
  static bool RunOperations(
      const std::vector<std::pair<Operation, Operation>>& operations,
      std::string* error) {
    std::vector<std::size_t> completed;
    for (std::size_t index = 0; index < operations.size(); ++index) {
      if (operations[index].first.Run(error)) {
        completed.push_back(index);
        continue;
      }
      const std::string failure = error ? *error : "interface operation failed";
      std::vector<std::string> rollback_failures;
      for (auto rollback = completed.rbegin(); rollback != completed.rend();
           ++rollback) {
        std::string rollback_error;
        if (!operations[*rollback].second.Run(&rollback_error))
          rollback_failures.push_back(std::move(rollback_error));
      }
      if (error) {
        *error = failure;
        if (!rollback_failures.empty()) {
          *error += "; rollback incomplete: ";
          for (std::size_t failure_index = 0;
               failure_index < rollback_failures.size(); ++failure_index) {
            if (failure_index) *error += ", ";
            *error += rollback_failures[failure_index];
          }
        }
      }
      return false;
    }
    return true;
  }

  std::string rollback_before_;
  std::string rollback_desired_;
  std::vector<std::pair<Operation, Operation>> rollback_operations_;
};

}  // namespace

std::unique_ptr<PlatformBackend> MakePlatformBackend() {
  return std::make_unique<FreeBsdBackend>();
}

}  // namespace dangd::ip_management
