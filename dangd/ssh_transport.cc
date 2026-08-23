// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/ssh_transport.h"

#include "dangd/application.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <libssh/libssh.h>
#include <libssh/server.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace dangd {
namespace {

template <typename Type, void (*Release)(Type)>
using Handle = std::unique_ptr<std::remove_pointer_t<Type>, decltype(Release)>;

volatile std::sig_atomic_t reload_requested = 0;
void RequestReload(int) { reload_requested = 1; }

struct AuthorizedIdentity {
  Handle<ssh_key, ssh_key_free> key{nullptr, ssh_key_free};
  std::vector<std::string> external_groups;
};

class LibsshStream final : public yang::netconf::SecureByteStream {
 public:
  explicit LibsshStream(ssh_channel channel) : channel_(channel) {}

  yang::netconf::WriteStatus Write(std::string_view bytes) override {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const std::size_t remaining = bytes.size() - offset;
      const std::uint32_t chunk = static_cast<std::uint32_t>(std::min<std::size_t>(
          remaining, std::numeric_limits<std::uint32_t>::max()));
      const int written = ssh_channel_write(channel_, bytes.data() + offset,
                                            chunk);
      if (written == SSH_ERROR) return yang::netconf::WriteStatus::kFailed;
      if (written == 0) return yang::netconf::WriteStatus::kClosed;
      offset += static_cast<std::size_t>(written);
    }
    return yang::netconf::WriteStatus::kAccepted;
  }

  void Close() override {
    if (!channel_) return;
    ssh_channel_send_eof(channel_);
    ssh_channel_close(channel_);
    channel_ = nullptr;
  }

 private:
  ssh_channel channel_;
};

bool LoadAuthorization(
    const SshServerOptions& options,
    std::map<std::string, AuthorizedIdentity>* authorization,
    std::ostream& diagnostics) {
  if (options.authorized_users.empty()) {
    diagnostics << "dangd: SSH requires at least one authorized public key\n";
    return false;
  }
  for (const SshAuthorizedUser& user : options.authorized_users) {
    if (user.username.empty() || authorization->contains(user.username)) {
      diagnostics << "dangd: SSH authorized usernames must be nonempty and "
                     "unique\n";
      return false;
    }
    ssh_key key = nullptr;
    if (ssh_pki_import_pubkey_file(user.public_key.c_str(), &key) != SSH_OK) {
      diagnostics << "dangd: cannot load SSH public key for " << user.username
                  << ": " << user.public_key << '\n';
      return false;
    }
    authorization->emplace(
        user.username,
        AuthorizedIdentity{Handle<ssh_key, ssh_key_free>(key, ssh_key_free),
                           user.external_groups});
  }
  return true;
}

std::optional<std::string> Authenticate(
    ssh_session session,
    const std::map<std::string, AuthorizedIdentity>& authorization) {
  ssh_set_auth_methods(session, SSH_AUTH_METHOD_PUBLICKEY);
  constexpr std::size_t kMaximumAuthenticationRequests = 6;
  std::size_t authentication_requests = 0;
  while (ssh_is_connected(session) &&
         authentication_requests < kMaximumAuthenticationRequests) {
    ssh_message message = ssh_message_get(session);
    if (!message) return std::nullopt;
    std::optional<std::string> authenticated;
    bool replied = false;
    if (ssh_message_type(message) == SSH_REQUEST_AUTH &&
        ssh_message_subtype(message) == SSH_AUTH_METHOD_PUBLICKEY) {
      ++authentication_requests;
      const char* username = ssh_message_auth_user(message);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
      // libssh 0.12's message-mode server API still exposes the offered key
      // and verified-signature state through these deprecated accessors. The
      // callback API requires converting the complete host to ssh_event; keep
      // this compatibility boundary narrow until that event-loop migration.
      ssh_key supplied = ssh_message_auth_pubkey(message);
      const auto found = username ? authorization.find(username)
                                  : authorization.end();
      if (found != authorization.end() && supplied &&
          ssh_key_cmp(supplied, found->second.key.get(), SSH_KEY_CMP_PUBLIC) ==
              0) {
        const auto state = ssh_message_auth_publickey_state(message);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
        if (state == SSH_PUBLICKEY_STATE_NONE) {
          (void)ssh_message_auth_reply_pk_ok_simple(message);
          replied = true;
        } else if (state == SSH_PUBLICKEY_STATE_VALID &&
                   ssh_message_auth_reply_success(message, 0) == SSH_OK) {
          authenticated = found->first;
          replied = true;
        }
      }
      if (!replied) {
        (void)ssh_message_auth_set_methods(message, SSH_AUTH_METHOD_PUBLICKEY);
        (void)ssh_message_reply_default(message);
      }
    } else {
      (void)ssh_message_reply_default(message);
    }
    ssh_message_free(message);
    if (authenticated) return authenticated;
  }
  return std::nullopt;
}

