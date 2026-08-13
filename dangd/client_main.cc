// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <iostream>
#include <string>

#include "dangd/tls_transport.h"

namespace {
void Usage() {
  std::cerr << "usage: dangctl --host HOST [--port PORT] --cert FILE "
               "--key FILE --ca FILE\n";
}
}  // namespace

int main(int argc, char* argv[]) {
  dangd::TlsClientOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--host" && index + 1 < argc)
      options.host = argv[++index];
    else if (argument == "--port" && index + 1 < argc) {
      try {
        const unsigned long port = std::stoul(argv[++index]);
        if (port == 0 || port > UINT16_MAX) throw std::out_of_range("port");
        options.port = static_cast<std::uint16_t>(port);
      } catch (const std::exception&) {
        Usage();
        return 2;
      }
    } else if (argument == "--cert" && index + 1 < argc)
      options.certificate = argv[++index];
    else if (argument == "--key" && index + 1 < argc)
      options.private_key = argv[++index];
    else if (argument == "--ca" && index + 1 < argc)
      options.trust_anchor = argv[++index];
    else {
      Usage();
      return 2;
    }
  }
  if (options.certificate.empty() || options.private_key.empty() ||
      options.trust_anchor.empty()) {
    Usage();
    return 2;
  }
  return dangd::RunTlsClient(options, std::cin, std::cout, std::cerr);
}
