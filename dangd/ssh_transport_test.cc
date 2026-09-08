// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/ssh_transport.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <libssh/libssh.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

struct ProcessResult {
  int status = -1;
  std::string output;
};

#ifdef DANG_TEST_OPENSSH_CLIENT
class OpenSshPrivateKey {
 public:
  explicit OpenSshPrivateKey(const std::filesystem::path& source) {
    std::error_code error;
    path_ = std::filesystem::temp_directory_path(error) /
            ("dangd-openssh-key-" + std::to_string(getpid()));
    if (error || !std::filesystem::copy_file(
                     source, path_,
                     std::filesystem::copy_options::overwrite_existing,
                     error)) {
      path_.clear();
      return;
    }
    std::filesystem::permissions(
        path_, std::filesystem::perms::owner_read |
                   std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, error);
    if (error) {
      std::filesystem::remove(path_, error);
      path_.clear();
    }
  }

  ~OpenSshPrivateKey() {
    std::error_code ignored;
    if (!path_.empty()) std::filesystem::remove(path_, ignored);
  }

  OpenSshPrivateKey(const OpenSshPrivateKey&) = delete;
  OpenSshPrivateKey& operator=(const OpenSshPrivateKey&) = delete;

  const std::filesystem::path& path() const { return path_; }
  bool valid() const { return !path_.empty(); }

 private:
  std::filesystem::path path_;
};

/** Runs the system OpenSSH client with pipes instead of a shell.
 *
 * Keeping argv explicit makes paths containing whitespace safe and ensures the
 * interoperability test cannot accidentally reinterpret XML as shell syntax.
 * Standard error is deliberately captured with standard output: OpenSSH's
 * authentication and subsystem diagnostics are useful assertion context.
 */
ProcessResult ConnectWithOpenSsh(const std::filesystem::path& private_key,
                                 std::uint16_t port,
                                 std::string_view subsystem,
                                 std::string_view input) {
  int input_pipe[2] = {-1, -1};
  int output_pipe[2] = {-1, -1};
  if (pipe(input_pipe) != 0) return {};
  if (pipe(output_pipe) != 0) {
    close(input_pipe[0]);
    close(input_pipe[1]);
    return {};
  }
  const pid_t child = fork();
  if (child < 0) {
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    return {};
  }
  if (child == 0) {
    (void)dup2(input_pipe[0], STDIN_FILENO);
    (void)dup2(output_pipe[1], STDOUT_FILENO);
    (void)dup2(output_pipe[1], STDERR_FILENO);
    close(input_pipe[0]);
    close(input_pipe[1]);
    close(output_pipe[0]);
    close(output_pipe[1]);
    const std::string port_text = std::to_string(port);
    const std::string key_text = private_key.string();
    const std::string subsystem_text(subsystem);
    execl(DANG_TEST_OPENSSH_CLIENT, DANG_TEST_OPENSSH_CLIENT, "-p",
          port_text.c_str(), "-i", key_text.c_str(), "-o",
          "BatchMode=yes", "-o", "IdentitiesOnly=yes", "-o",
          "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
          "-o", "ConnectTimeout=5", "-s", "alice@127.0.0.1",
          subsystem_text.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  close(input_pipe[0]);
  close(output_pipe[1]);
  std::size_t offset = 0;
  while (offset < input.size()) {
    const ssize_t count =
        write(input_pipe[1], input.data() + offset, input.size() - offset);
    if (count <= 0) break;
    offset += static_cast<std::size_t>(count);
  }
  close(input_pipe[1]);
  ProcessResult result;
  std::array<char, 16 * 1024> buffer{};
  while (true) {
    const ssize_t count = read(output_pipe[0], buffer.data(), buffer.size());
    if (count <= 0) break;
    result.output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  close(output_pipe[0]);
  if (waitpid(child, &result.status, 0) < 0) result.status = -1;
  return result;
}
#endif

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

ClientResult HoldBackpressuredSession(
    const std::filesystem::path& private_key, std::uint16_t port,
    std::promise<void>& ready, std::shared_future<void> release) {
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
      ssh_channel_request_subsystem(channel.get(), "netconf") != SSH_OK) {
    channel.reset();
    ssh_disconnect(session.get());
    return result;
  }
  result.subsystem = true;
  ready.set_value();

  std::string request =
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability></capabilities></hello>]]>]]>";
  // The request bytes fit comfortably in the inbound SSH window, while the
  // complete <get> replies exceed the peer's unread outbound window. This
  // predictably blocks only this server worker in channel output.
  constexpr std::size_t kPipelinedGets = 512;
  for (std::size_t index = 0; index < kPipelinedGets; ++index) {
    request +=
        "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
        "message-id=\"slow-" +
        std::to_string(index) + "\"><get/></rpc>]]>]]>";
  }
  request +=
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"slow-close\"><close-session/></rpc>]]>]]>";
  std::size_t offset = 0;
  while (offset < request.size()) {
    const int count = ssh_channel_write(
        channel.get(), request.data() + offset,
        static_cast<std::uint32_t>(request.size() - offset));
    if (count <= 0) break;
    offset += static_cast<std::size_t>(count);
  }
  release.wait();
  std::array<char, 16 * 1024> buffer{};
  while (true) {
    const int count = ssh_channel_read_timeout(
        channel.get(), buffer.data(), buffer.size(), 0, 5000);
    if (count <= 0) break;
    result.received.append(buffer.data(), static_cast<std::size_t>(count));
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

TEST(DangdSshTransportTest, IndependentOpenSshNegativeAndConcurrentMatrix) {
#ifndef DANG_TEST_OPENSSH_CLIENT
  GTEST_SKIP() << "OpenSSH client not found at configure time";
#else
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
  OpenSshPrivateKey private_key(keys / "alice-key");
  ASSERT_TRUE(private_key.valid());
  const std::uint16_t port = AvailableLoopbackPort();
  ASSERT_NE(port, 0);
  constexpr std::size_t kParallelClients = 4;
  SshServerOptions options{
      .address = "127.0.0.1",
      .port = port,
      .host_key = keys / "host-key",
      .authorized_users = {
          {"alice", keys / "alice-key.pub", {"administrators"}}},
      .username_mappings = {{"alice", "administrator"}},
      .require_username_mapping = true,
      .maximum_connections = 2 + kParallelClients};
  std::ostringstream diagnostics;
  int server_result = -1;
  std::thread server([&] {
    server_result = RunReloadableSshServer(
        loaded.application, application_options, options, diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  const ProcessResult unauthorized =
      ConnectWithOpenSsh(keys / "host-key", port, "netconf", "");
  EXPECT_FALSE(WIFEXITED(unauthorized.status) &&
               WEXITSTATUS(unauthorized.status) == 0)
      << unauthorized.output;
  const ProcessResult wrong_subsystem =
      ConnectWithOpenSsh(private_key.path(), port, "shell", "");
  EXPECT_FALSE(WIFEXITED(wrong_subsystem.status) &&
               WEXITSTATUS(wrong_subsystem.status) == 0)
      << wrong_subsystem.output;

  const std::string request =
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability></capabilities></hello>]]>]]>"
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"openssh\"><get/></rpc>]]>]]>"
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"close\"><close-session/></rpc>]]>]]>";
  std::vector<std::future<ProcessResult>> clients;
  for (std::size_t i = 0; i < kParallelClients; ++i) {
    clients.push_back(std::async(std::launch::async, [&] {
      return ConnectWithOpenSsh(private_key.path(), port, "netconf", request);
    }));
  }
  for (auto& client : clients) {
    const ProcessResult result = client.get();
    // OpenSSH may return 255 after a successful close-session because the
    // NETCONF server closes its channel and connection immediately afterward.
    // The complete, correlated protocol replies below are the success signal.
    EXPECT_TRUE(WIFEXITED(result.status)) << result.output;
    EXPECT_NE(result.output.find("message-id=\"openssh\""), std::string::npos)
        << result.output;
    EXPECT_NE(result.output.find("message-id=\"close\"><ok/>"),
              std::string::npos)
        << result.output;
  }
  server.join();
  EXPECT_EQ(server_result, 0) << diagnostics.str();
#endif
}