ssh_channel AcceptNetconfSubsystem(ssh_session session) {
  ssh_channel channel = nullptr;
  bool subsystem = false;
  constexpr std::size_t kMaximumChannelRequests = 16;
  std::size_t requests = 0;
  while (ssh_is_connected(session) && !subsystem &&
         requests++ < kMaximumChannelRequests) {
    ssh_message message = ssh_message_get(session);
    if (!message) break;
    if (!channel && ssh_message_type(message) == SSH_REQUEST_CHANNEL_OPEN &&
        ssh_message_subtype(message) == SSH_CHANNEL_SESSION) {
      channel = ssh_message_channel_request_open_reply_accept(message);
    } else if (channel && ssh_message_type(message) == SSH_REQUEST_CHANNEL &&
               ssh_message_subtype(message) == SSH_CHANNEL_REQUEST_SUBSYSTEM &&
               ssh_message_channel_request_channel(message) == channel) {
      const char* requested = ssh_message_channel_request_subsystem(message);
      if (requested && std::string_view(requested) == "netconf" &&
          ssh_message_channel_request_reply_success(message) == SSH_OK) {
        subsystem = true;
      } else {
        (void)ssh_message_reply_default(message);
        ssh_message_free(message);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        return nullptr;
      }
    } else {
      (void)ssh_message_reply_default(message);
    }
    ssh_message_free(message);
  }
  if (!subsystem && channel) {
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return nullptr;
  }
  return channel;
}

void DrainDiagnostics(Application& application, std::ostream& diagnostics) {
  for (const std::string& delta : application.DrainBackendDeltas())
    diagnostics << "dangd: configuration delta: " << delta << '\n';
  for (const std::string& audit : application.DrainRecoveryAuditRecords())
    diagnostics << "dangd: audit: " << audit << '\n';
}

bool BoundHandshakeIo(ssh_session session) {
  constexpr timeval kHandshakeTimeout{30, 0};
  const int socket = ssh_get_fd(session);
  return socket >= 0 &&
      setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &kHandshakeTimeout,
                 sizeof(kHandshakeTimeout)) == 0 &&
      setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &kHandshakeTimeout,
                 sizeof(kHandshakeTimeout)) == 0;
}

}  // namespace

