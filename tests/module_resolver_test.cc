// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/module_resolver.h"
#include "test_support.h"

namespace yang {
namespace {

std::shared_ptr<const SourceFile> Root(std::string text, VectorDiagnosticSink& sink) {
  return test::Source(std::move(text), sink);
}

TEST(ModuleResolverTest, ResolvesLatestImportAndBuildsPrefixTable) {
  InMemoryModuleRepository repository;
  repository.Add("types", R"yang(module types {
    namespace "urn:types:2024"; prefix t; revision 2024-01-01;
  })yang", "2024-01-01");
  repository.Add("types", R"yang(module types {
    namespace "urn:types:2025"; prefix t; revision 2025-01-01;
  })yang", "2025-01-01");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto module = resolver.Resolve(Root(R"yang(module app {
    namespace "urn:app"; prefix app;
    import types { prefix types; }
  })yang", sink));
  ASSERT_TRUE(module);
  ASSERT_TRUE(module->imports.contains("types"));
  EXPECT_EQ(module->imports.at("types")->revision, "2025-01-01");
  EXPECT_EQ(module->imports.at("types")->namespace_uri, "urn:types:2025");
  EXPECT_FALSE(sink.has_errors());
}

TEST(ModuleResolverTest, HonorsRevisionDate) {
  InMemoryModuleRepository repository;
  repository.Add("types", R"yang(module types {
    namespace "urn:types"; prefix t; revision 2024-01-01;
  })yang", "2024-01-01");
  repository.Add("types", R"yang(module types {
    namespace "urn:types"; prefix t; revision 2025-01-01;
  })yang", "2025-01-01");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto module = resolver.Resolve(Root(R"yang(module app {
    namespace "urn:app"; prefix app;
    import types { prefix t; revision-date 2024-01-01; }
  })yang", sink));
  ASSERT_TRUE(module);
  EXPECT_EQ(module->imports.at("t")->revision, "2024-01-01");
}

TEST(ModuleResolverTest, ResolvesIncludedSubmodule) {
  InMemoryModuleRepository repository;
  repository.Add("app-data", R"yang(submodule app-data {
    belongs-to app { prefix app; }
    leaf shared { type string; }
  })yang");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto module = resolver.Resolve(Root(R"yang(module app {
    namespace "urn:app"; prefix app; include app-data;
  })yang", sink));
  ASSERT_TRUE(module);
  ASSERT_EQ(module->includes.size(), 1);
  EXPECT_EQ(module->includes[0]->name, "app-data");
  EXPECT_EQ(module->includes[0]->kind, ModuleKind::kSubmodule);
}

TEST(ModuleResolverTest, RejectsBelongsToMismatch) {
  InMemoryModuleRepository repository;
  repository.Add("data", R"yang(submodule data {
    belongs-to different { prefix d; }
  })yang");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  EXPECT_FALSE(resolver.Resolve(Root(R"yang(module app {
    namespace "urn:app"; prefix app; include data;
  })yang", sink)));
  EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kBelongsToMismatch);
}

TEST(ModuleResolverTest, ReportsMissingModule) {
  InMemoryModuleRepository repository;
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  EXPECT_FALSE(resolver.Resolve(Root(R"yang(module app {
    namespace "urn:app"; prefix app; import absent { prefix a; }
  })yang", sink)));
  EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kModuleNotFound);
}

TEST(ModuleResolverTest, DetectsDependencyCycle) {
  InMemoryModuleRepository repository;
  repository.Add("b", R"yang(module b {
    namespace "urn:b"; prefix b; import a { prefix a; }
  })yang");
  repository.Add("a", R"yang(module a {
    namespace "urn:a"; prefix a; import b { prefix b; }
  })yang");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  EXPECT_FALSE(resolver.Resolve(repository.Load("a", std::nullopt, ModuleKind::kModule, sink)));
  bool found_cycle = false;
  for (const auto& diagnostic : sink.diagnostics()) {
    found_cycle |= diagnostic.code == DiagnosticCode::kDependencyCycle;
  }
  EXPECT_TRUE(found_cycle);
}

TEST(ModuleResolverTest, ReusesCachedDependency) {
  InMemoryModuleRepository repository;
  repository.Add("shared", "module shared { namespace 'urn:s'; prefix s; }");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto first = resolver.Resolve(Root(R"yang(module first {
    namespace "urn:first"; prefix f; import shared { prefix s; }
  })yang", sink));
  auto second = resolver.Resolve(Root(R"yang(module second {
    namespace "urn:second"; prefix s2; import shared { prefix shared; }
  })yang", sink));
  ASSERT_TRUE(first); ASSERT_TRUE(second);
  EXPECT_EQ(first->imports.at("s").get(), second->imports.at("shared").get());
}

}  // namespace
}  // namespace yang
