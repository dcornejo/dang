// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/tls_transport.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <condition_variable>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "dangd/application.h"
#include "yang/netconf_transport.h"
#include "yang/resource_limits.h"
#include "yang/xml_security.h"

namespace dangd {
namespace {

volatile std::sig_atomic_t reload_requested = 0;

extern "C" void RequestReload(int) { reload_requested = 1; }

using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Session = std::unique_ptr<SSL, decltype(&SSL_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

void DrainDiagnostics(Application& application, std::ostream& diagnostics) {
  for (const std::string& delta : application.DrainBackendDeltas())
    diagnostics << "dangd: configuration delta: " << delta << '\n';
  for (const std::string& audit : application.DrainRecoveryAuditRecords())
    diagnostics << "dangd: audit: " << audit << '\n';
  for (const std::string& error : application.PollPluginNotifications())
    diagnostics << "dangd: plugin notification: " << error << '\n';
}

std::string LastTlsError(std::string_view prefix) {
  const unsigned long code = ERR_get_error();
  if (code == 0) return std::string(prefix);
  std::array<char, 256> text{};
  ERR_error_string_n(code, text.data(), text.size());
  return std::string(prefix) + ": " + text.data();
}

bool ConfigureCredentials(SSL_CTX* context,
                          const std::filesystem::path& certificate,
                          const std::filesystem::path& private_key,
                          const std::filesystem::path& trust_anchor,
                          std::string* error) {
  if (SSL_CTX_use_certificate_chain_file(context,
                                         certificate.string().c_str()) != 1) {
    *error = LastTlsError("cannot load certificate chain");
    return false;
  }
  if (SSL_CTX_use_PrivateKey_file(context, private_key.string().c_str(),
                                  SSL_FILETYPE_PEM) != 1 ||
      SSL_CTX_check_private_key(context) != 1) {
    *error = LastTlsError("cannot load matching private key");
    return false;
  }
  if (SSL_CTX_load_verify_locations(context, trust_anchor.string().c_str(),
                                    nullptr) != 1) {
    *error = LastTlsError("cannot load TLS trust anchor");
    return false;
  }
  return true;
}

bool ConfigurePeerIdentity(SSL* tls, const std::string& host,
                           std::string* error) {
  std::array<unsigned char, sizeof(in6_addr)> address{};
  const bool numeric = inet_pton(AF_INET, host.c_str(), address.data()) == 1 ||
                       inet_pton(AF_INET6, host.c_str(), address.data()) == 1;
  X509_VERIFY_PARAM* verification = SSL_get0_param(tls);
  if (verification == nullptr ||
      (numeric ? X509_VERIFY_PARAM_set1_ip_asc(verification, host.c_str())
               : X509_VERIFY_PARAM_set1_host(verification, host.data(),
                                             host.size())) != 1) {
    *error = "cannot configure TLS peer identity verification";
    return false;
  }
  if (!numeric && SSL_set_tlsext_host_name(tls, host.c_str()) != 1) {
    *error = LastTlsError("cannot configure TLS server name");
    return false;
  }
  return true;
}

int ConnectSocket(std::string_view host, std::uint16_t port,
                  std::uint32_t timeout_milliseconds,
                  std::optional<std::chrono::steady_clock::time_point>
                      transaction_deadline,
                  std::string* error) {
  if (timeout_milliseconds == 0) {
    *error = "TLS client timeout must be positive";
    return -1;
  }
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(timeout_milliseconds);
  if (transaction_deadline && *transaction_deadline < deadline)
    deadline = *transaction_deadline;
  if (deadline <= std::chrono::steady_clock::now()) {
    *error = "TLS client deadline expired";
    return -1;
  }
  struct SocketAddress {
    sockaddr_storage storage{};
    socklen_t length = 0;
    int family = AF_UNSPEC;
    int socket_type = 0;
    int protocol = 0;
  };
  const auto collect = [](addrinfo* addresses) {
    constexpr std::size_t kMaximumResolvedAddresses = 32;
    std::vector<SocketAddress> result;
    for (const addrinfo* address = addresses; address != nullptr;
         address = address->ai_next) {
      if (result.size() == kMaximumResolvedAddresses) break;
      if (address->ai_addr == nullptr || address->ai_addrlen <= 0 ||
          static_cast<std::size_t>(address->ai_addrlen) >
              sizeof(sockaddr_storage)) {
        continue;
      }
      SocketAddress copied{.length = address->ai_addrlen,
                           .family = address->ai_family,
                           .socket_type = address->ai_socktype,
                           .protocol = address->ai_protocol};
      std::memcpy(&copied.storage, address->ai_addr,
                  static_cast<std::size_t>(address->ai_addrlen));
      result.push_back(copied);
    }
    return result;
  };
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  const std::string service = std::to_string(port);
  const std::string host_text(host);
  std::array<unsigned char, sizeof(in6_addr)> numeric_bytes{};
  const bool numeric =
      inet_pton(AF_INET, host_text.c_str(), numeric_bytes.data()) == 1 ||
      inet_pton(AF_INET6, host_text.c_str(), numeric_bytes.data()) == 1;
  std::vector<SocketAddress> resolved;
  if (numeric) {
    hints.ai_flags |= AI_NUMERICHOST;
    addrinfo* addresses = nullptr;
    const int lookup = getaddrinfo(host_text.c_str(), service.c_str(), &hints,
                                   &addresses);
    if (lookup != 0) {
      *error = std::string("cannot resolve TLS server: ") +
               gai_strerror(lookup);
      return -1;
    }
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(addresses,
                                                             freeaddrinfo);
    resolved = collect(addresses);
  } else {
    struct ResolverState {
      std::mutex mutex;
      std::condition_variable ready;
      bool complete = false;
      int result = EAI_FAIL;
      std::vector<SocketAddress> addresses;
    };
    constexpr std::size_t kMaximumConcurrentResolvers = 8;
    // A platform resolver call cannot be cancelled portably. The process-wide
    // cap bounds timed-out workers and later requests fail closed until a
    // worker eventually returns capacity.
    static std::atomic<std::size_t> active_resolvers{0};
    auto* resolver_counter = &active_resolvers;
    std::size_t active = active_resolvers.load(std::memory_order_relaxed);
    while (active < kMaximumConcurrentResolvers &&
           !active_resolvers.compare_exchange_weak(
               active, active + 1, std::memory_order_acq_rel,
               std::memory_order_relaxed)) {
    }
    if (active >= kMaximumConcurrentResolvers) {
      *error = "TLS server resolver capacity is exhausted";
      return -1;
    }
    const auto state = std::make_shared<ResolverState>();
    try {
      std::thread([state, host_text, service, hints, collect,
                   resolver_counter] {
        int lookup = EAI_MEMORY;
        std::vector<SocketAddress> addresses;
        try {
          addrinfo* raw = nullptr;
          lookup = getaddrinfo(host_text.c_str(), service.c_str(), &hints,
                               &raw);
          std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(raw,
                                                                   freeaddrinfo);
          if (lookup == 0) addresses = collect(raw);
        } catch (...) {
          lookup = EAI_MEMORY;
        }
        {
          std::lock_guard lock(state->mutex);
          state->result = lookup;
          state->addresses = std::move(addresses);
          state->complete = true;
        }
        state->ready.notify_one();
        resolver_counter->fetch_sub(1, std::memory_order_acq_rel);
      }).detach();
    } catch (...) {
      active_resolvers.fetch_sub(1, std::memory_order_acq_rel);
      *error = "cannot start TLS server resolver";
      return -1;
    }
    std::unique_lock lock(state->mutex);
    if (!state->ready.wait_until(lock, deadline,
                                 [&state] { return state->complete; })) {
      *error = "TLS server name resolution timed out";
      return -1;
    }
    if (state->result != 0) {
      *error = std::string("cannot resolve TLS server: ") +
               gai_strerror(state->result);
      return -1;
    }
    resolved = std::move(state->addresses);
  }
  if (resolved.empty()) {
    *error = "TLS server name resolved to no usable addresses";
    return -1;
  }
  const auto remaining_milliseconds = [&]() -> std::optional<std::uint32_t> {
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) return std::nullopt;
    return static_cast<std::uint32_t>(std::min<std::int64_t>(
        remaining.count(), std::numeric_limits<std::uint32_t>::max()));
  };
  int last_socket_error = 0;
  for (const SocketAddress& address : resolved) {
    const auto remaining = remaining_milliseconds();
    if (!remaining) {
      *error = "TLS server connection timed out";
      return -1;
    }
    const int socket_fd =
        socket(address.family, address.socket_type, address.protocol);
    if (socket_fd < 0) continue;
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      close(socket_fd);
      continue;
    }
    int connected = connect(
        socket_fd, reinterpret_cast<const sockaddr*>(&address.storage),
        address.length);
    if (connected != 0 && errno == EINPROGRESS) {
      const auto poll_budget = remaining_milliseconds();
      if (!poll_budget) {
        close(socket_fd);
        *error = "TLS server connection timed out";
        return -1;
      }
      pollfd descriptor{socket_fd, POLLOUT, 0};
      const std::uint32_t bounded = std::min<std::uint32_t>(
          *poll_budget,
          static_cast<std::uint32_t>(std::numeric_limits<int>::max()));
      connected = poll(&descriptor, 1, static_cast<int>(bounded));
      if (connected <= 0) {
        last_socket_error = connected == 0 ? ETIMEDOUT : errno;
        close(socket_fd);
        continue;
      }
      int socket_error = 0;
      socklen_t error_size = sizeof(socket_error);
      if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error,
                     &error_size) != 0 ||
          socket_error != 0) {
        last_socket_error = socket_error == 0 ? errno : socket_error;
        close(socket_fd);
        continue;
      }
    } else if (connected != 0) {
      last_socket_error = errno;
      close(socket_fd);
      continue;
    }
    if (fcntl(socket_fd, F_SETFL, flags) != 0) {
      close(socket_fd);
      continue;
    }
    const auto socket_budget = remaining_milliseconds();
    if (!socket_budget) {
      close(socket_fd);
      *error = "TLS server connection timed out";
      return -1;
    }
    timeval timeout{
        .tv_sec = static_cast<time_t>(*socket_budget / 1000),
        .tv_usec = static_cast<suseconds_t>(
            (*socket_budget % 1000) * 1000)};
    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                   sizeof(timeout)) != 0) {
      close(socket_fd);
      continue;
    }
    return socket_fd;
  }
  *error = last_socket_error == 0
               ? "cannot connect to TLS server"
               : std::string("cannot connect to TLS server: ") +
                     std::strerror(last_socket_error);
  return -1;
}

