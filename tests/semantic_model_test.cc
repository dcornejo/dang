// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/module_resolver.h"
#include "yang/semantic_model.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

std::shared_ptr<const ResolvedModule> ResolveRoot(
    InMemoryModuleRepository& repository, std::string source,
    VectorDiagnosticSink& sink) {
  ModuleResolver resolver(repository, sink);
  return resolver.Resolve(test::Source(std::move(source), sink));
}

TEST(SemanticModelTest, ResolvesLocalImportedAndLexicalSymbols) {
  InMemoryModuleRepository repository;
  repository.Add("base", R"yang(module base {
    namespace "urn:base"; prefix b;
    typedef remote-type { type string; }
    grouping remote-group { leaf remote { type remote-type; } }
    feature remote-feature;
    identity root;
    extension note;
  })yang");
  VectorDiagnosticSink sink;
  auto module = ResolveRoot(repository, R"yang(module app {
    yang-version 1.1;
    namespace "urn:app"; prefix app;
    import base { prefix b; }
    typedef local-type { type b:remote-type; }
    feature local-feature;
    identity child { base b:root; }
    container system {
      typedef nested-type { type local-type; }
      leaf value { if-feature "local-feature and b:remote-feature"; type nested-type; }
      uses b:remote-group;
      b:note;
    }
  })yang", sink);
  ASSERT_TRUE(module);
  SymbolResolver resolver(sink);
  auto semantics = resolver.Resolve(module);
  ASSERT_TRUE(semantics);
  ASSERT_TRUE(semantics->root());
  EXPECT_TRUE(semantics->root()->Find(SymbolKind::kTypedef, "local-type"));
  EXPECT_TRUE(semantics->root()->Find(SymbolKind::kFeature, "local-feature"));
  auto base = semantics->FindModule("base");
  ASSERT_TRUE(base);
  EXPECT_TRUE(base->Find(SymbolKind::kGrouping, "remote-group"));
  EXPECT_FALSE(sink.has_errors());
}

TEST(SemanticModelTest, MergesSubmoduleDeclarations) {
  InMemoryModuleRepository repository;
  repository.Add("app-types", R"yang(submodule app-types {
    belongs-to app { prefix app; }
    typedef shared-type { type string; }
    grouping shared-group { leaf x { type shared-type; } }
  })yang");
  VectorDiagnosticSink sink;
  auto module = ResolveRoot(repository, R"yang(module app {
    namespace "urn:app"; prefix app;
    include app-types;
    leaf value { type shared-type; }
    uses shared-group;
  })yang", sink);
  ASSERT_TRUE(module);
  SymbolResolver resolver(sink);
  auto semantics = resolver.Resolve(module);
  ASSERT_TRUE(semantics);
  EXPECT_TRUE(semantics->root()->Find(SymbolKind::kTypedef, "shared-type"));
  EXPECT_TRUE(semantics->root()->Find(SymbolKind::kGrouping, "shared-group"));
}

TEST(SemanticModelTest, ReportsDuplicateSymbolsAcrossIncludes) {
  InMemoryModuleRepository repository;
  repository.Add("part", R"yang(submodule part {
    belongs-to app { prefix app; }
    typedef duplicate { type string; }
  })yang");
  VectorDiagnosticSink sink;
  auto module = ResolveRoot(repository, R"yang(module app {
    namespace "urn:app"; prefix app; include part;
    typedef duplicate { type string; }
  })yang", sink);
  ASSERT_TRUE(module);
  SymbolResolver resolver(sink);
  EXPECT_FALSE(resolver.Resolve(module));
  EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kDuplicateSymbol);
}

TEST(SemanticModelTest, ReportsUnknownReferences) {
  InMemoryModuleRepository repository;
  VectorDiagnosticSink sink;
  auto module = ResolveRoot(repository, R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type missing-type; }
    uses missing-group;
    identity child { base missing-identity; }
    container guarded { if-feature missing-feature; }
  })yang", sink);
  ASSERT_TRUE(module);
  SymbolResolver resolver(sink);
  EXPECT_FALSE(resolver.Resolve(module));
  std::size_t unknown_count = 0;
  for (const auto& diagnostic : sink.diagnostics()) {
    if (diagnostic.code == DiagnosticCode::kUnknownSymbol) ++unknown_count;
  }
  EXPECT_EQ(unknown_count, 4);
}

TEST(SemanticModelTest, ReportsUnknownQualifiedPrefix) {
  InMemoryModuleRepository repository;
  VectorDiagnosticSink sink;
  auto module = ResolveRoot(repository, R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type absent:text; }
  })yang", sink);
  ASSERT_TRUE(module);
  SymbolResolver resolver(sink);
  EXPECT_FALSE(resolver.Resolve(module));
  EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kUnknownPrefix);
}

}  // namespace
}  // namespace yang::semantic
