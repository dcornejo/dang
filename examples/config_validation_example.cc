// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>

#include <yang/compiler.h>
#include <yang/config_validation.h>
#include <yang/diagnostic.h>
#include <yang/module_resolver.h>
#include <yang/source_file.h>

int main(int argc, char* argv[]) {
  if (argc != 3) {
    std::cerr << "usage: config_validation_example MODEL.yang CONFIG.xml\n";
    return 2;
  }
  std::ifstream model_input(argv[1], std::ios::binary);
  std::ifstream config_input(argv[2], std::ios::binary);
  const std::string model((std::istreambuf_iterator<char>(model_input)), {});
  const std::string xml((std::istreambuf_iterator<char>(config_input)), {});
  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create(argv[1], model, diagnostics);
  yang::FilesystemModuleRepository repository({"."});
  yang::Compiler compiler(repository, diagnostics);
  auto compilation = source ? compiler.Compile(source) : std::nullopt;
  if (!compilation) {
    for (const auto& diagnostic : diagnostics.diagnostics())
      std::cerr << yang::FormatDiagnostic(diagnostic, source.get()) << '\n';
    return 1;
  }
  const auto schema = yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto parsed = yang::config::ParseDatastoreXml(schema, xml);
  if (!parsed.document) {
    for (const auto& finding : parsed.findings)
      std::cerr << finding.instance_path << ": " << finding.message << '\n';
    return 1;
  }
  yang::config::ConfigValidator validator;
  const auto result = validator.Validate({schema, *parsed.document});
  for (const auto& finding : result.findings)
    std::cerr << finding.instance_path << ": " << finding.message << '\n';
  std::cout << (result.valid && result.complete ? "valid\n" : "not valid\n");
  return result.valid && result.complete ? 0 : 1;
}