int ListenSocket(const TlsServerOptions& options, std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  const std::string service = std::to_string(options.port);
  const char* host = options.address.empty() ? nullptr : options.address.c_str();
  const int lookup = getaddrinfo(host, service.c_str(), &hints, &addresses);
  if (lookup != 0) {
    *error = std::string("cannot resolve TLS listen address: ") +
             gai_strerror(lookup);
    return -1;
  }
  std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> owned(addresses,
                                                           freeaddrinfo);
  for (addrinfo* address = addresses; address != nullptr;
       address = address->ai_next) {
    const int socket_fd =
        socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket_fd < 0) continue;
    const int enabled = 1;
    setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
    if (bind(socket_fd, address->ai_addr, address->ai_addrlen) == 0 &&
        listen(socket_fd, 16) == 0)
      return socket_fd;
    close(socket_fd);
  }
  *error = std::string("cannot bind TLS listener: ") + std::strerror(errno);
  return -1;
}

std::optional<std::string> SafeUsername(const unsigned char* bytes,
                                        int length) {
  if (bytes == nullptr || length <= 0 || length > 255) return std::nullopt;
  const std::string username(reinterpret_cast<const char*>(bytes),
                             static_cast<std::size_t>(length));
  if (username.find('\0') != std::string::npos ||
      std::isspace(static_cast<unsigned char>(username.front())) ||
      std::isspace(static_cast<unsigned char>(username.back())) ||
      std::ranges::any_of(username, [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
      })) {
    return std::nullopt;
  }
  return username;
}

