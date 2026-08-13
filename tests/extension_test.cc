// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/extension.h"
#include "yang/module_resolver.h"
#include "yang/semantic_model.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

TEST(ExtensionTest, ResolvesTypedImportedInstanceAndInvokesHook) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  repository.Add("annotations", R"yang(module annotations {
    namespace "urn:annotations"; prefix a;
    extension note {
      argument text { yin-element true; }
    }
  })yang");
  ModuleResolver modules(repository, sink);
  auto module = modules.Resolve(test::Source(R"yang(module app {
    namespace "urn:app"; prefix app;
    import annotations { prefix a; }
    container root { a:note "hello"; }
  })yang", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbols(sink);
  auto semantics = symbols.Resolve(module);
  ASSERT_TRUE(semantics);
  bool hook_called = false;
  ExtensionResolver resolver(sink);
  auto extensions = resolver.Resolve(*semantics, {
      [&](const ExtensionInstance& instance, DiagnosticSink&) {
        hook_called = instance.argument == "hello";
        return true;
      }});
  ASSERT_TRUE(extensions);
  ASSERT_EQ(extensions->instances().size(), 1U);
  EXPECT_EQ(extensions->instances()[0].definition.name.module, "annotations");
  EXPECT_TRUE(extensions->instances()[0].definition.yin_element);
  EXPECT_TRUE(hook_called);
}

TEST(ExtensionTest, EnforcesDeclaredArgumentPresence) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  ModuleResolver modules(repository, sink);
  auto module = modules.Resolve(test::Source(R"yang(module app {
    namespace "urn:app"; prefix app;
    extension note { argument text; }
    app:note;
  })yang", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbols(sink);
  auto semantics = symbols.Resolve(module);
  ASSERT_TRUE(semantics);
  ExtensionResolver resolver(sink);
  EXPECT_FALSE(resolver.Resolve(*semantics));
  EXPECT_EQ(sink.diagnostics().back().code,
            DiagnosticCode::kInvalidExtensionInstance);
}

}  // namespace
}  // namespace yang::semantic
