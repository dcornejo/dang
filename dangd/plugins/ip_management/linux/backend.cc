// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/platform_backend.h"
#include "dangd/plugins/ip_management/platform_config.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <linux/if_addr.h>
#include <linux/if_link.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <pugixml.hpp>

namespace dangd::ip_management {
namespace {

constexpr std::size_t kMessageBytes = 1024;

const InterfaceConfig* Find(const std::vector<InterfaceConfig>& values,
                            const std::string& name) {
  const auto found = std::ranges::find(values, name, &InterfaceConfig::name);
  return found == values.end() ? nullptr : &*found;
}

bool AddAttribute(nlmsghdr* header, std::size_t capacity, std::uint16_t type,
                  const void* data, std::size_t size) {
  const std::size_t attribute_size = RTA_LENGTH(size);
  const std::size_t offset = NLMSG_ALIGN(header->nlmsg_len);
  if (offset + RTA_ALIGN(attribute_size) > capacity) return false;
  auto* attribute = reinterpret_cast<rtattr*>(
      reinterpret_cast<std::byte*>(header) + offset);
  attribute->rta_type = type;
  attribute->rta_len = static_cast<unsigned short>(attribute_size);
  if (size) std::memcpy(RTA_DATA(attribute), data, size);
  header->nlmsg_len = static_cast<std::uint32_t>(
      offset + RTA_ALIGN(attribute_size));
  return true;
}

class RouteSocket {
 public:
  RouteSocket() {
    descriptor_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (descriptor_ < 0) {
      error_ = errno;
      return;
    }
    sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (bind(descriptor_, reinterpret_cast<sockaddr*>(&local), sizeof(local))) {
      error_ = errno;
      close(descriptor_);
      descriptor_ = -1;
    }
  }
  ~RouteSocket() {
    if (descriptor_ >= 0) close(descriptor_);
  }
  RouteSocket(const RouteSocket&) = delete;
  RouteSocket& operator=(const RouteSocket&) = delete;

  bool valid(std::string* error) const {
    if (descriptor_ >= 0) return true;
    if (error) *error = "cannot open rtnetlink socket: " +
                        std::string(std::strerror(error_));
    return false;
  }

  bool Request(nlmsghdr* request, std::string_view description,
               std::string* error) {
    request->nlmsg_seq = ++sequence_;
    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    iovec part{request, request->nlmsg_len};
    msghdr message{};
    message.msg_name = &kernel;
    message.msg_namelen = sizeof(kernel);
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    if (sendmsg(descriptor_, &message, 0) < 0) {
      if (error) *error = std::string(description) + ": " +
                          std::strerror(errno);
      return false;
    }
    std::array<std::byte, 8192> response{};
    while (true) {
      sockaddr_nl sender{};
      iovec response_part{response.data(), response.size()};
      msghdr response_message{};
      response_message.msg_name = &sender;
      response_message.msg_namelen = sizeof(sender);
      response_message.msg_iov = &response_part;
      response_message.msg_iovlen = 1;
      const ssize_t received = recvmsg(descriptor_, &response_message, 0);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) {
        if (error) *error = std::string(description) + ": " +
                            (received ? std::strerror(errno)
                                      : "netlink peer closed");
        return false;
      }
      if (sender.nl_pid != 0) continue;
      int remaining = static_cast<int>(received);
      for (auto* header = reinterpret_cast<nlmsghdr*>(response.data());
           NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
        if (header->nlmsg_seq != sequence_ || header->nlmsg_type != NLMSG_ERROR)
          continue;
        if (NLMSG_PAYLOAD(header, 0) < sizeof(nlmsgerr)) {
          if (error) *error = std::string(description) +
                              ": truncated rtnetlink acknowledgement";
          return false;
        }
        const auto* reply =
            reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(header));
        if (!reply->error) return true;
        if (error) *error = std::string(description) + ": " +
                            std::strerror(-reply->error);
        return false;
      }
    }
  }

 private:
  int descriptor_ = -1;
  int error_ = 0;
  std::uint32_t sequence_ = 0;
};

bool InterfaceIndex(std::string_view name, unsigned* index,
                    std::string* error) {
  const std::string copied(name);
  *index = if_nametoindex(copied.c_str());
  if (*index) return true;
  if (error) *error = "cannot resolve interface " + copied + ": " +
                      std::strerror(errno);
  return false;
}