std::optional<std::string> PeerUsername(SSL* tls,
                                        TlsUsernameSource source) {
  Certificate certificate(SSL_get1_peer_certificate(tls), X509_free);
  if (!certificate || SSL_get_verify_result(tls) != X509_V_OK)
    return std::nullopt;
  return CertificateUsername(certificate.get(), source);
}

bool WriteTls(SSL* tls, std::string_view bytes, std::string* error) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    std::size_t count = 0;
    if (SSL_write_ex(tls, bytes.data() + written, bytes.size() - written,
                     &count) != 1) {
      *error = LastTlsError("TLS write failed");
      return false;
    }
    written += count;
  }
  return true;
}

std::optional<std::string> ReadBase10Message(SSL* tls, std::string* pending,
                                             std::string* error,
                                             std::size_t maximum_message_size =
                                                 16 * 1024 * 1024) {
  constexpr std::string_view delimiter = "]]>]]>";
  std::array<char, 16 * 1024> buffer{};
  while (true) {
    const std::size_t end = pending->find(delimiter);
    if (end != std::string::npos) {
      if (end > maximum_message_size) {
        *error = "NETCONF message exceeds the byte limit";
        return std::nullopt;
      }
      std::string message = pending->substr(0, end);
      pending->erase(0, end + delimiter.size());
      return message;
    }
    std::size_t count = 0;
    if (SSL_read_ex(tls, buffer.data(), buffer.size(), &count) != 1) {
      *error = LastTlsError("TLS connection closed while reading NETCONF");
      return std::nullopt;
    }
    pending->append(buffer.data(), count);
    if (pending->size() > maximum_message_size + delimiter.size() - 1) {
      *error = "NETCONF message exceeds the byte limit";
      return std::nullopt;
    }
  }
}

std::string_view XmlLocalName(std::string_view name) {
  const std::size_t separator = name.find(':');
  return separator == std::string_view::npos ? name
                                              : name.substr(separator + 1);
}

