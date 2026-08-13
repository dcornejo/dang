// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "yang/deviation.h"
#include "yang/extension.h"
#include "yang/identity_feature.h"
#include "yang/leafref.h"
#include "yang/module_resolver.h"
#include "yang/resource_limits.h"
#include "yang/schema_tree.h"
#include "yang/schema_value.h"
#include "yang/semantic_model.h"
#include "yang/source_file.h"
#include "yang/type_system.h"
#include "yang/xpath.h"
#include "yang/yin_document.h"

namespace {

enum class OutputMode { kValidate, kYin, kYinJson, kEffectiveYin };

void PrintUsage() {
  std::cerr << "usage: yangc [--search DIR] [--yin|--yin-json|--effective-yin] FILE\n";
}

void PrintDiagnostics(const yang::VectorDiagnosticSink& diagnostics,
                      const yang::SourceFile* source) {
  for (const yang::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    std::cerr << yang::FormatDiagnostic(diagnostic, source) << '\n';
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  OutputMode mode = OutputMode::kValidate;
  std::vector<std::filesystem::path> search_paths;
  std::filesystem::path input_path;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--search" && index + 1 < argc) {
      search_paths.emplace_back(argv[++index]);
    } else if (argument == "--yin") {
      mode = OutputMode::kYin;
    } else if (argument == "--yin-json") {
      mode = OutputMode::kYinJson;
    } else if (argument == "--effective-yin") {
      mode = OutputMode::kEffectiveYin;
    } else if (!argument.empty() && argument.front() == '-') {
      PrintUsage();
      return 2;
    } else if (input_path.empty()) {
      input_path = argument;
    } else {
      PrintUsage();
      return 2;
    }
  }
  if (input_path.empty()) {
    PrintUsage();
    return 2;
  }

  std::error_code size_error;
  const std::uintmax_t input_size =
      std::filesystem::file_size(input_path, size_error);
  if (!size_error &&
      input_size > yang::DefaultResourceLimits().maximum_source_bytes) {
    std::cerr << "yangc: input exceeds the YANG source byte limit\n";
    return 1;
  }
  std::ifstream input(input_path, std::ios::binary);
  if (!input) {
    std::cerr << "yangc: cannot open " << input_path << '\n';
    return 2;
  }
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create(input_path.string(), contents,
                                         diagnostics);
  if (!source) {
    PrintDiagnostics(diagnostics, nullptr);
    return 1;
  }
  search_paths.insert(search_paths.begin(), input_path.parent_path());
  yang::FilesystemModuleRepository repository(std::move(search_paths));
  yang::ModuleResolver module_resolver(repository, diagnostics);
  auto module = module_resolver.Resolve(source);
  if (!module) {
    PrintDiagnostics(diagnostics, source.get());
    return 1;
  }

  yang::semantic::SymbolResolver symbol_resolver(diagnostics);
  auto semantics = symbol_resolver.Resolve(module);
  yang::semantic::TypeResolver type_resolver(diagnostics);
  auto types = semantics ? type_resolver.Resolve(*semantics) : std::nullopt;
  yang::semantic::IdentityFeatureResolver identity_resolver(diagnostics);
  auto identities = semantics ? identity_resolver.Resolve(*semantics)
                              : std::nullopt;
  yang::semantic::ExtensionResolver extension_resolver(diagnostics);
  auto extensions = semantics ? extension_resolver.Resolve(*semantics)
                              : std::nullopt;
  yang::semantic::SchemaContextBuilder schema_builder(diagnostics);
  auto schemas = semantics && types
      ? schema_builder.Build(*semantics, *types) : std::nullopt;
  bool valid = semantics && types && identities && extensions && schemas;
  if (valid) {
    yang::semantic::DeviationApplier deviations(diagnostics);
    valid = deviations.Apply(*schemas, *types);
  }
  std::optional<yang::semantic::LeafrefContext> leafrefs;
  if (valid) {
    yang::semantic::LeafrefResolver resolver(diagnostics);
    leafrefs = resolver.Resolve(*schemas);
    valid = leafrefs.has_value();
  }
  if (valid) {
    yang::semantic::SchemaValueValidator values(diagnostics);
    valid = values.Validate(*schemas, identities->identities, *leafrefs);
  }
  if (valid) {
    yang::semantic::XPathValidator xpath(diagnostics);
    valid = xpath.Validate(*schemas).has_value();
  }
  if (!valid || diagnostics.has_errors()) {
    PrintDiagnostics(diagnostics, source.get());
    return 1;
  }

  yang::YinConverter converter(diagnostics);
  std::optional<yang::YinDocument> yin;
  if (mode == OutputMode::kEffectiveYin) {
    yin = converter.ConvertEffective(*schemas, *module);
  } else if (mode == OutputMode::kYin || mode == OutputMode::kYinJson) {
    yin = converter.Convert(*module);
  }
  if ((mode != OutputMode::kValidate) && !yin) {
    PrintDiagnostics(diagnostics, source.get());
    return 1;
  }
  if (mode == OutputMode::kYinJson) {
    std::cout << yin->ToJson().dump(2) << '\n';
  } else if (yin) {
    std::cout << yin->ToString();
  }
  return 0;
}
