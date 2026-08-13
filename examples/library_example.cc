// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "yang/compiler.h"
#include "yang/yin_document.h"

int main() {
  std::ifstream input("examples/example.yang", std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create("examples/example.yang", text,
                                         diagnostics);
  yang::FilesystemModuleRepository repository({"examples"});
  yang::Compiler compiler(repository, diagnostics);
  auto result = source ? compiler.Compile(source) : std::nullopt;
  if (!result) {
    for (const auto& diagnostic : diagnostics.diagnostics()) {
      std::cerr << yang::FormatDiagnostic(diagnostic, source.get()) << '\n';
    }
    return 1;
  }

  yang::YinConverter yin_converter(diagnostics);
  auto yin = yin_converter.ConvertEffective(result->schemas, *result->module);
  if (!yin) return 1;
  std::cout << yin->ToString();
  return 0;
}
