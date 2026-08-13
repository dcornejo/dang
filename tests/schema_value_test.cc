// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/identity_feature.h"
#include "yang/leafref.h"
#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/schema_value.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct SchemaValuePipeline {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;
  std::optional<TypeContext> types;
  std::optional<IdentityFeatureContext> identities;
  std::optional<SchemaContext> schemas;
  std::optional<LeafrefContext> leafrefs;

  void Build(std::string source) {
    ModuleResolver modules(repository, sink);
    module = modules.Resolve(test::Source(std::move(source), sink));
    if (!module) return;
    SymbolResolver symbols(sink);
    semantics = symbols.Resolve(module);
    if (!semantics) return;
    TypeResolver type_resolver(sink);
    types = type_resolver.Resolve(*semantics);
    if (!types) return;
    IdentityFeatureResolver identity_resolver(sink);
    identities = identity_resolver.Resolve(*semantics);
    if (!identities) return;
    SchemaContextBuilder schema_builder(sink);
    schemas = schema_builder.Build(*semantics, *types);
    if (!schemas) return;
    LeafrefResolver leafref_resolver(sink);
    leafrefs = leafref_resolver.Resolve(*schemas);
  }
};

TEST(SchemaValueTest, AcceptsResolvedIdentityrefAndLeafrefDefaults) {
  SchemaValuePipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    identity animal;
    identity dog { base animal; }
    leaf kind { type identityref { base animal; } default dog; }
    container root {
      leaf target { type uint8; }
      leaf reference {
        type leafref { path "../target"; }
        default "7";
      }
    }
  })yang");
  ASSERT_TRUE(pipeline.leafrefs);

  SchemaValueValidator validator(pipeline.sink);
  EXPECT_TRUE(validator.Validate(*pipeline.schemas,
                                 pipeline.identities->identities,
                                 *pipeline.leafrefs));
}

TEST(SchemaValueTest, RejectsDefaultsOutsideResolvedValueSpace) {
  SchemaValuePipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    identity animal;
    identity unrelated;
    leaf kind {
      type identityref { base animal; }
      default unrelated;
    }
    container root {
      leaf target { type uint8; }
      leaf reference {
        type leafref { path "../target"; }
        default "999";
      }
    }
  })yang");
  ASSERT_TRUE(pipeline.leafrefs);

  SchemaValueValidator validator(pipeline.sink);
  EXPECT_FALSE(validator.Validate(*pipeline.schemas,
                                  pipeline.identities->identities,
                                  *pipeline.leafrefs));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidDefaultValue);
}

}  // namespace
}  // namespace yang::semantic
