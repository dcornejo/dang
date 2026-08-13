// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "yang/parser.h"
#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "yang/yin_document.h"
#include "test_support.h"

namespace yang {
namespace {

std::optional<YinDocument> Convert(std::string text, VectorDiagnosticSink& sink) {
  Parser parser(test::Source(std::move(text), sink), sink);
  const SyntaxTree tree = parser.Parse();
  YinConverter converter(sink);
  return converter.Convert(tree);
}

TEST(YinDocumentTest, ConvertsModuleToYinWithEscapingAndElements) {
  VectorDiagnosticSink sink;
  auto yin = Convert(R"yang(module example {
    yang-version 1.1;
    namespace "urn:example";
    prefix ex;
    description "A <sensor> & controller";
    container system {
      leaf hostname { type string; default "router"; }
    }
  })yang", sink);
  ASSERT_TRUE(yin);
  const pugi::xml_node module = yin->document().child("module");
  ASSERT_TRUE(module);
  EXPECT_STREQ(module.attribute("name").value(), "example");
  EXPECT_STREQ(module.attribute("xmlns").value(), "urn:ietf:params:xml:ns:yang:yin:1");
  EXPECT_STREQ(module.attribute("xmlns:ex").value(), "urn:example");
  EXPECT_STREQ(module.child("description").child("text").text().get(),
               "A <sensor> & controller");
  EXPECT_STREQ(module.child("container").child("leaf").attribute("name").value(), "hostname");
  const std::string xml = yin->ToString();
  EXPECT_NE(xml.find("A &lt;sensor&gt; &amp; controller"), std::string::npos);
}

TEST(YinDocumentTest, ConvertsLocallyDeclaredExtensionMetadata) {
  VectorDiagnosticSink sink;
  auto yin = Convert(R"yang(module example {
    namespace "urn:example";
    prefix ex;
    extension note {
      argument content { yin-element true; }
    }
    ex:note "hello";
  })yang", sink);
  ASSERT_TRUE(yin);
  const pugi::xml_node note = yin->document().child("module").child("ex:note");
  ASSERT_TRUE(note);
  EXPECT_STREQ(note.child("ex:content").text().get(), "hello");
}

TEST(YinDocumentTest, RefusesInvalidInput) {
  VectorDiagnosticSink sink;
  auto yin = Convert("module broken { prefix b; }", sink);
  EXPECT_FALSE(yin);
  EXPECT_TRUE(sink.has_errors());
}

TEST(YinDocumentTest, ReportsUnresolvedImportedExtensionPrefix) {
  VectorDiagnosticSink sink;
  auto yin = Convert(R"yang(module example {
    namespace "urn:example";
    prefix ex;
    import vendor { prefix v; }
    v:flag enabled;
  })yang", sink);
  EXPECT_FALSE(yin);
  ASSERT_TRUE(sink.has_errors());
  EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kUnknownPrefix);
}

TEST(YinDocumentTest, ConvertsResolvedImportedExtension) {
  InMemoryModuleRepository repository;
  repository.Add("vendor", R"yang(module vendor {
    namespace "urn:vendor";
    prefix v;
    extension note { argument content { yin-element true; } }
  })yang");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto module = resolver.Resolve(test::Source(R"yang(module example {
    namespace "urn:example";
    prefix ex;
    import vendor { prefix vendor; }
    vendor:note "imported text";
  })yang", sink));
  ASSERT_TRUE(module);
  YinConverter converter(sink);
  auto yin = converter.Convert(*module);
  ASSERT_TRUE(yin);
  const pugi::xml_node root = yin->document().child("module");
  EXPECT_STREQ(root.attribute("xmlns:vendor").value(), "urn:vendor");
  EXPECT_STREQ(root.child("vendor:note").child("vendor:content").text().get(),
               "imported text");
}

