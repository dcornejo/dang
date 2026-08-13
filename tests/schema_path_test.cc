// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/module_resolver.h"
#include "yang/schema_path.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

TEST(SchemaPathTest, ParsesAbsoluteAndQualifiedSegments) {
  VectorDiagnosticSink sink;
  SchemaPathParser parser(sink);
  auto path = parser.Parse("/if:interfaces/interface/name",
                           SchemaPathKind::kAbsolute, {});
  ASSERT_TRUE(path);
  ASSERT_EQ(path->segments.size(), 3);
  EXPECT_EQ(path->segments[0].prefix, "if");
  EXPECT_EQ(path->segments[0].identifier, "interfaces");
  EXPECT_FALSE(path->segments[1].prefix);
}

TEST(SchemaPathTest, RejectsWrongFormAndEmptySegments) {
  VectorDiagnosticSink sink;
  SchemaPathParser parser(sink);
  EXPECT_FALSE(parser.Parse("relative", SchemaPathKind::kAbsolute, {}));
  EXPECT_FALSE(parser.Parse("a//b", SchemaPathKind::kDescendant, {}));
  EXPECT_FALSE(parser.Parse("/a/", SchemaPathKind::kAbsolute, {}));
  EXPECT_EQ(sink.diagnostics().size(), 3);
}

TEST(SchemaPathTest, ResolvesAbsoluteAndTransparentImplicitCasePaths) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  ModuleResolver modules(repository, sink);
  auto module = modules.Resolve(test::Source(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root { choice mode { leaf automatic { type string; } } }
  })yang", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbols(sink);
  auto semantics = symbols.Resolve(module);
  ASSERT_TRUE(semantics);
  TypeResolver type_resolver(sink);
  auto types = type_resolver.Resolve(*semantics);
  ASSERT_TRUE(types);
  SchemaBuilder builder(sink);
  auto tree = builder.Build(*semantics, *types);
  ASSERT_TRUE(tree);
  SchemaPathParser parser(sink);
  auto path = parser.Parse("/app:root/app:mode/app:automatic",
                           SchemaPathKind::kAbsolute, {});
  ASSERT_TRUE(path);
  SchemaPathResolver resolver(*module, *tree, sink);
  auto target = resolver.ResolveAbsolute(*path, {});
  ASSERT_TRUE(target);
  EXPECT_EQ(tree->Get(*target).kind, SchemaNodeKind::kLeaf);
}

TEST(SchemaPathTest, ReportsUnknownPrefixAndNode) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  ModuleResolver modules(repository, sink);
  auto module = modules.Resolve(test::Source(
      "module app { namespace 'urn:app'; prefix app; container root; }", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbols(sink); auto semantics = symbols.Resolve(module); ASSERT_TRUE(semantics);
  TypeResolver type_resolver(sink); auto types = type_resolver.Resolve(*semantics); ASSERT_TRUE(types);
  SchemaBuilder builder(sink); auto tree = builder.Build(*semantics, *types); ASSERT_TRUE(tree);
  SchemaPathParser parser(sink);
  SchemaPathResolver resolver(*module, *tree, sink);
  auto bad_prefix = parser.Parse("/bad:root", SchemaPathKind::kAbsolute, {});
  ASSERT_TRUE(bad_prefix); EXPECT_FALSE(resolver.ResolveAbsolute(*bad_prefix, {}));
  auto bad_node = parser.Parse("/missing", SchemaPathKind::kAbsolute, {});
  ASSERT_TRUE(bad_node); EXPECT_FALSE(resolver.ResolveAbsolute(*bad_node, {}));
}

}  // namespace
}  // namespace yang::semantic
