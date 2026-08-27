// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include "yang/module_resolver.h"
#include "yang/schema_tree.h"
#include "yang/semantic_model.h"
#include "yang/type_system.h"
#include "test_support.h"

namespace yang::semantic {
namespace {

struct BuiltSchema {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  std::shared_ptr<const ResolvedModule> module;
  std::optional<SemanticContext> semantics;
  std::optional<TypeContext> types;
  std::optional<SchemaTree> schema;

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
    SchemaBuilder schema_builder(sink);
    schema = schema_builder.Build(*semantics, *types);
  }
};

const SchemaNode& Child(const SchemaTree& tree, std::optional<SchemaNodeId> parent,
                        std::string_view module, std::string_view name) {
  const auto id = tree.FindChild(parent, {std::string(module), std::string(name)});
  EXPECT_TRUE(id);
  return tree.Get(*id);
}

TEST(SchemaTreeTest, BuildsTypedNodesAndInheritsConfig) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    yang-version 1.1;
    namespace "urn:app"; prefix app;
    container state {
      config false;
      leaf name { type string; }
    }
    list item { key id; leaf id { type uint32; mandatory true; } }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& state = Child(*built.schema, std::nullopt, "app", "state");
  const SchemaNode& name = Child(*built.schema, state.id, "app", "name");
  EXPECT_FALSE(state.effective_config);
  EXPECT_FALSE(name.effective_config);
  ASSERT_TRUE(name.type);
  EXPECT_EQ(name.type->builtin, BuiltinType::kString);
  const SchemaNode& item = Child(*built.schema, std::nullopt, "app", "item");
  EXPECT_TRUE(Child(*built.schema, item.id, "app", "id").mandatory);
}

TEST(SchemaTreeTest, CreatesImplicitCasesForChoiceShorthand) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    choice transport {
      leaf tcp { type string; }
      case udp-case { leaf udp { type string; } }
    }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& choice = Child(*built.schema, std::nullopt, "app", "transport");
  const SchemaNode& implicit = Child(*built.schema, choice.id, "app", "tcp");
  EXPECT_EQ(implicit.kind, SchemaNodeKind::kCase);
  EXPECT_EQ(implicit.origin, SchemaNodeOrigin::kImplicitCase);
  EXPECT_EQ(Child(*built.schema, implicit.id, "app", "tcp").kind,
            SchemaNodeKind::kLeaf);
  EXPECT_EQ(Child(*built.schema, choice.id, "app", "udp-case").kind,
            SchemaNodeKind::kCase);
}

TEST(SchemaTreeTest, InstantiatesImportedGroupingInUsingModuleNamespace) {
  BuiltSchema built;
  built.repository.Add("common", R"yang(module common {
    namespace "urn:common"; prefix c;
    grouping endpoint { leaf address { type string; } }
  })yang");
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    import common { prefix c; }
    container service { uses c:endpoint; }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& service = Child(*built.schema, std::nullopt, "app", "service");
  const SchemaNode& address = Child(*built.schema, service.id, "app", "address");
  EXPECT_EQ(address.origin, SchemaNodeOrigin::kUses);
  EXPECT_NE(address.instantiation, kInvalidStatementId);
}

TEST(SchemaTreeTest, ResolvesListKeyInstantiatedFromImportedGrouping) {
  BuiltSchema built;
  built.repository.Add("common", R"yang(module common {
    namespace "urn:common"; prefix c;
    grouping identified { leaf id { type uint32; } }
  })yang");
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    import common { prefix c; }
    list item { key id; uses c:identified; }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& item = Child(*built.schema, std::nullopt, "app", "item");
  ASSERT_EQ(item.key.size(), 1);
  EXPECT_EQ(built.schema->Get(item.key.front()).name,
            (SchemaName{"app", "id"}));
}

TEST(SchemaTreeTest, AppliesRefineProperties) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    grouping settings { container options { leaf mode { type string; } } }
    container service {
      uses settings { refine "options/mode" { mandatory false; default auto; } }
    }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& service = Child(*built.schema, std::nullopt, "app", "service");
  const SchemaNode& options = Child(*built.schema, service.id, "app", "options");
  const SchemaNode& mode = Child(*built.schema, options.id, "app", "mode");
  EXPECT_FALSE(mode.mandatory);
  EXPECT_EQ(mode.default_value, "auto");
}

TEST(SchemaTreeTest, RepresentsOperationsAndNotifications) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    yang-version 1.1;
    namespace "urn:app"; prefix app;
    rpc reset { input { leaf force { type boolean; } } output { leaf result { type string; } } }
    container system { action restart { input { leaf delay { type uint16; } } } notification changed; }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& rpc = Child(*built.schema, std::nullopt, "app", "reset");
  EXPECT_FALSE(rpc.effective_config);
  EXPECT_EQ(Child(*built.schema, rpc.id, "app", "input").kind, SchemaNodeKind::kInput);
  const SchemaNode& system = Child(*built.schema, std::nullopt, "app", "system");
  EXPECT_EQ(Child(*built.schema, system.id, "app", "restart").kind, SchemaNodeKind::kAction);
  EXPECT_EQ(Child(*built.schema, system.id, "app", "changed").kind, SchemaNodeKind::kNotification);
}