std::string XmlNamespace(pugi::xml_node node) {
  const std::string_view name = node.name();
  const std::size_t separator = name.find(':');
  const std::string attribute =
      separator == std::string_view::npos
          ? "xmlns"
          : "xmlns:" + std::string(name.substr(0, separator));
  for (pugi::xml_node current = node; current; current = current.parent()) {
    const pugi::xml_attribute declaration = current.attribute(attribute.c_str());
    if (declaration) return declaration.as_string();
  }
  return {};
}

bool ValidateNetconfDocument(std::string_view xml, std::string_view root_name,
                             std::string* error) {
  if (xml.size() > yang::DefaultResourceLimits().maximum_xml_bytes) {
    *error = "NETCONF XML exceeds the byte limit";
    return false;
  }
  pugi::xml_document parsed;
  const auto status = yang::ParseUntrustedXml(xml, &parsed);
  if (!status.ok) {
    *error = "NETCONF document is not safe well-formed XML";
    return false;
  }
  const pugi::xml_node root = parsed.document_element();
  if (!root || XmlLocalName(root.name()) != root_name ||
      XmlNamespace(root) != "urn:ietf:params:xml:ns:netconf:base:1.0") {
    *error = "NETCONF document has the wrong root element or namespace";
    return false;
  }
  return true;
}

std::optional<std::string> RpcMessageId(std::string_view xml) {
  pugi::xml_document parsed;
  if (!yang::ParseUntrustedXml(xml, &parsed).ok) return std::nullopt;
  const pugi::xml_attribute id =
      parsed.document_element().attribute("message-id");
  if (!id || std::string_view(id.value()).empty()) return std::nullopt;
  return id.value();
}

bool HelloHasCapability(std::string_view xml, std::string_view capability) {
  pugi::xml_document parsed;
  if (!yang::ParseUntrustedXml(xml, &parsed).ok) return false;
  for (const pugi::xml_node child : parsed.document_element().children()) {
    if (XmlLocalName(child.name()) != "capabilities" ||
        XmlNamespace(child) !=
            "urn:ietf:params:xml:ns:netconf:base:1.0")
      continue;
    for (const pugi::xml_node value : child.children()) {
      if (XmlLocalName(value.name()) == "capability" &&
          XmlNamespace(value) ==
              "urn:ietf:params:xml:ns:netconf:base:1.0" &&
          std::string_view(value.text().as_string()) == capability)
        return true;
    }
  }
  return false;
}

class OpenSslStream final : public yang::netconf::SecureByteStream {
 public:
  explicit OpenSslStream(SSL* tls) : tls_(tls) {}
  yang::netconf::WriteStatus Write(std::string_view bytes) override {
    std::string ignored;
    return WriteTls(tls_, bytes, &ignored)
               ? yang::netconf::WriteStatus::kAccepted
               : yang::netconf::WriteStatus::kFailed;
  }
  void Close() override {
    if (!closed_) SSL_shutdown(tls_);
    closed_ = true;
  }

 private:
  SSL* tls_;
  bool closed_ = false;
};

}  // namespace

std::optional<std::string> CertificateSubjectUsername(
    const X509_NAME* subject) {
  if (subject == nullptr) return std::nullopt;
  const int first = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  if (first < 0 ||
      X509_NAME_get_index_by_NID(subject, NID_commonName, first) >= 0) {
    return std::nullopt;
  }
  const X509_NAME_ENTRY* entry = X509_NAME_get_entry(subject, first);
  if (entry == nullptr) return std::nullopt;
  unsigned char* utf8 = nullptr;
  const int length = ASN1_STRING_to_UTF8(
      &utf8, X509_NAME_ENTRY_get_data(entry));
  if (length <= 0 || length > 255 || utf8 == nullptr) {
    OPENSSL_free(utf8);
    return std::nullopt;
  }
  const auto username = SafeUsername(utf8, length);
  OPENSSL_free(utf8);
  return username;
}

std::optional<std::string> CertificateUsername(
    const X509* certificate, TlsUsernameSource source) {
  if (certificate == nullptr) return std::nullopt;
  if (source == TlsUsernameSource::kCommonName) {
    return CertificateSubjectUsername(X509_get_subject_name(certificate));
  }
  GENERAL_NAMES* names = static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
  if (names == nullptr) return std::nullopt;
  std::unique_ptr<GENERAL_NAMES, decltype(&GENERAL_NAMES_free)> owned(
      names, GENERAL_NAMES_free);
  const int wanted = source == TlsUsernameSource::kSanDns ? GEN_DNS : GEN_URI;
  std::optional<std::string> username;
  for (int index = 0; index < sk_GENERAL_NAME_num(names); ++index) {
    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, index);
    if (name == nullptr || name->type != wanted) continue;
    if (username) return std::nullopt;
    const ASN1_IA5STRING* value = name->d.ia5;
    username = SafeUsername(ASN1_STRING_get0_data(value),
                            ASN1_STRING_length(value));
    if (!username) return std::nullopt;
  }
  return username;
}