bool AddressRequest(RouteSocket* socket, std::string_view interface,
                    const AddressConfig& address, bool add,
                    std::string* error) {
  unsigned index = 0;
  if (!InterfaceIndex(interface, &index, error)) return false;
  std::array<std::byte, kMessageBytes> storage{};
  auto* header = reinterpret_cast<nlmsghdr*>(storage.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ifaddrmsg));
  header->nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
      (add ? NLM_F_CREATE | NLM_F_EXCL : 0);
  auto* body = reinterpret_cast<ifaddrmsg*>(NLMSG_DATA(header));
  body->ifa_family = address.ipv6 ? AF_INET6 : AF_INET;
  body->ifa_prefixlen = static_cast<unsigned char>(address.prefix_length);
  body->ifa_scope = RT_SCOPE_UNIVERSE;
  body->ifa_index = index;
  std::array<std::byte, sizeof(in6_addr)> binary{};
  const int family = address.ipv6 ? AF_INET6 : AF_INET;
  if (inet_pton(family, address.address.c_str(), binary.data()) != 1) {
    if (error) *error = "invalid address " + address.address;
    return false;
  }
  const std::size_t size = address.ipv6 ? sizeof(in6_addr) : sizeof(in_addr);
  if (!AddAttribute(header, storage.size(), IFA_LOCAL, binary.data(), size) ||
      !AddAttribute(header, storage.size(), IFA_ADDRESS, binary.data(), size)) {
    if (error) *error = "rtnetlink address request is too large";
    return false;
  }
  return socket->Request(header, add ? "add address" : "delete address", error);
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

bool NeighborRequest(RouteSocket* socket, std::string_view interface,
                     const NeighborConfig& neighbor, bool add,
                     std::string* error) {
  unsigned index = 0;
  if (!InterfaceIndex(interface, &index, error)) return false;
  const auto link_layer = EthernetAddress(neighbor.link_layer_address);
  if (!link_layer) {
    if (error) *error = "unsupported link-layer address " +
                        neighbor.link_layer_address;
    return false;
  }
  std::array<std::byte, kMessageBytes> storage{};
  auto* header = reinterpret_cast<nlmsghdr*>(storage.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ndmsg));
  header->nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK |
      (add ? NLM_F_CREATE | NLM_F_REPLACE : 0);
  auto* body = reinterpret_cast<ndmsg*>(NLMSG_DATA(header));
  body->ndm_family = neighbor.ipv6 ? AF_INET6 : AF_INET;
  body->ndm_ifindex = static_cast<int>(index);
  body->ndm_state = NUD_PERMANENT;
  std::array<std::byte, sizeof(in6_addr)> destination{};
  const int family = neighbor.ipv6 ? AF_INET6 : AF_INET;
  if (inet_pton(family, neighbor.address.c_str(), destination.data()) != 1) {
    if (error) *error = "invalid neighbor address " + neighbor.address;
    return false;
  }
  const std::size_t size = neighbor.ipv6 ? sizeof(in6_addr) : sizeof(in_addr);
  if (!AddAttribute(header, storage.size(), NDA_DST, destination.data(), size) ||
      (add && !AddAttribute(header, storage.size(), NDA_LLADDR,
                            link_layer->data(), link_layer->size()))) {
    if (error) *error = "rtnetlink neighbor request is too large";
    return false;
  }
  return socket->Request(header, add ? "add neighbor" : "delete neighbor",
                         error);
}

bool LinkRequest(RouteSocket* socket, std::string_view interface,
                 std::optional<bool> enabled, std::optional<unsigned> mtu,
                 std::string* error) {
  unsigned index = 0;
  if (!InterfaceIndex(interface, &index, error)) return false;
  std::array<std::byte, kMessageBytes> storage{};
  auto* header = reinterpret_cast<nlmsghdr*>(storage.data());
  header->nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
  header->nlmsg_type = RTM_NEWLINK;
  header->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
  auto* body = reinterpret_cast<ifinfomsg*>(NLMSG_DATA(header));
  body->ifi_family = AF_UNSPEC;
  body->ifi_index = static_cast<int>(index);
  if (enabled) {
    body->ifi_change = IFF_UP;
    if (*enabled) body->ifi_flags = IFF_UP;
  }
  if (mtu && !AddAttribute(header, storage.size(), IFLA_MTU, &*mtu,
                           sizeof(*mtu))) {
    if (error) *error = "rtnetlink link request is too large";
    return false;
  }
  return socket->Request(header, "set interface attributes", error);
}

struct LinkSnapshot {
  bool enabled = false;
  bool running = false;
  unsigned mtu = 0;
};

