// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/identity_feature.h"
#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct FeatureSchemaPipeline {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;
  std::optional<TypeContext> types;

  void Parse(std::string source) {
    ModuleResolver resolver(repository, sink);
    module = resolver.Resolve(test::Source(std::move(source), sink));
    if (!module) return;
    SymbolResolver symbols(sink);
    semantics = symbols.Resolve(module);
    if (!semantics) return;
    TypeResolver type_resolver(sink);
    types = type_resolver.Resolve(*semantics);
  }
};

TEST(FeatureSchemaTest, PrunesNodesForDisabledFeature) {
  FeatureSchemaPipeline pipeline;
  pipeline.Parse(R"yang(module app {
    namespace "urn:app"; prefix app;
    feature advanced;
    container basic;
    container optional { if-feature advanced; }
  })yang");
  ASSERT_TRUE(pipeline.types);
  IdentityFeatureResolver feature_resolver(pipeline.sink);
  auto identity_features = feature_resolver.Resolve(*pipeline.semantics);
  ASSERT_TRUE(identity_features);

  SchemaContextBuilder builder(pipeline.sink);
  auto schemas = builder.Build(*pipeline.semantics, *pipeline.types,
                               &identity_features->features);

  ASSERT_TRUE(schemas);
  EXPECT_TRUE(schemas->root().FindChild(std::nullopt, {"app", "basic"}));
  EXPECT_FALSE(schemas->root().FindChild(std::nullopt, {"app", "optional"}));
}

TEST(FeatureSchemaTest, ExpandsEnabledUsesAndAugment) {
  FeatureSchemaPipeline pipeline;
  pipeline.Parse(R"yang(module app {
    namespace "urn:app"; prefix app;
    feature advanced;
    grouping extras { leaf from-uses { type string; } }
    container root {
      uses extras { if-feature advanced; }
    }
    augment "/root" {
      if-feature advanced;
      leaf from-augment { type string; }
    }
  })yang");
  ASSERT_TRUE(pipeline.types);
  IdentityFeatureResolver feature_resolver(pipeline.sink);
  auto identity_features = feature_resolver.Resolve(
      *pipeline.semantics, {{"app", "advanced"}});
  ASSERT_TRUE(identity_features);

  SchemaContextBuilder builder(pipeline.sink);
  auto schemas = builder.Build(*pipeline.semantics, *pipeline.types,
                               &identity_features->features);

  ASSERT_TRUE(schemas);
  const auto root = schemas->root().FindChild(std::nullopt, {"app", "root"});
  ASSERT_TRUE(root);
  EXPECT_TRUE(schemas->root().FindChild(*root, {"app", "from-uses"}));
  EXPECT_TRUE(schemas->root().FindChild(*root, {"app", "from-augment"}));
}

}  // namespace
}  // namespace yang::semantic