int RunTlsServer(Application& application, const TlsServerOptions& options,
                 std::ostream& diagnostics) {
  if (!yang::netconf::UsernameMappingsValid(options.username_mappings)) {
    diagnostics << "dangd: TLS username mapping table is invalid\n";
    return 1;
  }
  Context context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
  if (!context) {
    diagnostics << LastTlsError("cannot create TLS server context") << '\n';
    return 1;
  }
  SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
  SSL_CTX_set_verify(context.get(),
                     SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
  std::string error;
  if (!ConfigureCredentials(context.get(), options.certificate,
                            options.private_key, options.trust_anchor, &error)) {
    diagnostics << "dangd: " << error << '\n';
    return 1;
  }
  const int listener = ListenSocket(options, &error);
  if (listener < 0) {
    diagnostics << "dangd: " << error << '\n';
    return 1;
  }
  diagnostics << "dangd: TLS listening on " << options.address << ':'
              << options.port << '\n';
  std::size_t accepted = 0;
  std::uint32_t session_id = 1;
  while (options.maximum_connections == 0 ||
         accepted < options.maximum_connections) {
    const int connection = accept(listener, nullptr, nullptr);
    if (connection < 0) {
      diagnostics << "dangd: TLS accept failed: " << std::strerror(errno)
                  << '\n';
      close(listener);
      return 1;
    }
    ++accepted;
    Session tls(SSL_new(context.get()), SSL_free);
    if (!tls) {
      close(connection);
      continue;
    }
    SSL_set_fd(tls.get(), connection);
    if (SSL_accept(tls.get()) != 1) {
      diagnostics << "dangd: " << LastTlsError("TLS handshake failed") << '\n';
      close(connection);
      continue;
    }
    auto username = PeerUsername(tls.get(), options.username_source);
    if (username) {
      username = yang::netconf::MapAuthenticatedUsername(
          *username, options.username_mappings,
          options.require_username_mapping);
    }
    if (!username) {
      diagnostics << "dangd: authenticated certificate has no unique safe "
                     "configured username field\n";
      SSL_shutdown(tls.get());
      close(connection);
      continue;
    }
    OpenSslStream stream(tls.get());
    yang::netconf::TransportIdentity identity{
        yang::netconf::SecureTransport::kTls, *username, {}, "", true};
    yang::netconf::NetconfTransportAdapter adapter(
        application.server(), stream, session_id++, std::move(identity));
    std::array<char, 16 * 1024> buffer{};
    while (adapter.valid()) {
      if (SSL_pending(tls.get()) == 0) {
        pollfd descriptor{connection, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 50);
        if (ready == 0) {
          DrainDiagnostics(application, diagnostics);
          adapter.Poll();
          continue;
        }
        if (ready < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if ((descriptor.revents & POLLIN) == 0) break;
      }
      std::size_t count = 0;
      if (SSL_read_ex(tls.get(), buffer.data(), buffer.size(), &count) != 1)
        break;
      adapter.Receive(std::string_view(buffer.data(), count));
      DrainDiagnostics(application, diagnostics);
    }
    adapter.TransportClosed();
    SSL_shutdown(tls.get());
    close(connection);
  }
  close(listener);
  return 0;
}

int RunReloadableTlsServer(
    std::unique_ptr<Application>& application,
    const ApplicationOptions& application_options,
    const TlsServerOptions& options, std::ostream& diagnostics) {
  if (!yang::netconf::UsernameMappingsValid(options.username_mappings)) {
    diagnostics << "dangd: TLS username mapping table is invalid\n";
    return 1;
  }
  Context context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
  if (!context) return 1;
  SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
  SSL_CTX_set_verify(context.get(),
                     SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                     nullptr);
  std::string error;
  if (!ConfigureCredentials(context.get(), options.certificate,
                            options.private_key, options.trust_anchor, &error)) {
    diagnostics << "dangd: " << error << '\n';
    return 1;
  }
  const int listener = ListenSocket(options, &error);
  if (listener < 0) {
    diagnostics << "dangd: " << error << '\n';
    return 1;
  }
  struct sigaction action {};
  action.sa_handler = RequestReload;
  sigemptyset(&action.sa_mask);
  struct sigaction previous {};
  if (sigaction(SIGHUP, &action, &previous) != 0) {
    diagnostics << "dangd: cannot install SIGHUP handler: "
                << std::strerror(errno) << '\n';
    close(listener);
    return 1;
  }
  reload_requested = 0;
  const auto reload = [&]() -> std::unique_ptr<Application> {
    reload_requested = 0;
    auto loaded = Application::Reload(application_options, *application);
    if (!loaded.application) {
      diagnostics << "dangd: SIGHUP reload rejected; keeping current schema\n";
      for (const std::string& message : loaded.errors)
        diagnostics << "dangd: reload: " << message << '\n';
      return nullptr;
    }
    const std::string id = loaded.application->yang_library_content_id();
    if (id != application->yang_library_content_id()) {
      (void)application->PublishYangLibraryUpdate(id);
      diagnostics << "dangd: reloaded YANG library " << id << '\n';
    } else {
      diagnostics << "dangd: reloaded implementation; YANG library unchanged\n";
    }
    return std::move(loaded.application);
  };

  diagnostics << "dangd: TLS listening on " << options.address << ':'
              << options.port << '\n';
  std::size_t accepted = 0;
  std::uint32_t session_id = 1;
  int result = 0;
  while (options.maximum_connections == 0 ||
         accepted < options.maximum_connections) {
    if (reload_requested) {
      if (auto replacement = reload()) application = std::move(replacement);
    }
    const int connection = accept(listener, nullptr, nullptr);
    if (connection < 0) {
      if (errno == EINTR) continue;
      diagnostics << "dangd: TLS accept failed: " << std::strerror(errno)
                  << '\n';
      result = 1;
      break;
    }
    ++accepted;
    Session tls(SSL_new(context.get()), SSL_free);
    if (!tls) {
      close(connection);
      continue;
    }
    SSL_set_fd(tls.get(), connection);
    if (SSL_accept(tls.get()) != 1) {
      diagnostics << "dangd: " << LastTlsError("TLS handshake failed") << '\n';
      close(connection);
      continue;
    }
    auto username = PeerUsername(tls.get(), options.username_source);
    if (username) {
      username = yang::netconf::MapAuthenticatedUsername(
          *username, options.username_mappings,
          options.require_username_mapping);
    }
    if (!username) {
      diagnostics << "dangd: authenticated certificate has no unique safe "
                     "configured username field\n";
      SSL_shutdown(tls.get());
      close(connection);
      continue;
    }
    std::unique_ptr<Application> replacement;
    {
      OpenSslStream stream(tls.get());
      yang::netconf::TransportIdentity identity{
          yang::netconf::SecureTransport::kTls, *username, {}, "", true};
      yang::netconf::NetconfTransportAdapter adapter(
          application->server(), stream, session_id++, std::move(identity));
      std::array<char, 16 * 1024> buffer{};
      while (adapter.valid()) {
        if (reload_requested) {
          replacement = reload();
          adapter.Poll();
          break;
        }
        if (SSL_pending(tls.get()) == 0) {
          pollfd descriptor{connection, POLLIN, 0};
          const int ready = poll(&descriptor, 1, 50);
          if (ready == 0) {
            DrainDiagnostics(*application, diagnostics);
            adapter.Poll();
            continue;
          }
          if (ready < 0) {
            if (errno == EINTR) continue;
            break;
          }
          if ((descriptor.revents & POLLIN) == 0) break;
        }
        std::size_t count = 0;
        if (SSL_read_ex(tls.get(), buffer.data(), buffer.size(), &count) != 1) {
          if (reload_requested) {
            replacement = reload();
            adapter.Poll();
          }
          break;
        }
        adapter.Receive(std::string_view(buffer.data(), count));
        DrainDiagnostics(*application, diagnostics);
      }
      adapter.TransportClosed();
    }
    SSL_shutdown(tls.get());
    close(connection);
    if (replacement) application = std::move(replacement);
  }
  (void)sigaction(SIGHUP, &previous, nullptr);
  close(listener);
  return result;
}

struct TlsRpcSession::Impl {
  Impl(Context client_context, Session client_tls, int client_socket,
       std::string hello)
      : context(std::move(client_context)),
        tls(std::move(client_tls)),
        socket_fd(client_socket),
        server_hello(std::move(hello)) {}

  Context context{nullptr, SSL_CTX_free};
  Session tls{nullptr, SSL_free};
  int socket_fd = -1;
  std::string pending;
  std::string server_hello;
};

TlsRpcSession::TlsRpcSession(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

TlsRpcSession::~TlsRpcSession() { Close(); }

TlsRpcSession::TlsRpcSession(TlsRpcSession&&) noexcept = default;

TlsRpcSession& TlsRpcSession::operator=(TlsRpcSession&& other) noexcept {
  if (this == &other) return *this;
  Close();
  implementation_ = std::move(other.implementation_);
  return *this;
}

std::unique_ptr<TlsRpcSession> TlsRpcSession::Connect(
    const TlsClientOptions& options, std::string* error,
    std::span<const std::string_view> required_server_capabilities) {
  if (error == nullptr) return nullptr;
  error->clear();
  if (options.deadline &&
      std::chrono::steady_clock::now() >= *options.deadline) {
    *error = "TLS client deadline expired";
    return nullptr;
  }
  Context context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
  if (!context) {
    *error = LastTlsError("cannot create TLS client context");
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
  SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
  if (!ConfigureCredentials(context.get(), options.certificate,
                            options.private_key, options.trust_anchor, error))
    return nullptr;
  const int socket_fd = ConnectSocket(options.host, options.port,
                                      options.timeout_milliseconds,
                                      options.deadline, error);
  if (socket_fd < 0) return nullptr;
  Session tls(SSL_new(context.get()), SSL_free);
  if (!tls) {
    close(socket_fd);
    *error = "cannot create TLS client session";
    return nullptr;
  }
  if (SSL_set_fd(tls.get(), socket_fd) != 1) {
    *error = LastTlsError("cannot attach TLS client socket");
    close(socket_fd);
    return nullptr;
  }
  if (!ConfigurePeerIdentity(tls.get(), options.host, error)) {
    close(socket_fd);
    return nullptr;
  }
  if (SSL_connect(tls.get()) != 1) {
    *error = LastTlsError("TLS handshake failed");
    close(socket_fd);
    return nullptr;
  }

  std::string pending;
  auto server_hello = ReadBase10Message(tls.get(), &pending, error);
  if (!server_hello || !ValidateNetconfDocument(*server_hello, "hello", error) ||
      !HelloHasCapability(
          *server_hello,
          "urn:ietf:params:netconf:base:1.0")) {
    if (error->empty()) *error = "NETCONF server does not advertise base 1.0";
    SSL_shutdown(tls.get());
    close(socket_fd);
    return nullptr;
  }
  for (const std::string_view capability : required_server_capabilities) {
    if (HelloHasCapability(*server_hello, capability)) continue;
    *error = "NETCONF server does not advertise required capability " +
             std::string(capability);
    SSL_shutdown(tls.get());
    close(socket_fd);
    return nullptr;
  }
  constexpr std::string_view client_hello =
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability></capabilities></hello>]]>]]>";
  if (!WriteTls(tls.get(), client_hello, error)) {
    SSL_shutdown(tls.get());
    close(socket_fd);
    return nullptr;
  }
  auto implementation = std::make_unique<Impl>(
      std::move(context), std::move(tls), socket_fd, std::move(*server_hello));
  implementation->pending = std::move(pending);
  return std::unique_ptr<TlsRpcSession>(
      new TlsRpcSession(std::move(implementation)));
}