std::optional<LinkSnapshot> ReadLink(std::string_view interface,
                                     std::string* error) {
  const int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    if (error) *error = "cannot open interface ioctl socket: " +
                        std::string(std::strerror(errno));
    return std::nullopt;
  }
  ifreq request{};
  const std::string copied(interface);
  if (copied.size() >= sizeof(request.ifr_name)) {
    close(descriptor);
    if (error) *error = "interface name exceeds the Linux kernel limit";
    return std::nullopt;
  }
  std::memcpy(request.ifr_name, copied.c_str(), copied.size() + 1);
  if (ioctl(descriptor, SIOCGIFFLAGS, &request) != 0) {
    if (error) *error = "cannot read interface flags for " + copied + ": " +
                        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
  const bool enabled = (request.ifr_flags & IFF_UP) != 0;
  const bool running = (request.ifr_flags & IFF_RUNNING) != 0;
  if (ioctl(descriptor, SIOCGIFMTU, &request) != 0) {
    if (error) *error = "cannot read interface MTU for " + copied + ": " +
                        std::strerror(errno);
    close(descriptor);
    return std::nullopt;
  }
  const unsigned mtu = static_cast<unsigned>(request.ifr_mtu);
  close(descriptor);
  return LinkSnapshot{enabled, running, mtu};
}

std::string_view LocalName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

pugi::xml_node Child(const pugi::xml_node& parent, std::string_view local) {
  for (const pugi::xml_node child : parent.children())
    if (LocalName(child.name()) == local) return child;
  return {};
}

unsigned PrefixLength(const sockaddr* mask) {
  if (!mask) return 0;
  const std::byte* bytes = nullptr;
  std::size_t size = 0;
  if (mask->sa_family == AF_INET) {
    bytes = reinterpret_cast<const std::byte*>(
        &reinterpret_cast<const sockaddr_in*>(mask)->sin_addr);
    size = sizeof(in_addr);
  } else if (mask->sa_family == AF_INET6) {
    bytes = reinterpret_cast<const std::byte*>(
        &reinterpret_cast<const sockaddr_in6*>(mask)->sin6_addr);
    size = sizeof(in6_addr);
  }
  unsigned bits = 0;
  for (std::size_t index = 0; index < size; ++index)
    bits += static_cast<unsigned>(__builtin_popcount(
        std::to_integer<unsigned char>(bytes[index])));
  return bits;
}

bool AppendAddresses(const std::string& name, pugi::xml_node entry,
                     unsigned mtu, std::string* error) {
  ifaddrs* values = nullptr;
  if (getifaddrs(&values) != 0) {
    if (error) *error = "cannot enumerate interface addresses: " +
                        std::string(std::strerror(errno));
    return false;
  }
  pugi::xml_node ipv4;
  pugi::xml_node ipv6;
  for (const ifaddrs* value = values; value; value = value->ifa_next) {
    if (!value->ifa_addr || name != value->ifa_name) continue;
    const int family = value->ifa_addr->sa_family;
    if (family != AF_INET && family != AF_INET6) continue;
    pugi::xml_node* family_node = family == AF_INET ? &ipv4 : &ipv6;
    if (!*family_node) {
      *family_node = entry.append_child(family == AF_INET ? "ipv4" : "ipv6");
      // RFC 8344 deliberately gives IPv4 MTU a uint16 representation. Linux
      // loopback commonly reports 65536, so omit that unrepresentable value
      // instead of publishing invalid instance data.
      if (family == AF_INET6 || mtu <= 65535)
        family_node->append_child("mtu").text() = mtu;
    }
    char text[INET6_ADDRSTRLEN]{};
    const void* binary = family == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(
              value->ifa_addr)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(
              value->ifa_addr)->sin6_addr);
    if (!inet_ntop(family, binary, text, sizeof(text))) continue;
    pugi::xml_node address = family_node->append_child("address");
    address.append_child("ip").text() = text;
    address.append_child("prefix-length").text() =
        PrefixLength(value->ifa_netmask);
    address.append_child("origin").text() = "other";
  }
  freeifaddrs(values);
  return true;
}

std::optional<unsigned> Mtu(const InterfaceConfig& interface,
                            std::string* error) {
  if (interface.ipv4_mtu && interface.ipv6_mtu &&
      interface.ipv4_mtu != interface.ipv6_mtu) {
    if (error) *error = "Linux requires equal IPv4 and IPv6 link MTUs on " +
                        interface.name;
    return std::nullopt;
  }
  return interface.ipv4_mtu ? interface.ipv4_mtu : interface.ipv6_mtu;
}

struct Operation {
  enum class Kind { kAddress, kNeighbor, kLink } kind;
  std::string interface;
  std::optional<AddressConfig> address;
  std::optional<NeighborConfig> neighbor;
  std::optional<bool> enabled;
  std::optional<unsigned> mtu;
  bool add = false;

  bool Run(RouteSocket* socket, std::string* error) const {
    if (kind == Kind::kAddress)
      return AddressRequest(socket, interface, *address, add, error);
    if (kind == Kind::kNeighbor)
      return NeighborRequest(socket, interface, *neighbor, add, error);
    return LinkRequest(socket, interface, enabled, mtu, error);
  }
};

