// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/platform_backend.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace dangd::ip_management {
namespace {

const char* TestInterface() {
  return std::getenv("DANG_PRIVILEGED_IP_INTERFACE");
}

int InterfaceMtu(const char* name) {
  const int descriptor = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) return -1;
  ifreq request{};
  std::strncpy(request.ifr_name, name, sizeof(request.ifr_name) - 1);
  const int result = ioctl(descriptor, SIOCGIFMTU, &request);
  close(descriptor);
  return result == 0 ? request.ifr_mtu : -1;
}

bool AddressExists(const char* interface, const char* expected) {
  const int family = std::strchr(expected, ':') ? AF_INET6 : AF_INET;
  ifaddrs* values = nullptr;
  if (getifaddrs(&values) != 0) return false;
  bool found = false;
  for (const ifaddrs* value = values; value; value = value->ifa_next) {
    if (!value->ifa_addr || std::strcmp(interface, value->ifa_name) != 0 ||
        value->ifa_addr->sa_family != family)
      continue;
    char text[INET6_ADDRSTRLEN]{};
    const void* address = family == AF_INET
        ? static_cast<const void*>(&reinterpret_cast<const sockaddr_in*>(
              value->ifa_addr)->sin_addr)
        : static_cast<const void*>(&reinterpret_cast<const sockaddr_in6*>(
              value->ifa_addr)->sin6_addr);
    if (inet_ntop(family, address, text, sizeof(text)) &&
        std::strcmp(text, expected) == 0)
      found = true;
  }
  freeifaddrs(values);
  return found;
}

std::string Configuration(const char* interface, const char* address,
                          int mtu = 0) {
  std::string result = "<config><interfaces><interface><name>";
  result += interface;
  result += "</name><ipv4>";
  if (mtu) result += "<mtu>" + std::to_string(mtu) + "</mtu>";
  if (address && *address) {
    result += "<address><ip>";
    result += address;
    result += "</ip><prefix-length>32</prefix-length></address>";
  }
  result += "</ipv4><ipv6><address><ip>2001:db8::123</ip>"
      "<prefix-length>128</prefix-length></address>";
  if (mtu) result += "<mtu>" + std::to_string(mtu) + "</mtu>";
  result += "</ipv6></interface></interfaces></config>";
  return result;
}

TEST(FreeBsdIpBackendTest, AppliesAndRollsBackAddressAndObservedMtu) {
  const char* interface = TestInterface();
  if (!interface)
    GTEST_SKIP() << "set DANG_PRIVILEGED_IP_INTERFACE to a disposable interface";
  const int original_mtu = InterfaceMtu(interface);
  ASSERT_GT(original_mtu, 576);
  const std::string configured =
      Configuration(interface, "198.51.100.123", original_mtu - 1);
  auto backend = MakePlatformBackend();
  std::string error;
  ASSERT_TRUE(backend->Reconcile("<config/>", configured, &error)) << error;
  EXPECT_TRUE(AddressExists(interface, "198.51.100.123"));
  EXPECT_TRUE(AddressExists(interface, "2001:db8::123"));
  EXPECT_EQ(InterfaceMtu(interface), original_mtu - 1);
  ASSERT_TRUE(backend->Reconcile(configured, "<config/>", &error)) << error;
  EXPECT_FALSE(AddressExists(interface, "198.51.100.123"));
  EXPECT_FALSE(AddressExists(interface, "2001:db8::123"));
  EXPECT_EQ(InterfaceMtu(interface), original_mtu);
}

TEST(FreeBsdIpBackendTest, CompensatesAfterLaterOperationFails) {
  const char* interface = TestInterface();
  if (!interface)
    GTEST_SKIP() << "set DANG_PRIVILEGED_IP_INTERFACE to a disposable interface";
  std::string desired = "<config><interfaces>";
  desired += "<interface><name>" + std::string(interface) +
      "</name><ipv4><address><ip>198.51.100.124</ip>"
      "<prefix-length>32</prefix-length></address></ipv4></interface>";
  desired +=
      "<interface><name>dang-missing0</name><ipv4><address>"
      "<ip>198.51.100.125</ip><prefix-length>32</prefix-length>"
      "</address></ipv4></interface></interfaces></config>";
  auto backend = MakePlatformBackend();
  std::string error;
  EXPECT_FALSE(backend->Reconcile("<config/>", desired, &error));
  EXPECT_NE(error.find("dang-missing0"), std::string::npos) << error;
  EXPECT_FALSE(AddressExists(interface, "198.51.100.124"));
}

}  // namespace
}  // namespace dangd::ip_management