TEST(YinDocumentTest, ConvertsIncludedSubmoduleWithOwnerNamespace) {
  InMemoryModuleRepository repository;
  repository.Add("app-extra", R"yang(submodule app-extra {
    belongs-to app { prefix app; }
    extension note { argument text; }
    app:note "from submodule";
  })yang");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto module = resolver.Resolve(test::Source(R"yang(module app {
    namespace "urn:app"; prefix app;
    include app-extra;
  })yang", sink));
  ASSERT_TRUE(module);
  ASSERT_EQ(module->includes.size(), 1U);
  YinConverter converter(sink);
  auto yin = converter.ConvertSubmodule(*module->includes[0], *module);
  ASSERT_TRUE(yin);
  const pugi::xml_node root = yin->document().child("submodule");
  EXPECT_STREQ(root.attribute("xmlns:app").value(), "urn:app");
  EXPECT_STREQ(root.child("app:note").attribute("text").value(),
               "from submodule");
}

TEST(YinDocumentTest, EmitsNamespaceDeclarationsDeterministically) {
  InMemoryModuleRepository repository;
  repository.Add("alpha", R"yang(module alpha {
    namespace "urn:alpha"; prefix a; extension marker;
  })yang");
  repository.Add("zeta", R"yang(module zeta {
    namespace "urn:zeta"; prefix z; extension marker;
  })yang");
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto module = resolver.Resolve(test::Source(R"yang(module example {
    namespace "urn:example"; prefix ex;
    import zeta { prefix z; }
    import alpha { prefix a; }
    a:marker;
    z:marker;
  })yang", sink));
  ASSERT_TRUE(module);
  YinConverter converter(sink);
  auto yin = converter.Convert(*module);
  ASSERT_TRUE(yin);
  const std::string xml = yin->ToString(false);
  EXPECT_LT(xml.find("xmlns:a"), xml.find("xmlns:ex"));
  EXPECT_LT(xml.find("xmlns:ex"), xml.find("xmlns:z"));
}

TEST(YinDocumentTest, ConvertsExpandedEffectiveSchema) {
  InMemoryModuleRepository repository;
  VectorDiagnosticSink sink;
  ModuleResolver resolver(repository, sink);
  auto module = resolver.Resolve(test::Source(R"yang(module example {
    namespace "urn:example"; prefix ex;
    grouping common { leaf name { type string; default "node"; } }
    container system { uses common; }
  })yang", sink));
  ASSERT_TRUE(module);
  semantic::SymbolResolver symbols(sink);
  auto semantics = symbols.Resolve(module);
  ASSERT_TRUE(semantics);
  semantic::TypeResolver type_resolver(sink);
  auto types = type_resolver.Resolve(*semantics);
  ASSERT_TRUE(types);
  semantic::SchemaContextBuilder schema_builder(sink);
  auto schemas = schema_builder.Build(*semantics, *types);
  ASSERT_TRUE(schemas);

  YinConverter converter(sink);
  auto yin = converter.ConvertEffective(*schemas, *module);
  ASSERT_TRUE(yin);
  const pugi::xml_node container =
      yin->document().child("module").child("container");
  ASSERT_TRUE(container);
  EXPECT_STREQ(container.child("leaf").attribute("name").value(), "name");
  EXPECT_STREQ(container.child("leaf").child("default")
                   .attribute("value").value(), "node");
  EXPECT_FALSE(yin->document().child("module").child("grouping"));
}

TEST(YinDocumentTest, RoundTripsOrderedYinTreeThroughJson) {
  VectorDiagnosticSink sink;
  auto yin = Convert(R"yang(module example {
    namespace "urn:example"; prefix ex;
    description "A <model>";
    leaf enabled { type boolean; default true; }
  })yang", sink);
  ASSERT_TRUE(yin);
  const auto json = yin->ToJson();
  EXPECT_EQ(json["format"], "yang-cpp-yin-tree-v1");
  auto restored = YinDocument::FromJson(json, sink);
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->ToString(false), yin->ToString(false));

  auto invalid = YinDocument::FromJson(nlohmann::json::object(), sink);
  EXPECT_FALSE(invalid);
  EXPECT_EQ(sink.diagnostics().back().code, DiagnosticCode::kInvalidYinJson);
}

}  // namespace
}  // namespace yang