TEST(SchemaTreeTest, DetectsRecursiveGroupingExpansion) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    grouping first { uses second; }
    grouping second { uses first; }
    container root { uses first; }
  })yang");
  EXPECT_FALSE(built.schema);
  EXPECT_EQ(built.sink.diagnostics().back().code, DiagnosticCode::kGroupingCycle);
}

TEST(SchemaTreeTest, ValidatesChoiceDefaultsAndElementBounds) {
  BuiltSchema valid;
  valid.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    choice mode { default automatic; case automatic { leaf enabled { type boolean; } } }
  })yang");
  EXPECT_TRUE(valid.schema);

  BuiltSchema invalid_choice;
  invalid_choice.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    choice mode { default missing; case automatic { leaf enabled { type boolean; } } }
  })yang");
  EXPECT_FALSE(invalid_choice.schema);
  EXPECT_EQ(invalid_choice.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidChoiceDefault);

  BuiltSchema invalid_bounds;
  invalid_bounds.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf-list values { type string; min-elements 3; max-elements 2; }
  })yang");
  EXPECT_FALSE(invalid_bounds.schema);
  EXPECT_EQ(invalid_bounds.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidElementBounds);
}

TEST(SchemaTreeTest, RejectsOperationIntroducedUnderRpcByGrouping) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    yang-version 1.1;
    namespace "urn:app"; prefix app;
    grouping nested { action illegal; }
    rpc reset { input { uses nested; } }
  })yang");
  EXPECT_FALSE(built.schema);
  EXPECT_EQ(built.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidOperationPlacement);
}

TEST(SchemaTreeTest, RejectsDuplicateEffectiveChildren) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    grouping names { leaf name { type string; } }
    container root { leaf name { type string; } uses names; }
  })yang");
  EXPECT_FALSE(built.schema);
  EXPECT_EQ(built.sink.diagnostics().back().code, DiagnosticCode::kDuplicateSchemaNode);
}

TEST(SchemaTreeTest, AppliesTopLevelAugment) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    container root;
    augment "/root" { leaf added { type string; } }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& root = Child(*built.schema, std::nullopt, "app", "root");
  const SchemaNode& added = Child(*built.schema, root.id, "app", "added");
  EXPECT_EQ(added.origin, SchemaNodeOrigin::kAugment);
}

TEST(SchemaTreeTest, AppliesUsesLocalAugment) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    grouping base { container target; }
    container root {
      uses base { augment target { leaf added { type string; } } }
    }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& root = Child(*built.schema, std::nullopt, "app", "root");
  const SchemaNode& target = Child(*built.schema, root.id, "app", "target");
  EXPECT_EQ(Child(*built.schema, target.id, "app", "added").origin,
            SchemaNodeOrigin::kAugment);
}

TEST(SchemaTreeTest, RejectsAugmentOfLeaf) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    leaf target { type string; }
    augment "/target" { leaf impossible { type string; } }
  })yang");
  EXPECT_FALSE(built.schema);
  EXPECT_EQ(built.sink.diagnostics().back().code,
            DiagnosticCode::kInvalidAugmentTarget);
}

TEST(SchemaTreeTest, ResolvesKeyAndUniqueLeafPaths) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    list user {
      key "tenant name";
      unique "profile/email";
      leaf tenant { type string; }
      leaf name { type string; }
      container profile { leaf email { type string; } }
    }
  })yang");
  ASSERT_TRUE(built.schema);
  const SchemaNode& user = Child(*built.schema, std::nullopt, "app", "user");
  EXPECT_EQ(user.key.size(), 2);
  ASSERT_EQ(user.unique.size(), 1);
  ASSERT_EQ(user.unique[0].size(), 1);
  EXPECT_EQ(built.schema->Get(user.unique[0][0]).name.local_name, "email");
}

TEST(SchemaTreeTest, RequiresKeyForConfigurationList) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    list user { leaf name { type string; } }
  })yang");
  EXPECT_FALSE(built.schema);
  EXPECT_EQ(built.sink.diagnostics().back().code, DiagnosticCode::kInvalidKey);
}

TEST(SchemaTreeTest, AllowsKeylessStateList) {
  BuiltSchema built;
  built.Build(R"yang(module app {
    namespace "urn:app"; prefix app;
    list event { config false; leaf message { type string; } }
  })yang");
  ASSERT_TRUE(built.schema);
  EXPECT_TRUE(Child(*built.schema, std::nullopt, "app", "event").key.empty());
}

