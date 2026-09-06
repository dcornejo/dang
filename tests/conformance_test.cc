// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <filesystem>
#include <fstream>
#include <iterator>

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/identity_feature.h"
#include "yang/leafref.h"
#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"

namespace yang::semantic {
namespace {

std::shared_ptr<const SourceFile> LoadFixture(
    const std::filesystem::path& fixture, DiagnosticSink& diagnostics) {
  std::ifstream input(fixture, std::ios::binary);
  if (!input) return nullptr;
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  return SourceFile::Create(fixture.string(), contents, diagnostics);
}

TEST(ConformanceTest, BuildsRfc7950ExampleEffectiveSchema) {
  const std::filesystem::path fixture =
      std::filesystem::path(YANG_TEST_SOURCE_DIR) / "fixtures" /
      "rfc7950-example-system.yang";
  std::ifstream input(fixture, std::ios::binary);
  ASSERT_TRUE(input);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create(fixture.string(), contents, diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  ModuleResolver modules(repository, diagnostics);
  auto module = modules.Resolve(source);
  ASSERT_TRUE(module);
  SymbolResolver symbols(diagnostics);
  auto semantics = symbols.Resolve(module);
  ASSERT_TRUE(semantics);
  TypeResolver type_resolver(diagnostics);
  auto types = type_resolver.Resolve(*semantics);
  ASSERT_TRUE(types);
  SchemaContextBuilder builder(diagnostics);
  auto schemas = builder.Build(*semantics, *types);
  ASSERT_TRUE(schemas);
  EXPECT_EQ(schemas->root().roots().size(), 1U);
  EXPECT_EQ(schemas->root().size(), 9U);
  EXPECT_FALSE(diagnostics.has_errors());
}

TEST(ConformanceTest, HighLevelCompilerRunsCompletePipeline) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("example.yang", R"yang(module example {
    yang-version 1.1;
    namespace "urn:example"; prefix ex;
    identity kind;
    identity ethernet { base kind; }
    leaf selected { type identityref { base kind; } default ethernet; }
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto result = compiler.Compile(source);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->module->name, "example");
  EXPECT_EQ(result->schemas.root().size(), 1U);
  EXPECT_FALSE(diagnostics.has_errors());
}

TEST(ConformanceTest, AppliesXmlSchemaRegularExpressionFixture) {
  const std::filesystem::path fixture =
      std::filesystem::path(YANG_TEST_SOURCE_DIR) / "fixtures" /
      "xml-schema-patterns.yang";
  std::ifstream input(fixture, std::ios::binary);
  ASSERT_TRUE(input);
  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create(fixture.string(), contents, diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto result = compiler.Compile(source);
  ASSERT_TRUE(result);

  const Statement& module =
      result->module->syntax->Get(result->module->syntax->roots().front());
  auto leaf_type = [&](std::string_view name) {
    for (StatementId child_id : module.children) {
      const Statement& child = result->module->syntax->Get(child_id);
      if (child.keyword != "leaf" || child.argument != name) continue;
      const Statement& type = result->module->syntax->Get(child.children.front());
      return result->types.Find(*result->module, type.id);
    }
    return std::shared_ptr<const ResolvedType>{};
  };

  const auto unicode = leaf_type("unicode-label");
  const auto consonants = leaf_type("consonants");
  const auto excluded = leaf_type("excluded");
  ASSERT_TRUE(unicode && consonants && excluded);
  EXPECT_TRUE(ValueMatchesType(*unicode, "M\xC4\x81noa"));
  EXPECT_FALSE(ValueMatchesType(*unicode, "route-1"));
  EXPECT_TRUE(ValueMatchesType(*consonants, "rhythm"));
  EXPECT_FALSE(ValueMatchesType(*consonants, "aloha"));
  EXPECT_TRUE(ValueMatchesType(*excluded, "allowed"));
  EXPECT_FALSE(ValueMatchesType(*excluded, "blocked"));
}

TEST(ConformanceTest, CompilesPinnedIetfInterfacesDependencyClosure) {
  const std::filesystem::path directory =
      std::filesystem::path(YANG_TEST_SOURCE_DIR) / "third_party" / "ietf";
  VectorDiagnosticSink diagnostics;
  auto source = LoadFixture(directory / "ietf-interfaces@2018-02-20.yang",
                            diagnostics);
  ASSERT_TRUE(source);
  FilesystemModuleRepository repository({directory});
  Compiler compiler(repository, diagnostics);
  const auto result = compiler.Compile(source);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->module->name, "ietf-interfaces");
  EXPECT_FALSE(diagnostics.has_errors());
}

TEST(ConformanceTest, CompilesPinnedRfc9644DependencyClosure) {
  const std::filesystem::path directory =
      std::filesystem::path(YANG_PROJECT_SOURCE_DIR) / "dangd" / "models";
  for (const std::string_view module : {"ietf-ssh-common", "ietf-ssh-client",
                                        "ietf-ssh-server"}) {
    SCOPED_TRACE(module);
    VectorDiagnosticSink diagnostics;
    auto source = LoadFixture(
        directory / (std::string(module) + "@2024-10-10.yang"), diagnostics);
    ASSERT_TRUE(source);
    FilesystemModuleRepository repository({directory});
    Compiler compiler(repository, diagnostics);
    const auto result = compiler.Compile(source);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->module->name, module);
    EXPECT_FALSE(diagnostics.has_errors());
  }
}

TEST(ConformanceTest, RejectsEveryMalformedYangCorpusEntry) {
  const std::filesystem::path directory =
      std::filesystem::path(YANG_TEST_SOURCE_DIR) / "corpus" / "yang";
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".yang") continue;
    SCOPED_TRACE(entry.path().filename().string());
    VectorDiagnosticSink diagnostics;
    auto source = LoadFixture(entry.path(), diagnostics);
    if (!source) {
      EXPECT_TRUE(diagnostics.has_errors());
      continue;
    }
    InMemoryModuleRepository repository;
    Compiler compiler(repository, diagnostics);
    EXPECT_FALSE(compiler.Compile(source));
    EXPECT_TRUE(diagnostics.has_errors());
  }
}

}  // namespace
}  // namespace yang::semantic
