// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/leafref.h"
#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct LeafrefPipeline {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;
  std::optional<TypeContext> types;
  std::optional<SchemaContext> schemas;

  void Build(std::string source) {
    ModuleResolver modules(repository, sink);
    module = modules.Resolve(test::Source(std::move(source), sink));
    if (!module) return;
    SymbolResolver symbols(sink); semantics = symbols.Resolve(module);
    if (!semantics) return;
    TypeResolver type_resolver(sink); types = type_resolver.Resolve(*semantics);
    if (!types) return;
    SchemaContextBuilder schema_builder(sink); schemas = schema_builder.Build(*semantics, *types);
  }
};

const SchemaNode& Node(const SchemaTree& tree, std::optional<SchemaNodeId> parent,
                       std::string_view module, std::string_view name) {
  auto id = tree.FindChild(parent, {std::string(module), std::string(name)});
  EXPECT_TRUE(id); return tree.Get(*id);
}

TEST(LeafrefTest, ResolvesRelativePathAndInheritsTargetType) {
  LeafrefPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root {
      leaf target { type uint32; }
      leaf reference { type leafref { path "../target"; } }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);
  LeafrefResolver resolver(pipeline.sink);
  auto leafrefs = resolver.Resolve(*pipeline.schemas);
  ASSERT_TRUE(leafrefs);
  const SchemaTree& tree = pipeline.schemas->root();
  const SchemaNode& root = Node(tree, std::nullopt, "app", "root");
  const SchemaNode& reference = Node(tree, root.id, "app", "reference");
  auto target = leafrefs->Target({pipeline.module.get(), reference.id});
  ASSERT_TRUE(target);
  EXPECT_EQ(tree.Get(target->node).name.local_name, "target");
  auto effective = leafrefs->EffectiveType(*pipeline.schemas,
                                            {pipeline.module.get(), reference.id});
  ASSERT_TRUE(effective);
  EXPECT_EQ(effective->builtin, BuiltinType::kUint32);
}

TEST(LeafrefTest, ResolvesAbsoluteCrossModulePath) {
  LeafrefPipeline pipeline;
  pipeline.repository.Add("base", R"yang(module base {
    namespace "urn:base"; prefix b;
    container root { leaf name { type string; } }
  })yang");
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    import base { prefix b; }
    leaf remote-name { type leafref { path "/b:root/b:name"; } }
  })yang");
  ASSERT_TRUE(pipeline.schemas);
  LeafrefResolver resolver(pipeline.sink);
  auto leafrefs = resolver.Resolve(*pipeline.schemas);
  ASSERT_TRUE(leafrefs);
  const SchemaNode& reference = Node(pipeline.schemas->root(), std::nullopt,
                                     "app", "remote-name");
  auto target = leafrefs->Target({pipeline.module.get(), reference.id});
  ASSERT_TRUE(target);
  EXPECT_EQ(target->tree_module->name, "base");
}

TEST(LeafrefTest, ValidatesCurrentPredicatePaths) {
  LeafrefPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    list user { key id; leaf id { type string; } }
    container selection {
      leaf wanted { type string; }
      leaf selected {
        type leafref { path "/user[id = current()/../wanted]/id"; }
      }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);
  LeafrefResolver resolver(pipeline.sink);
  EXPECT_TRUE(resolver.Resolve(*pipeline.schemas));
}

TEST(LeafrefTest, RejectsNonLeafTargetAndMalformedPath) {
  LeafrefPipeline target_pipeline;
  target_pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container target;
    leaf reference { type leafref { path "/target"; } }
  })yang");
  ASSERT_TRUE(target_pipeline.schemas);
  LeafrefResolver target_resolver(target_pipeline.sink);
  EXPECT_FALSE(target_resolver.Resolve(*target_pipeline.schemas));
  EXPECT_EQ(target_pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidLeafrefTarget);

  LeafrefPipeline malformed;
  malformed.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf reference { type leafref { path "target"; } }
  })yang");
  ASSERT_TRUE(malformed.schemas);
  LeafrefResolver malformed_resolver(malformed.sink);
  EXPECT_FALSE(malformed_resolver.Resolve(*malformed.schemas));
  EXPECT_EQ(malformed.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidLeafrefPath);
}

TEST(LeafrefTest, DetectsLeafrefCycle) {
  LeafrefPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root {
      leaf first { type leafref { path "../second"; } }
      leaf second { type leafref { path "../first"; } }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);
  LeafrefResolver resolver(pipeline.sink);
  EXPECT_FALSE(resolver.Resolve(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code, DiagnosticCode::kLeafrefCycle);
}

TEST(LeafrefTest, RejectsConfigurationReferenceToStateData) {
  LeafrefPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf state { config false; type string; }
    leaf configured { type leafref { path "/state"; } }
  })yang");
  ASSERT_TRUE(pipeline.schemas);
  LeafrefResolver resolver(pipeline.sink);
  EXPECT_FALSE(resolver.Resolve(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidLeafrefTarget);
}

}  // namespace
}  // namespace yang::semantic