std::optional<std::string> TlsRpcSession::Execute(std::string_view rpc,
                                                  std::string* error) {
  if (error == nullptr) return std::nullopt;
  error->clear();
  if (!implementation_ || !implementation_->tls) {
    *error = "NETCONF TLS session is closed";
    return std::nullopt;
  }
  if (rpc.find("]]>]]>") != std::string_view::npos) {
    *error = "NETCONF RPC contains a framing delimiter";
    return std::nullopt;
  }
  if (!ValidateNetconfDocument(rpc, "rpc", error)) return std::nullopt;
  if (!WriteTls(implementation_->tls.get(),
                std::string(rpc) + "]]>]]>", error)) {
    Close();
    return std::nullopt;
  }
  auto reply = ReadBase10Message(implementation_->tls.get(),
                                 &implementation_->pending, error);
  if (!reply || !ValidateNetconfDocument(*reply, "rpc-reply", error)) {
    Close();
    return std::nullopt;
  }
  const auto request_id = RpcMessageId(rpc);
  const auto reply_id = RpcMessageId(*reply);
  if (!request_id || !reply_id || *request_id != *reply_id) {
    *error = "NETCONF reply message-id does not match the request";
    Close();
    return std::nullopt;
  }
  return reply;
}

bool TlsRpcSession::SetTimeout(std::chrono::milliseconds timeout,
                               std::string* error) {
  if (error == nullptr) return false;
  error->clear();
  if (!implementation_ || implementation_->socket_fd < 0) {
    *error = "NETCONF TLS session is closed";
    return false;
  }
  if (timeout <= std::chrono::milliseconds::zero()) {
    *error = "TLS client timeout must be positive";
    return false;
  }
  const auto bounded = std::min<std::int64_t>(
      timeout.count(), std::numeric_limits<std::uint32_t>::max());
  const auto milliseconds = static_cast<std::uint32_t>(bounded);
  const timeval socket_timeout{
      .tv_sec = static_cast<time_t>(milliseconds / 1000),
      .tv_usec =
          static_cast<suseconds_t>((milliseconds % 1000) * 1000)};
  if (setsockopt(implementation_->socket_fd, SOL_SOCKET, SO_RCVTIMEO,
                 &socket_timeout, sizeof(socket_timeout)) != 0 ||
      setsockopt(implementation_->socket_fd, SOL_SOCKET, SO_SNDTIMEO,
                 &socket_timeout, sizeof(socket_timeout)) != 0) {
    *error = std::string("cannot update TLS client timeout: ") +
             std::strerror(errno);
    return false;
  }
  return true;
}

