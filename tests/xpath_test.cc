// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/identity_feature.h"
#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "yang/xpath.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct XPathPipeline {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;
  std::optional<TypeContext> types;
  std::optional<IdentityFeatureContext> identity_features;
  std::optional<SchemaContext> schemas;

  void Build(std::string source, bool filter_features = false) {
    ModuleResolver module_resolver(repository, sink);
    module = module_resolver.Resolve(test::Source(std::move(source), sink));
    if (!module) return;
    SymbolResolver symbol_resolver(sink);
    semantics = symbol_resolver.Resolve(module);
    if (!semantics) return;
    TypeResolver type_resolver(sink);
    types = type_resolver.Resolve(*semantics);
    if (!types) return;
    const FeatureSet* features = nullptr;
    if (filter_features) {
      IdentityFeatureResolver feature_resolver(sink);
      identity_features = feature_resolver.Resolve(*semantics);
      if (!identity_features) return;
      features = &identity_features->features;
    }
    SchemaContextBuilder schema_builder(sink);
    schemas = schema_builder.Build(*semantics, *types, features);
  }
};

TEST(XPathTest, ValidatesMustAndWhenWithRelativePaths) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root {
      leaf enabled { type boolean; }
      leaf count { type uint32; }
      must "enabled = 'true' and count > 0";
      leaf value {
        type string;
        when "../enabled = 'true'";
        must "string-length(.) > 0";
      }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  auto context = validator.Validate(*pipeline.schemas);

  ASSERT_TRUE(context);
  EXPECT_EQ(context->expressions().size(), 3U);
  EXPECT_EQ(context->expressions()[0].kind, XPathConstraintKind::kMust);
  EXPECT_FALSE(context->expressions()[0].statically_resolved_nodes.empty());
}

TEST(XPathTest, TreatsChoiceAndCaseAsTransparentInDataPaths) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container key {
      leaf format { type string; }
      choice representation {
        case cleartext {
          leaf value { type string; must "../format"; }
        }
      }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  auto context = validator.Validate(*pipeline.schemas);

  ASSERT_TRUE(context);
  ASSERT_EQ(context->expressions().size(), 1U);
  ASSERT_EQ(context->expressions()[0].statically_resolved_nodes.size(), 1U);
  const SchemaNodeRef& resolved =
      context->expressions()[0].statically_resolved_nodes[0];
  EXPECT_EQ(resolved.tree_module->name, "app");
  EXPECT_EQ(pipeline.schemas->Find(*resolved.tree_module)
                ->Get(resolved.node)
                .name.local_name,
            "format");
}

TEST(XPathTest, ResolvesUnprefixedGroupingPathsInUsingModuleNamespace) {
  XPathPipeline pipeline;
  pipeline.repository.Add("types", R"yang(module types {
    namespace "urn:types"; prefix t;
    grouping key {
      leaf format { type string; }
      choice representation {
        leaf value { type string; must "../format"; }
      }
    }
  })yang");
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    import types { prefix t; }
    container key { uses t:key; }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  auto context = validator.Validate(*pipeline.schemas);

  ASSERT_TRUE(context);
  ASSERT_EQ(context->expressions().size(), 1U);
  ASSERT_EQ(context->expressions()[0].statically_resolved_nodes.size(), 1U);
  EXPECT_EQ(context->expressions()[0]
                .statically_resolved_nodes[0]
                .tree_module->name,
            "app");
}

TEST(XPathTest, ResolvesAbsoluteCrossModulePaths) {
  XPathPipeline pipeline;
  pipeline.repository.Add("base", R"yang(module base {
    namespace "urn:base"; prefix b;
    container root { leaf status { type string; } }
  })yang");
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    import base { prefix b; }
    leaf guard {
      type string;
      must "/b:root/b:status = 'ready'";
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  auto context = validator.Validate(*pipeline.schemas);

  ASSERT_TRUE(context);
  ASSERT_EQ(context->expressions().size(), 1U);
  ASSERT_EQ(context->expressions()[0].statically_resolved_nodes.size(), 1U);
  EXPECT_EQ(context->expressions()[0].statically_resolved_nodes[0]
                .tree_module->name,
            "base");
}

