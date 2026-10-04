// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include "dangd/tls_transport.h"
#include "yang/resource_limits.h"

namespace {
void Usage() {
  std::cerr << "usage: dangctl --host HOST [--port PORT] --cert FILE "
               "--key FILE --ca FILE [--edit-config FILE "
               "[--default-operation merge|replace|none]]\n";
}

std::optional<std::string> ReadBounded(std::istream& input,
                                       std::string* error) {
  std::string result;
  std::array<char, 4096> buffer{};
  const std::size_t limit = yang::DefaultResourceLimits().maximum_xml_bytes;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      if (result.size() + static_cast<std::size_t>(count) > limit) {
        *error = "configuration exceeds the XML byte limit";
        return std::nullopt;
      }
      result.append(buffer.data(), static_cast<std::size_t>(count));
    }
  }
  if (!input.eof()) {
    *error = "cannot read configuration";
    return std::nullopt;
  }
  return result;
}
}  // namespace

int main(int argc, char* argv[]) {
  dangd::TlsClientOptions options;
  std::optional<std::string> edit_configuration;
  dangd::EditDefaultOperation default_operation =
      dangd::EditDefaultOperation::kMerge;
  bool default_operation_set = false;
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
    else if (argument == "--edit-config" && index + 1 < argc)
      edit_configuration = argv[++index];
    else if (argument == "--default-operation" && index + 1 < argc) {
      const std::string operation = argv[++index];
      if (operation == "merge")
        default_operation = dangd::EditDefaultOperation::kMerge;
      else if (operation == "replace")
        default_operation = dangd::EditDefaultOperation::kReplace;
      else if (operation == "none")
        default_operation = dangd::EditDefaultOperation::kNone;
      else {
        Usage();
        return 2;
      }
      default_operation_set = true;
    }
    else {
      Usage();
      return 2;
    }
  }
  if (options.certificate.empty() || options.private_key.empty() ||
      options.trust_anchor.empty() ||
      (default_operation_set && !edit_configuration)) {
    Usage();
    return 2;
  }
  if (edit_configuration) {
    std::ifstream file;
    std::istream* input = &std::cin;
    if (*edit_configuration != "-") {
      file.open(*edit_configuration, std::ios::binary);
      if (!file) {
        std::cerr << "dangctl: cannot open configuration file\n";
        return 1;
      }
      input = &file;
    }
    std::string error;
    const auto configuration = ReadBounded(*input, &error);
    if (!configuration) {
      std::cerr << "dangctl: " << error << '\n';
      return 1;
    }
    return dangd::RunTlsConfigurationTransaction(
        options, *configuration, default_operation, std::cout, std::cerr);
  }
  return dangd::RunTlsClient(options, std::cin, std::cout, std::cerr);
}
