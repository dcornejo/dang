// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

// A narrowly scoped live server driver for the external ncclient NACM test.
// It links the production TLS transport and application; only event timing is
// controlled here so an independent client can subscribe before publication.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "dangd/application.h"
#include "dangd/tls_transport.h"

namespace {

bool WaitFor(const std::filesystem::path& marker,
             std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code error;
    if (std::filesystem::is_regular_file(marker, error) && !error) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool Mark(const std::filesystem::path& marker) {
  std::ofstream output(marker);
  output << "ready\n";
  return output.good();
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    std::cerr << "usage: nacm-notification-server SOURCE PORT MARKER-DIR\n";
    return 2;
  }
  const std::filesystem::path source = argv[1];
  const std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argv[2]));
  const std::filesystem::path markers = argv[3];
  dangd::ApplicationOptions application_options{
      .model = source / "dangd/examples/appliance.yang",
      .search_paths = {source / "dangd/models"},
      .configuration = source / "dangd/examples/config.xml",
      .nacm_configuration =
          source / "tests/interoperability/nacm-notifications.xml",
      .recovery_users = {"audit"}};
  auto loaded = dangd::Application::Load(application_options);
  if (!loaded.application) {
    for (const std::string& error : loaded.errors) std::cerr << error << '\n';
    return 1;
  }
  dangd::Application* application = loaded.application.get();
  const std::filesystem::path credentials = source / "dangd/testdata/tls";
  dangd::TlsServerOptions options{
      .address = "127.0.0.1",
      .port = port,
      .certificate = credentials / "server-cert.pem",
      .private_key = credentials / "server-key.pem",
      .trust_anchor = credentials / "ca-cert.pem",
      .maximum_connections = 1};
  std::ostringstream diagnostics;
  int server_result = -1;
  std::thread server([&] {
    server_result = dangd::RunTlsServer(*application, options, diagnostics);
  });

  int result = 0;
  if (!Mark(markers / "server-ready") ||
      !WaitFor(markers / "subscribed", std::chrono::seconds(15))) {
    std::cerr << "subscriber did not become ready\n";
    result = 1;
  } else if (!application->PublishYangLibraryUpdate("ncclient-interop")) {
    std::cerr << "no subscriber accepted the permitted notification\n";
    result = 1;
  } else if (!WaitFor(markers / "client-done", std::chrono::seconds(15))) {
    std::cerr << "independent client did not complete\n";
    result = 1;
  }

  server.join();
  if (server_result != 0) {
    std::cerr << diagnostics.str();
    result = 1;
  }
  const auto counters = application->server().Process(
      {9000, "audit", "audit", {}},
      "<rpc xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\" "
      "message-id=\"counters\"><get/></rpc>");
  if (counters.xml.find("<denied-notifications>1</denied-notifications>") ==
      std::string::npos) {
    std::cerr << "expected exactly one NACM-denied notification: "
              << counters.xml << '\n';
    result = 1;
  }
  if (result == 0) {
    std::cout << "dangd delivery: yang-library-update PASS\n"
              << "dangd suppression: yang-library-change PASS\n"
              << "denied-notifications counter: 1 PASS\n";
  }
  return result;
}