TEST(XPathTest, AcceptsYangFunctionsAndCurrentPath) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root {
      leaf enabled { type boolean; }
      leaf value {
        type string;
        must "not(false()) and current()/../enabled = true()";
      }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_TRUE(validator.Validate(*pipeline.schemas));
}

TEST(XPathTest, RejectsMalformedExpression) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type string; must "string(.) and"; }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_FALSE(validator.Validate(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidXPath);
}

TEST(XPathTest, RejectsUnknownFunction) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type string; must "mystery(.)"; }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_FALSE(validator.Validate(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kUnknownXPathFunction);
}

TEST(XPathTest, RejectsUnknownStaticSchemaPath) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root {
      leaf value { type string; must "../missing = 'x'"; }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_FALSE(validator.Validate(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kUnknownXPathNode);
}

TEST(XPathTest, AllowsConstraintPathToFeaturePrunedNode) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    feature optional;
    container root {
      container optional-data {
        if-feature optional;
        leaf value { type string; }
      }
      leaf guard { type string; must "../optional-data/value"; }
    }
  })yang", true);
  ASSERT_TRUE(pipeline.schemas);
  const auto root = pipeline.schemas->root().FindChild(
      std::nullopt, {"app", "root"});
  ASSERT_TRUE(root);
  ASSERT_FALSE(pipeline.schemas->root().FindChild(
      *root, {"app", "optional-data"}));
  XPathValidator validator(pipeline.sink);
  EXPECT_TRUE(validator.Validate(*pipeline.schemas));
}

TEST(XPathTest, RejectsInvalidFunctionArity) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type string; must "contains(.)"; }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_FALSE(validator.Validate(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidXPathFunctionArity);
}

TEST(XPathTest, RejectsMalformedQualifiedName) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type string; must "bad:name:again = 'x'"; }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_FALSE(validator.Validate(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidXPath);
}

TEST(XPathTest, ResolvesPredicatePathsFromSelectedListNode) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root {
      leaf wanted { type string; }
      list item {
        key "name";
        leaf name { type string; }
      }
      must "count(item[name = current()/wanted]) > 0";
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_TRUE(validator.Validate(*pipeline.schemas));
}

TEST(XPathTest, RejectsUnknownPathInsidePredicateContext) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root {
      list item {
        key "name";
        leaf name { type string; }
      }
      must "item[missing = 'x']";
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);

  XPathValidator validator(pipeline.sink);
  EXPECT_FALSE(validator.Validate(*pipeline.schemas));
  EXPECT_EQ(pipeline.sink.diagnostics().back().code,
            DiagnosticCode::kUnknownXPathNode);
}

TEST(XPathTest, UsesWhenExpressionRetainsInstantiationParentContext) {
  XPathPipeline pipeline;
  pipeline.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    grouping conditional { container payload; }
    container root {
      leaf gate { type string; }
      uses conditional { when "gate = 'open'"; }
    }
  })yang");
  ASSERT_TRUE(pipeline.schemas);
  XPathValidator validator(pipeline.sink);
  EXPECT_TRUE(validator.Validate(*pipeline.schemas));
}

TEST(XPathTest, InfersExpressionTypeAndRejectsWrongNodeSetArgument) {
  XPathPipeline valid;
  valid.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root { leaf value { type string; } must "count(value) > 0"; }
  })yang");
  ASSERT_TRUE(valid.schemas);
  XPathValidator valid_validator(valid.sink);
  auto context = valid_validator.Validate(*valid.schemas);
  ASSERT_TRUE(context);
  ASSERT_EQ(context->expressions().size(), 1U);
  EXPECT_EQ(context->expressions()[0].inferred_type,
            XPathValueType::kBoolean);

  XPathPipeline invalid;
  invalid.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf value { type string; must "count('text') > 0"; }
  })yang");
  ASSERT_TRUE(invalid.schemas);
  XPathValidator invalid_validator(invalid.sink);
  EXPECT_FALSE(invalid_validator.Validate(*invalid.schemas));
  EXPECT_EQ(invalid.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidXPathType);
}

}  // namespace
}  // namespace yang::semantic
