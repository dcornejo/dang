// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/ssh_transport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>

#include <libssh/libssh.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "dangd/application.h"

namespace dangd {
namespace {

template <typename Type, void (*Release)(Type)>
using Handle = std::unique_ptr<std::remove_pointer_t<Type>, decltype(Release)>;

std::uint16_t AvailableLoopbackPort() {
  const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
      0) {
    close(socket_fd);
    return 0;
  }
  socklen_t length = sizeof(address);
  if (getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length) !=
      0) {
    close(socket_fd);
    return 0;
  }
  close(socket_fd);
  return ntohs(address.sin_port);
}

struct ClientResult {
  bool authenticated = false;
  bool subsystem = false;
  std::string received;
};

ClientResult Connect(const std::filesystem::path& private_key,
                     std::uint16_t port, std::string_view subsystem,
                     bool exchange_netconf) {
  ClientResult result;
  Handle<ssh_session, ssh_free> session(ssh_new(), ssh_free);
  if (!session) return result;
  const char* host = "127.0.0.1";
  const char* user = "alice";
  const unsigned int ssh_port = port;
  if (ssh_options_set(session.get(), SSH_OPTIONS_HOST, host) != SSH_OK ||
      ssh_options_set(session.get(), SSH_OPTIONS_PORT, &ssh_port) != SSH_OK ||
      ssh_options_set(session.get(), SSH_OPTIONS_USER, user) != SSH_OK ||
      ssh_connect(session.get()) != SSH_OK) {
    return result;
  }
  ssh_key key = nullptr;
  if (ssh_pki_import_privkey_file(private_key.c_str(), nullptr, nullptr,
                                  nullptr, &key) != SSH_OK) {
    return result;
  }
  Handle<ssh_key, ssh_key_free> client_key(key, ssh_key_free);
  if (ssh_userauth_publickey(session.get(), nullptr, client_key.get()) !=
      SSH_AUTH_SUCCESS) {
    ssh_disconnect(session.get());
    return result;
  }
  result.authenticated = true;
  Handle<ssh_channel, ssh_channel_free> channel(ssh_channel_new(session.get()),
                                                ssh_channel_free);
  if (!channel || ssh_channel_open_session(channel.get()) != SSH_OK ||
      ssh_channel_request_subsystem(channel.get(),
                                    std::string(subsystem).c_str()) != SSH_OK) {
    channel.reset();
    ssh_disconnect(session.get());
    return result;
  }
  result.subsystem = true;
  if (exchange_netconf) {
    const std::string request =
        "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
        "</capability></capabilities></hello>]]>]]>"
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"ssh\"><get/></rpc>]]>]]>"
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"close\"><close-session/></rpc>]]>]]>";
    (void)ssh_channel_write(channel.get(), request.data(),
                            static_cast<std::uint32_t>(request.size()));
    std::array<char, 16 * 1024> buffer{};
    while (true) {
      const int count = ssh_channel_read_timeout(
          channel.get(), buffer.data(), buffer.size(), 0, 2000);
      if (count <= 0) break;
      result.received.append(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  ssh_channel_close(channel.get());
  channel.reset();
  ssh_disconnect(session.get());
  return result;
}

TEST(DangdSshTransportTest,
     AuthenticatesPublicKeyRequiresNetconfAndExchangesRpc) {
  ASSERT_EQ(ssh_init(), SSH_OK);
  const std::filesystem::path source = DANG_TEST_SOURCE_DIR;
  ApplicationOptions application_options{
      .model = source / "dangd/examples/appliance.yang",
      .search_paths = {source / "dangd/models"},
      .configuration = source / "dangd/examples/config.xml",
      .nacm_configuration = source / "dangd/examples/nacm.xml"};
  auto loaded = Application::Load(application_options);
  ASSERT_NE(loaded.application, nullptr);

  const std::filesystem::path keys = source / "dangd/testdata/ssh";
  const std::uint16_t port = AvailableLoopbackPort();
  ASSERT_NE(port, 0);
  SshServerOptions options{
      .address = "127.0.0.1",
      .port = port,
      .host_key = keys / "host-key",
      .authorized_users = {
          {"alice", keys / "alice-key.pub", {"administrators"}}},
      .username_mappings = {{"alice", "administrator"}},
      .require_username_mapping = true,
      .maximum_connections = 3};
  std::ostringstream diagnostics;
  int server_result = -1;
  std::thread server([&] {
    server_result = RunReloadableSshServer(
        loaded.application, application_options, options, diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const ClientResult unauthorized =
      Connect(keys / "host-key", port, "netconf", false);
  EXPECT_FALSE(unauthorized.authenticated);
  const ClientResult wrong_subsystem =
      Connect(keys / "alice-key", port, "shell", false);
  EXPECT_TRUE(wrong_subsystem.authenticated);
  EXPECT_FALSE(wrong_subsystem.subsystem);
  const ClientResult valid =
      Connect(keys / "alice-key", port, "netconf", true);
  server.join();

  EXPECT_EQ(server_result, 0) << diagnostics.str();
  EXPECT_TRUE(valid.authenticated);
  EXPECT_TRUE(valid.subsystem);
  EXPECT_NE(valid.received.find("<hello"), std::string::npos);
  EXPECT_NE(valid.received.find("message-id=\"ssh\""), std::string::npos);
  EXPECT_EQ(valid.received.find("access-denied"), std::string::npos);
  EXPECT_NE(valid.received.find("message-id=\"close\"><ok/>"),
            std::string::npos);
}

}  // namespace
}  // namespace dangd
