// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/tls_transport.h"

#include <array>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "dangd/application.h"
#include "yang/netconf_transport.h"

namespace dangd {
namespace {

volatile std::sig_atomic_t reload_requested = 0;

extern "C" void RequestReload(int) { reload_requested = 1; }

using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Session = std::unique_ptr<SSL, decltype(&SSL_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;

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

int ConnectSocket(std::string_view host, std::uint16_t port,
                  std::string* error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  const std::string service = std::to_string(port);
  const std::string host_text(host);
  const int lookup = getaddrinfo(host_text.c_str(), service.c_str(), &hints,
                                 &addresses);
  if (lookup != 0) {
    *error = std::string("cannot resolve TLS server: ") +
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
    if (connect(socket_fd, address->ai_addr, address->ai_addrlen) == 0)
      return socket_fd;
    close(socket_fd);
  }
  *error = std::string("cannot connect to TLS server: ") +
           std::strerror(errno);
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
                                             std::string* error) {
  constexpr std::string_view delimiter = "]]>]]>";
  std::array<char, 16 * 1024> buffer{};
  while (true) {
    const std::size_t end = pending->find(delimiter);
    if (end != std::string::npos) {
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
  }
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
      std::size_t count = 0;
      if (SSL_read_ex(tls.get(), buffer.data(), buffer.size(), &count) != 1)
        break;
      adapter.Receive(std::string_view(buffer.data(), count));
      for (const std::string& delta : application.DrainBackendDeltas())
        diagnostics << "dangd: configuration delta: " << delta << '\n';
      for (const std::string& audit : application.DrainRecoveryAuditRecords())
        diagnostics << "dangd: audit: " << audit << '\n';
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
        std::size_t count = 0;
        if (SSL_read_ex(tls.get(), buffer.data(), buffer.size(), &count) != 1) {
          if (reload_requested) {
            replacement = reload();
            adapter.Poll();
          }
          break;
        }
        adapter.Receive(std::string_view(buffer.data(), count));
        for (const std::string& delta : application->DrainBackendDeltas())
          diagnostics << "dangd: configuration delta: " << delta << '\n';
        for (const std::string& audit :
             application->DrainRecoveryAuditRecords())
          diagnostics << "dangd: audit: " << audit << '\n';
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
  const int socket_fd = ConnectSocket(options.host, options.port, &error);
  if (socket_fd < 0) {
    diagnostics << "dangctl: " << error << '\n';
    return 1;
  }
  Session tls(SSL_new(context.get()), SSL_free);
  if (!tls) {
    close(socket_fd);
    return 1;
  }
  SSL_set_fd(tls.get(), socket_fd);
  SSL_set_tlsext_host_name(tls.get(), options.host.c_str());
  SSL_set1_host(tls.get(), options.host.c_str());
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