class LinuxBackend final : public PlatformBackend {
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
                                 old_interface.name, {}, value, {}, {}, false},
                                {Operation::Kind::kNeighbor,
                                 old_interface.name, {}, value, {}, {}, true}});
      for (const AddressConfig& value : old_interface.addresses)
        if (!replacement || std::ranges::find(replacement->addresses, value) ==
                                replacement->addresses.end())
          operations.push_back({{Operation::Kind::kAddress,
                                 old_interface.name, value, {}, {}, {}, false},
                                {Operation::Kind::kAddress,
                                 old_interface.name, value, {}, {}, {}, true}});
    }
    for (const InterfaceConfig& interface : desired) {
      const InterfaceConfig* old = Find(before, interface.name);
      for (const AddressConfig& value : interface.addresses)
        if (!old || std::ranges::find(old->addresses, value) ==
                        old->addresses.end())
          operations.push_back({{Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, {}, true},
                                {Operation::Kind::kAddress, interface.name,
                                 value, {}, {}, {}, false}});
      for (const NeighborConfig& value : interface.neighbors)
        if (!old || std::ranges::find(old->neighbors, value) ==
                        old->neighbors.end())
          operations.push_back({{Operation::Kind::kNeighbor, interface.name,
                                 {}, value, {}, {}, true},
                                {Operation::Kind::kNeighbor, interface.name,
                                 {}, value, {}, {}, false}});
      const auto desired_mtu = Mtu(interface, error);
      if ((interface.ipv4_mtu || interface.ipv6_mtu) && !desired_mtu)
        return false;
      std::optional<LinkSnapshot> snapshot;
      if (interface.enabled || desired_mtu)
        snapshot = ReadLink(interface.name, error);
      if ((interface.enabled || desired_mtu) && !snapshot) return false;
      const bool enabled_changed = interface.enabled &&
          snapshot->enabled != *interface.enabled;
      const bool mtu_changed = desired_mtu && snapshot->mtu != *desired_mtu;
      if (enabled_changed || mtu_changed)
        operations.push_back(
            {{Operation::Kind::kLink, interface.name, {}, {},
              enabled_changed ? interface.enabled : std::nullopt,
              mtu_changed ? desired_mtu : std::nullopt, false},
             {Operation::Kind::kLink, interface.name, {}, {},
              enabled_changed ? std::optional(snapshot->enabled) : std::nullopt,
              mtu_changed ? std::optional(snapshot->mtu) : std::nullopt, false}});
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

  bool OperationalXml(std::string_view configuration_xml, std::string* output,
                      std::string* error) override {
    pugi::xml_document configuration;
    const pugi::xml_parse_result parsed = configuration.load_buffer(
        configuration_xml.data(), configuration_xml.size());
    if (!parsed) {
      if (error) *error = "cannot parse applied configuration for live state";
      return false;
    }
    pugi::xml_document state;
    pugi::xml_node root = state.append_child("interfaces-state");
    root.append_attribute("xmlns") =
        "urn:ietf:params:xml:ns:yang:ietf-interfaces";
    for (const pugi::xml_node top : configuration.document_element().children()) {
      if (LocalName(top.name()) != "interfaces") continue;
      for (const pugi::xml_node configured : top.children()) {
        if (LocalName(configured.name()) != "interface") continue;
        const pugi::xml_node name_node = Child(configured, "name");
        const pugi::xml_node type_node = Child(configured, "type");
        if (!name_node || !type_node) continue;
        const std::string name = name_node.text().as_string();
        const auto link = ReadLink(name, error);
        if (!link) return false;
        pugi::xml_node entry = root.append_child("interface");
        entry.append_child("name").text() = name.c_str();
        entry.append_child("type").text() = type_node.text().as_string();
        entry.append_child("admin-status").text() =
            link->enabled ? "up" : "down";
        entry.append_child("oper-status").text() =
            link->running ? "up" : (link->enabled ? "dormant" : "down");
        if (!AppendAddresses(name, entry, link->mtu, error)) return false;
      }
    }
    std::ostringstream serialized;
    state.print(serialized, "", pugi::format_raw);
    *output = serialized.str();
    return true;
  }

 private:
  static bool RunOperations(
      const std::vector<std::pair<Operation, Operation>>& operations,
      std::string* error) {
    RouteSocket socket;
    if (!socket.valid(error)) return false;
    std::vector<std::size_t> completed;
    for (std::size_t index = 0; index < operations.size(); ++index) {
      if (operations[index].first.Run(&socket, error)) {
        completed.push_back(index);
        continue;
      }
      const std::string failure = error ? *error : "rtnetlink operation failed";
      std::vector<std::string> rollback_failures;
      for (auto rollback = completed.rbegin(); rollback != completed.rend();
           ++rollback) {
        std::string rollback_error;
        if (!operations[*rollback].second.Run(&socket, &rollback_error))
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
  return std::make_unique<LinuxBackend>();
}

}  // namespace dangd::ip_management
