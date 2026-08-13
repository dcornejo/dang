// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/deviation.h"
#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "yang/xpath.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct DeviationPipeline {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;
  std::optional<TypeContext> types;
  std::optional<SchemaContext> schemas;

  void Build(std::string source) {
    ModuleResolver module_resolver(repository, sink);
    module = module_resolver.Resolve(test::Source(std::move(source), sink));
    if (!module) return;
    SymbolResolver symbol_resolver(sink);
    semantics = symbol_resolver.Resolve(module);
    if (!semantics) return;
    TypeResolver type_resolver(sink);
    types = type_resolver.Resolve(*semantics);
    if (!types) return;
    SchemaContextBuilder schema_builder(sink);
    schemas = schema_builder.Build(*semantics, *types);
  }
};

TEST(DeviationTest, AppliesNotSupportedAddReplaceAndDelete) {
  DeviationPipeline pipeline;
  pipeline.repository.Add("base", R"yang(module base {
    namespace "urn:base"; prefix b;
    container root {
      leaf gone { type string; }
      leaf value {
        type string;
        units "old";
        default "7";
        config true;
      }
    }
  })yang");
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    import base { prefix b; }
    deviation "/b:root/b:gone" { deviate not-supported; }
    deviation "/b:root/b:value" {
      deviate delete { units "old"; default "7"; }
      deviate replace { type uint32; config false; }
      deviate add { mandatory true; }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  DeviationApplier applier(pipeline.sink);
  ASSERT_TRUE(applier.Apply(*pipeline.schemas, *pipeline.types));

  const ResolvedModule& base = *pipeline.module->imports.at("b");
  const SchemaTree* tree = pipeline.schemas->Find(base);
  ASSERT_NE(tree, nullptr);
  const auto root = tree->FindChild(std::nullopt, {"base", "root"});
  ASSERT_TRUE(root);
  EXPECT_FALSE(tree->FindChild(*root, {"base", "gone"}));
  const auto value = tree->FindChild(*root, {"base", "value"});
  ASSERT_TRUE(value);
  const SchemaNode& node = tree->Get(*value);
  EXPECT_FALSE(node.default_value);
  EXPECT_FALSE(node.units);
  EXPECT_EQ(node.declared_config, false);
  EXPECT_TRUE(node.mandatory);
  ASSERT_TRUE(node.type);
  EXPECT_EQ(node.type->builtin, BuiltinType::kUint32);
}

TEST(DeviationTest, RejectsAddingExistingProperty) {
  DeviationPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type string; config true; }
    deviation "/value" { deviate add { config false; } }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  DeviationApplier applier(pipeline.sink);
  EXPECT_FALSE(applier.Apply(*pipeline.schemas, *pipeline.types));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidDeviate);
}

TEST(DeviationTest, MutatesEffectiveMustAndUniqueConstraints) {
  DeviationPipeline pipeline;
  pipeline.repository.Add("base", R"yang(module base {
    namespace "urn:base"; prefix b;
    list item {
      key "id";
      unique "value";
      must "value";
      leaf id { type string; }
      leaf value { type string; }
    }
  })yang");
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    import base { prefix b; }
    deviation "/b:item" {
      deviate delete { must "value"; unique "b:value"; }
      deviate add { must "b:id"; unique "b:id"; }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  DeviationApplier applier(pipeline.sink);
  ASSERT_TRUE(applier.Apply(*pipeline.schemas, *pipeline.types));

  const ResolvedModule& base = *pipeline.module->imports.at("b");
  const SchemaTree* tree = pipeline.schemas->Find(base);
  ASSERT_NE(tree, nullptr);
  const auto item = tree->FindChild(std::nullopt, {"base", "item"});
  ASSERT_TRUE(item);
  const SchemaNode& node = tree->Get(*item);
  ASSERT_EQ(node.must_constraints.size(), 1U);
  ASSERT_EQ(node.unique.size(), 1U);
  ASSERT_EQ(node.unique[0].size(), 1U);
  EXPECT_EQ(tree->Get(node.unique[0][0]).name.local_name, "id");

  XPathValidator xpath(pipeline.sink);
  auto expressions = xpath.Validate(*pipeline.schemas);
  ASSERT_TRUE(expressions);
  ASSERT_EQ(expressions->expressions().size(), 1U);
  EXPECT_EQ(expressions->expressions()[0].expression, "b:id");
}

TEST(DeviationTest, PreservesExplicitUnboundedAndValidatesTargetKind) {
  DeviationPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf-list values { type string; max-elements 4; }
    deviation "/values" {
      deviate replace { max-elements unbounded; }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);
  DeviationApplier applier(pipeline.sink);
  ASSERT_TRUE(applier.Apply(*pipeline.schemas, *pipeline.types));
  const auto values = pipeline.schemas->root().FindChild(
      std::nullopt, {"app", "values"});
  ASSERT_TRUE(values);
  EXPECT_TRUE(pipeline.schemas->root().Get(*values).max_elements_unbounded);

  DeviationPipeline invalid;
  invalid.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root;
    deviation "/root" { deviate add { units "seconds"; } }
  })yang");
  ASSERT_TRUE(invalid.schemas);
  DeviationApplier invalid_applier(invalid.sink);
  EXPECT_FALSE(invalid_applier.Apply(*invalid.schemas, *invalid.types));
  EXPECT_EQ(invalid.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidDeviate);
}

}  // namespace
}  // namespace yang::semantic
