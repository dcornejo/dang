// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

// Minimal production-server driver for the independent ncclient NMDA suite.
// The test controls only listener lifetime; all protocol and datastore behavior
// comes from the normal dangd application and mutual-TLS transport.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include "dangd/application.h"
#include "dangd/tls_transport.h"

namespace {

bool Mark(const std::filesystem::path& marker) {
  std::ofstream output(marker);
  output << "ready\n";
  return output.good();
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc != 4) {
    std::cerr << "usage: nmda-interop-server SOURCE PORT MARKER-DIR\n";
    return 2;
  }
  const std::filesystem::path source = argv[1];
  const std::uint16_t port = static_cast<std::uint16_t>(std::stoul(argv[2]));
  const std::filesystem::path markers = argv[3];
  auto loaded = dangd::Application::Load({
      .model = source / "dangd/examples/appliance.yang",
      .search_paths = {source / "dangd/models"},
      .configuration = source / "dangd/examples/config.xml",
      .nacm_configuration = source / "dangd/examples/nacm.xml",
      .recovery_users = {"alice"}});
  if (!loaded.application) {
    for (const std::string& error : loaded.errors) std::cerr << error << '\n';
    return 1;
  }

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
    server_result =
        dangd::RunTlsServer(*loaded.application, options, diagnostics);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (!Mark(markers / "server-ready")) {
    std::cerr << "cannot publish server readiness\n";
    server.join();
    return 1;
  }
  server.join();
  if (server_result != 0) std::cerr << diagnostics.str();
  return server_result == 0 ? 0 : 1;
}
