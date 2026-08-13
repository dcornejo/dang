// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/identity_feature.h"
#include "yang/module_resolver.h"
#include "yang/semantic_model.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct SemanticPipeline {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;

  void Resolve(std::string source) {
    ModuleResolver modules(repository, sink);
    module = modules.Resolve(test::Source(std::move(source), sink));
    if (!module) return;
    SymbolResolver symbols(sink);
    semantics = symbols.Resolve(module);
  }
};

TEST(IdentityFeatureTest, EvaluatesFeatureExpressionsWithPrecedence) {
  SemanticPipeline pipeline;
  pipeline.Resolve(R"yang(module app {
    yang-version 1.1;
    namespace "urn:app"; prefix app;
    feature a;
    feature b;
    feature c;
    feature selected { if-feature "a or b and not c"; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  IdentityFeatureResolver resolver(pipeline.sink);
  auto result = resolver.Resolve(*pipeline.semantics,
      {{"app", "b"}, {"app", "selected"}});
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->features.IsEnabled("app", "a"));
  EXPECT_TRUE(result->features.IsEnabled("app", "b"));
  EXPECT_TRUE(result->features.IsEnabled("app", "selected"));
}

TEST(IdentityFeatureTest, ResolvesImportedFeatureDependency) {
  SemanticPipeline pipeline;
  pipeline.repository.Add("platform", R"yang(module platform {
    namespace "urn:platform"; prefix p; feature hardware;
  })yang");
  pipeline.Resolve(R"yang(module app {
    namespace "urn:app"; prefix app;
    import platform { prefix p; }
    feature accelerated { if-feature p:hardware; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  IdentityFeatureResolver resolver(pipeline.sink);
  auto result = resolver.Resolve(*pipeline.semantics,
      {{"platform", "hardware"}, {"app", "accelerated"}});
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->features.IsEnabled("app", "accelerated"));
}

TEST(IdentityFeatureTest, DetectsFeatureCycleAndMalformedExpression) {
  SemanticPipeline cycle;
  cycle.Resolve(R"yang(module app {
    namespace "urn:app"; prefix app;
    feature a { if-feature b; }
    feature b { if-feature a; }
  })yang");
  ASSERT_TRUE(cycle.semantics);
  IdentityFeatureResolver cycle_resolver(cycle.sink);
  EXPECT_FALSE(cycle_resolver.Resolve(*cycle.semantics));

  SemanticPipeline malformed;
  malformed.Resolve(R"yang(module app {
    yang-version 1.1;
    namespace "urn:app"; prefix app;
    feature a;
    feature b { if-feature "a and"; }
  })yang");
  ASSERT_TRUE(malformed.semantics);
  IdentityFeatureResolver malformed_resolver(malformed.sink);
  EXPECT_FALSE(malformed_resolver.Resolve(*malformed.semantics));
  EXPECT_EQ(malformed.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidFeatureExpression);
}

TEST(IdentityFeatureTest, BuildsTransitiveMultipleBaseIdentityGraph) {
  SemanticPipeline pipeline;
  pipeline.repository.Add("base", R"yang(module base {
    namespace "urn:base"; prefix b;
    identity root;
    identity secondary;
  })yang");
  pipeline.Resolve(R"yang(module app {
    yang-version 1.1;
    namespace "urn:app"; prefix app;
    import base { prefix b; }
    identity child { base b:root; base b:secondary; }
    identity grandchild { base child; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  IdentityFeatureResolver resolver(pipeline.sink);
  auto result = resolver.Resolve(*pipeline.semantics);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->identities.IsDerivedFrom({"app", "grandchild"},
                                                {"base", "root"}));
  EXPECT_TRUE(result->identities.IsDerivedFrom({"app", "child"},
                                                {"base", "secondary"}));
  const auto derived = result->identities.DerivedFrom({"base", "root"});
  EXPECT_EQ(derived.size(), 2);
}

TEST(IdentityFeatureTest, DetectsIdentityInheritanceCycle) {
  SemanticPipeline pipeline;
  pipeline.Resolve(R"yang(module app {
    namespace "urn:app"; prefix app;
    identity first { base second; }
    identity second { base first; }
  })yang");
  ASSERT_TRUE(pipeline.semantics);
  IdentityFeatureResolver resolver(pipeline.sink);
  EXPECT_FALSE(resolver.Resolve(*pipeline.semantics));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kIdentityCycle);
}

}  // namespace
}  // namespace yang::semantic
