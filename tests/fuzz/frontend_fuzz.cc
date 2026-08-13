// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "yang/config_validation.h"
#include "yang/lexer.h"
#include "yang/module_resolver.h"
#include "yang/parser.h"
#include "yang/source_file.h"
#include "yang/utf8.h"
#include "yang/yin_document.h"

namespace {

constexpr std::size_t kMaximumFuzzInput = 1024 * 1024;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (size == 0 || size > kMaximumFuzzInput) return 0;
  const std::uint8_t mode = data[0] % 5;
  const std::string input(reinterpret_cast<const char*>(data + 1), size - 1);
  yang::VectorDiagnosticSink diagnostics;

  if (mode == 0) {
    std::string_view remaining = input;
    while (!remaining.empty()) {
      const auto decoded = yang::utf8::Decode(remaining);
      if (!decoded) break;
      remaining.remove_prefix(decoded->byte_count);
    }
    return 0;
  }

  if (mode == 3) {
    pugi::xml_document document;
    if (document.load_buffer(input.data(), input.size())) {
      yang::InMemoryModuleRepository repository;
      (void)yang::config::RuntimeSchemaBuilder::FromYin(
          document, repository, diagnostics);
    }
    return 0;
  }

  if (mode == 4) {
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if (!json.is_discarded()) {
      (void)yang::YinDocument::FromJson(json, diagnostics);
    }
    return 0;
  }

  const auto source =
      yang::SourceFile::Create("fuzz.yang", input, diagnostics);
  if (!source) return 0;
  if (mode == 1) {
    yang::Lexer lexer(source, diagnostics);
    while (lexer.Next().kind != yang::TokenKind::kEndOfFile) {
    }
  } else {
    yang::Parser parser(source, diagnostics);
    (void)parser.Parse();
  }
  return 0;
}