TEST(SchemaContextTest, AppliesCrossModuleAugmentToTargetTree) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  repository.Add("base", R"yang(module base {
    namespace "urn:base"; prefix b;
    container root { leaf status { type string; } }
  })yang");
  ModuleResolver module_resolver(repository, sink);
  auto module = module_resolver.Resolve(test::Source(R"yang(module vendor {
    namespace "urn:vendor"; prefix v;
    import base { prefix b; }
    grouping extra { leaf grouped { type boolean; } }
    augment "/b:root" {
      leaf status { type string; }
      container metrics { leaf count { type uint64; } }
      uses extra;
    }
  })yang", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbol_resolver(sink);
  auto semantics = symbol_resolver.Resolve(module);
  ASSERT_TRUE(semantics);
  TypeResolver type_resolver(sink);
  auto types = type_resolver.Resolve(*semantics);
  ASSERT_TRUE(types);
  SchemaContextBuilder builder(sink);
  auto context = builder.Build(*semantics, *types);
  ASSERT_TRUE(context);
  const ResolvedModule& base = *module->imports.at("b");
  const SchemaTree* tree = context->Find(base);
  ASSERT_NE(tree, nullptr);
  const SchemaNode& root = Child(*tree, std::nullopt, "base", "root");
  EXPECT_EQ(Child(*tree, root.id, "base", "status").origin,
            SchemaNodeOrigin::kDeclared);
  EXPECT_EQ(Child(*tree, root.id, "vendor", "status").origin,
            SchemaNodeOrigin::kAugment);
  const SchemaNode& metrics = Child(*tree, root.id, "vendor", "metrics");
  EXPECT_EQ(Child(*tree, metrics.id, "vendor", "count").type->builtin,
            BuiltinType::kUint64);
  EXPECT_EQ(Child(*tree, root.id, "vendor", "grouped").origin,
            SchemaNodeOrigin::kUses);
}

TEST(SchemaContextTest, RequiresWhenForCrossModuleMandatoryConfig) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  repository.Add("base", "module base { namespace 'urn:base'; prefix b; container root; }");
  ModuleResolver module_resolver(repository, sink);
  auto module = module_resolver.Resolve(test::Source(R"yang(module vendor {
    namespace "urn:vendor"; prefix v;
    import base { prefix b; }
    augment "/b:root" { leaf required { type string; mandatory true; } }
  })yang", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbol_resolver(sink); auto semantics = symbol_resolver.Resolve(module); ASSERT_TRUE(semantics);
  TypeResolver type_resolver(sink); auto types = type_resolver.Resolve(*semantics); ASSERT_TRUE(types);
  SchemaContextBuilder builder(sink);
  EXPECT_FALSE(builder.Build(*semantics, *types));
  EXPECT_EQ(sink.diagnostics().back().code,
            DiagnosticCode::kMandatoryAugmentRequiresWhen);
}

TEST(SchemaContextTest, AllowsConditionalCrossModuleMandatoryConfig) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  repository.Add("base", "module base { namespace 'urn:base'; prefix b; container root; }");
  ModuleResolver module_resolver(repository, sink);
  auto module = module_resolver.Resolve(test::Source(R"yang(module vendor {
    namespace "urn:vendor"; prefix v;
    import base { prefix b; }
    augment "/b:root" {
      when "available";
      leaf required { type string; mandatory true; }
    }
  })yang", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbol_resolver(sink); auto semantics = symbol_resolver.Resolve(module); ASSERT_TRUE(semantics);
  TypeResolver type_resolver(sink); auto types = type_resolver.Resolve(*semantics); ASSERT_TRUE(types);
  SchemaContextBuilder builder(sink);
  EXPECT_TRUE(builder.Build(*semantics, *types));
}

TEST(SchemaContextTest, AllowsMandatoryNodesBelowOptionalAugmentInstances) {
  VectorDiagnosticSink sink;
  InMemoryModuleRepository repository;
  repository.Add("base", "module base { namespace 'urn:base'; prefix b; container root; }");
  ModuleResolver module_resolver(repository, sink);
  auto module = module_resolver.Resolve(test::Source(R"yang(module vendor {
    namespace "urn:vendor"; prefix v;
    import base { prefix b; }
    augment "/b:root" {
      container enabled {
        presence "Enables the feature.";
        leaf required { type string; mandatory true; }
      }
      list optional-entry {
        key "name";
        leaf name { type string; }
        leaf value { type string; mandatory true; }
      }
    }
  })yang", sink));
  ASSERT_TRUE(module);
  SymbolResolver symbol_resolver(sink);
  auto semantics = symbol_resolver.Resolve(module);
  ASSERT_TRUE(semantics);
  TypeResolver type_resolver(sink);
  auto types = type_resolver.Resolve(*semantics);
  ASSERT_TRUE(types);
  SchemaContextBuilder builder(sink);
  const auto context = builder.Build(*semantics, *types);
  EXPECT_TRUE(context) << sink.diagnostics().back().message;
}

}  // namespace
}  // namespace yang::semantic