const std::string& TlsRpcSession::server_hello() const noexcept {
  static const std::string empty;
  return implementation_ ? implementation_->server_hello : empty;
}

void TlsRpcSession::Close() noexcept {
  if (!implementation_) return;
  if (implementation_->tls) SSL_shutdown(implementation_->tls.get());
  if (implementation_->socket_fd >= 0) {
    close(implementation_->socket_fd);
    implementation_->socket_fd = -1;
  }
  implementation_->tls.reset();
}

std::optional<TlsRpcExchange> ExchangeTlsRpc(
    const TlsClientOptions& options, std::string_view rpc, std::string* error,
    std::span<const std::string_view> required_server_capabilities) {
  auto session =
      TlsRpcSession::Connect(options, error, required_server_capabilities);
  if (!session) return std::nullopt;
  const std::string server_hello = session->server_hello();
  auto reply = session->Execute(rpc, error);
  if (!reply) return std::nullopt;
  return TlsRpcExchange{.server_hello = server_hello,
                        .reply = std::move(*reply)};
}

int RunTlsClient(const TlsClientOptions& options, std::istream& input,
                 std::ostream& output, std::ostream& diagnostics) {
  Context context(SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
  if (!context) {
    diagnostics << LastTlsError("cannot create TLS client context") << '\n';
    return 1;
  }
  SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION);
  SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);
  std::string error;
  if (!ConfigureCredentials(context.get(), options.certificate,
                            options.private_key, options.trust_anchor, &error)) {
    diagnostics << "dangctl: " << error << '\n';
    return 1;
  }
  const int socket_fd = ConnectSocket(options.host, options.port,
                                      options.timeout_milliseconds,
                                      options.deadline, &error);
  if (socket_fd < 0) {
    diagnostics << "dangctl: " << error << '\n';
    return 1;
  }
  Session tls(SSL_new(context.get()), SSL_free);
  if (!tls) {
    close(socket_fd);
    return 1;
  }
  if (SSL_set_fd(tls.get(), socket_fd) != 1) {
    diagnostics << "dangctl: "
                << LastTlsError("cannot attach TLS client socket") << '\n';
    close(socket_fd);
    return 1;
  }
  if (!ConfigurePeerIdentity(tls.get(), options.host, &error)) {
    diagnostics << "dangctl: " << error << '\n';
    close(socket_fd);
    return 1;
  }
  if (SSL_connect(tls.get()) != 1) {
    diagnostics << "dangctl: " << LastTlsError("TLS handshake failed") << '\n';
    close(socket_fd);
    return 1;
  }
  std::string pending;
  const auto server_hello = ReadBase10Message(tls.get(), &pending, &error);
  if (!server_hello) {
    diagnostics << "dangctl: " << error << '\n';
    close(socket_fd);
    return 1;
  }
  output << "SERVER HELLO\n" << *server_hello << "\n\n";
  constexpr std::string_view client_hello =
      "<hello xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<capabilities><capability>urn:ietf:params:netconf:base:1.0"
      "</capability></capabilities></hello>]]>]]>";
  if (!WriteTls(tls.get(), client_hello, &error)) {
    diagnostics << "dangctl: " << error << '\n';
    close(socket_fd);
    return 1;
  }

  output << "Paste one XML RPC, then enter a blank line. EOF exits.\n";
  std::string document;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      document += line + '\n';
      continue;
    }
    if (document.empty()) continue;
    if (!WriteTls(tls.get(), document + "]]>]]>", &error)) break;
    document.clear();
    const auto response = ReadBase10Message(tls.get(), &pending, &error);
    if (!response) break;
    output << "REPLY\n" << *response << "\n\n";
  }
  if (!document.empty() && WriteTls(tls.get(), document + "]]>]]>", &error)) {
    if (const auto response = ReadBase10Message(tls.get(), &pending, &error))
      output << "REPLY\n" << *response << '\n';
  }
  SSL_shutdown(tls.get());
  close(socket_fd);
  if (!error.empty()) {
    diagnostics << "dangctl: " << error << '\n';
    return 1;
  }
  return 0;
}

}  // namespace dangd