TEST(DangdSshTransportTest, SlowReaderDoesNotBlockIndependentSessions) {
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
  constexpr std::size_t kIndependentClients = 4;
  SshServerOptions options{
      .address = "127.0.0.1",
      .port = port,
      .host_key = keys / "host-key",
      .authorized_users = {
          {"alice", keys / "alice-key.pub", {"administrators"}}},
      .username_mappings = {{"alice", "administrator"}},
      .require_username_mapping = true,
      .maximum_connections = 1 + kIndependentClients,
      .maximum_concurrent_sessions = 1 + kIndependentClients};
  std::ostringstream diagnostics;
  int server_result = -1;
  std::thread server([&] {
    server_result = RunReloadableSshServer(
        loaded.application, application_options, options, diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::promise<void> slow_ready;
  std::future<void> ready = slow_ready.get_future();
  std::promise<void> release_slow;
  const std::shared_future<void> release = release_slow.get_future().share();
  auto slow = std::async(std::launch::async, [&] {
    return HoldBackpressuredSession(keys / "alice-key", port, slow_ready,
                                    release);
  });
  EXPECT_EQ(ready.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::vector<std::future<ClientResult>> independent;
  for (std::size_t index = 0; index < kIndependentClients; ++index) {
    independent.push_back(std::async(std::launch::async, [&] {
      return Connect(keys / "alice-key", port, "netconf", true);
    }));
  }
  for (auto& client : independent) {
    EXPECT_EQ(client.wait_for(std::chrono::seconds(5)),
              std::future_status::ready)
        << "an unread SSH peer stalled an independent NETCONF session";
  }
  release_slow.set_value();
  for (auto& client : independent) {
    const ClientResult result = client.get();
    EXPECT_NE(result.received.find("message-id=\"close\"><ok/>"),
              std::string::npos);
  }
  const ClientResult slow_result = slow.get();
  server.join();
  EXPECT_TRUE(slow_result.authenticated);
  EXPECT_TRUE(slow_result.subsystem);
  EXPECT_NE(slow_result.received.find("message-id=\"slow-close\"><ok/>"),
            std::string::npos);
  EXPECT_EQ(server_result, 0) << diagnostics.str();
}

}  // namespace
}  // namespace dangd