int RunReloadableSshServer(std::unique_ptr<Application>& application,
                           const ApplicationOptions& application_options,
                           const SshServerOptions& options,
                           std::ostream& diagnostics) {
  if (ssh_init() != SSH_OK) {
    diagnostics << "dangd: cannot initialize libssh\n";
    return 1;
  }
  if (!yang::netconf::UsernameMappingsValid(options.username_mappings)) {
    diagnostics << "dangd: SSH username mapping table is invalid\n";
    return 1;
  }
  std::map<std::string, AuthorizedIdentity> authorization;
  if (!LoadAuthorization(options, &authorization, diagnostics)) return 1;

  Handle<ssh_bind, ssh_bind_free> listener(ssh_bind_new(), ssh_bind_free);
  if (!listener) return 1;
  const unsigned int port = options.port;
  if (ssh_bind_options_set(listener.get(), SSH_BIND_OPTIONS_BINDADDR,
                           options.address.c_str()) != SSH_OK ||
      ssh_bind_options_set(listener.get(), SSH_BIND_OPTIONS_BINDPORT, &port) !=
          SSH_OK ||
      ssh_bind_options_set(listener.get(), SSH_BIND_OPTIONS_HOSTKEY,
                           options.host_key.c_str()) != SSH_OK ||
      ssh_bind_listen(listener.get()) != SSH_OK) {
    diagnostics << "dangd: cannot start SSH listener: "
                << ssh_get_error(listener.get()) << '\n';
    return 1;
  }

  struct sigaction action {};
  action.sa_handler = RequestReload;
  sigemptyset(&action.sa_mask);
  struct sigaction previous {};
  if (sigaction(SIGHUP, &action, &previous) != 0) {
    diagnostics << "dangd: cannot install SIGHUP handler: "
                << std::strerror(errno) << '\n';
    return 1;
  }
  reload_requested = 0;
  diagnostics << "dangd: SSH listening on " << options.address << ':'
              << options.port << '\n';

  std::size_t accepted = 0;
  std::uint32_t session_id = 1;
  int result = 0;
  while (options.maximum_connections == 0 ||
         accepted < options.maximum_connections) {
    if (reload_requested) {
      reload_requested = 0;
      auto loaded = Application::Reload(application_options, *application);
      if (loaded.application) {
        const std::string id = loaded.application->yang_library_content_id();
        if (id != application->yang_library_content_id()) {
          (void)application->PublishYangLibraryUpdate(id);
          diagnostics << "dangd: reloaded YANG library " << id << '\n';
        } else {
          diagnostics <<
              "dangd: reloaded implementation; YANG library unchanged\n";
        }
        application = std::move(loaded.application);
      } else {
        diagnostics << "dangd: SIGHUP reload rejected; keeping current schema\n";
        for (const std::string& message : loaded.errors)
          diagnostics << "dangd: reload: " << message << '\n';
      }
    }
    Handle<ssh_session, ssh_free> session(ssh_new(), ssh_free);
    if (!session || ssh_bind_accept(listener.get(), session.get()) != SSH_OK) {
      if (reload_requested) continue;
      diagnostics << "dangd: SSH accept failed: "
                  << ssh_get_error(listener.get()) << '\n';
      result = 1;
      break;
    }
    ++accepted;
    if (!BoundHandshakeIo(session.get())) {
      diagnostics << "dangd: cannot apply SSH handshake I/O timeout\n";
      ssh_disconnect(session.get());
      continue;
    }
    if (ssh_handle_key_exchange(session.get()) != SSH_OK) {
      diagnostics << "dangd: SSH key exchange failed\n";
      continue;
    }
    const auto authenticated = Authenticate(session.get(), authorization);
    if (!authenticated) {
      diagnostics << "dangd: SSH public-key authentication failed\n";
      ssh_disconnect(session.get());
      continue;
    }
    ssh_channel raw_channel = AcceptNetconfSubsystem(session.get());
    if (!raw_channel) {
      diagnostics << "dangd: SSH client did not request exact netconf subsystem\n";
      ssh_disconnect(session.get());
      continue;
    }
    Handle<ssh_channel, ssh_channel_free> channel(raw_channel, ssh_channel_free);
    const AuthorizedIdentity& authorized = authorization.at(*authenticated);
    const auto local_username = yang::netconf::MapAuthenticatedUsername(
        *authenticated, options.username_mappings,
        options.require_username_mapping);
    if (!local_username) {
      diagnostics << "dangd: SSH username mapping rejected authenticated user\n";
      channel.reset();
      ssh_disconnect(session.get());
      continue;
    }
    LibsshStream stream(channel.get());
    yang::netconf::TransportIdentity identity{
        yang::netconf::SecureTransport::kSsh, *local_username,
        authorized.external_groups, "netconf", true,
        !authorized.external_groups.empty()};
    const std::uint32_t allocated_session_id = session_id;
    session_id = session_id == std::numeric_limits<std::uint32_t>::max()
        ? 1
        : session_id + 1;
    yang::netconf::NetconfTransportAdapter adapter(
        application->server(), stream, allocated_session_id,
        std::move(identity));
    std::array<char, 16 * 1024> buffer{};
    while (adapter.valid() && ssh_channel_is_open(channel.get()) &&
           !ssh_channel_is_eof(channel.get())) {
      const int count = ssh_channel_read_timeout(
          channel.get(), buffer.data(), buffer.size(), 0, 1000);
      if (count == SSH_AGAIN) {
        adapter.Poll();
        continue;
      }
      if (count <= 0) break;
      adapter.Receive(std::string_view(buffer.data(),
                                       static_cast<std::size_t>(count)));
      adapter.Poll();
      DrainDiagnostics(*application, diagnostics);
    }
    adapter.TransportClosed();
    channel.reset();
    ssh_disconnect(session.get());
  }
  (void)sigaction(SIGHUP, &previous, nullptr);
  return result;
}

}  // namespace dangd
