// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <optional>
#include <ranges>

#include <gtest/gtest.h>

#include "yang/compiler.h"
#include "yang/config_edit.h"
#include "yang/module_resolver.h"
#include "yang/source_file.h"

namespace yang::config {
namespace {

std::optional<RuntimeSchema> EditSchema(VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("edit.yang", R"yang(module edit {
    yang-version 1.1; namespace "urn:edit"; prefix e;
    container system {
      leaf hostname { type string; mandatory true; }
      leaf enabled { type boolean; }
      leaf guarded { when "../enabled = 'true'"; type string; }
      choice transport { mandatory true;
        case tcp { leaf tcp-port { type uint16; } }
        case unix { leaf socket { type string; } }
      }
      list interface { key "name"; ordered-by user;
        leaf name { type string; } leaf mtu { type uint16; }
      }
      leaf-list search-domain { type string; ordered-by user; }
    }
  })yang", *diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  Compiler compiler(repository, *diagnostics);
  auto compilation = compiler.Compile(source);
  return compilation ? std::optional(RuntimeSchemaBuilder::FromCompilation(*compilation))
                     : std::nullopt;
}

std::optional<ConfigDocument> Target(const RuntimeSchema& schema) {
  return ParseDatastoreXml(schema, R"xml(
    <system xmlns="urn:edit"><hostname>old</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name><mtu>1500</mtu></interface></system>)xml").document;
}

TEST(ConfigEditTest, MergesValuesAndCleansUpCompetingChoiceCase) {
  VectorDiagnosticSink diagnostics;
  auto schema = EditSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto target = Target(*schema);
  ASSERT_TRUE(target);
  auto edit = ParseEditXml(*schema, R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <system xmlns="urn:edit"><hostname>new</hostname><socket>/tmp/edit</socket></system>
    </config>)xml");
  ASSERT_TRUE(edit.document);
  ConfigEditor editor;
  EditResult result = editor.Apply({*schema, *target, *edit.document});
  ASSERT_TRUE(result.candidate) << (result.errors.empty() ? "no error"
                                                         : result.errors.front().message);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_TRUE(std::ranges::any_of(result.changes, [](const ChangeEvent& change) {
    return change.kind == ChangeKind::kValueChanged && change.after == "new";
  }));
  bool has_socket = false;
  bool has_tcp = false;
  for (ConfigNodeId id = 0; id < result.candidate->size(); ++id) {
    const std::string& name = result.candidate->Get(id).name.local_name;
    has_socket = has_socket || name == "socket";
    has_tcp = has_tcp || name == "tcp-port";
  }
  EXPECT_TRUE(has_socket);
  EXPECT_FALSE(has_tcp);
  auto round_trip = ParseDatastoreXml(*schema, result.candidate->ToXml());
  ASSERT_TRUE(round_trip.document);
  ConfigValidator validator;
  EXPECT_TRUE(validator.Validate({*schema, *round_trip.document}).valid);
}

TEST(ConfigEditTest, ImplementsCreateDeleteAndRemoveExistenceSemantics) {
  VectorDiagnosticSink diagnostics;
  auto schema = EditSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto target = Target(*schema);
  ASSERT_TRUE(target);
  ConfigEditor editor;
  auto create = ParseEditXml(*schema, R"xml(
    <system xmlns="urn:edit" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0">
      <hostname nc:operation="create">new</hostname></system>)xml");
  ASSERT_TRUE(create.document);
  EditResult create_result = editor.Apply({*schema, *target, *create.document});
  EXPECT_FALSE(create_result.candidate);
  ASSERT_FALSE(create_result.errors.empty());
  EXPECT_EQ(create_result.errors.front().netconf_error_tag, "data-exists");

  auto remove = ParseEditXml(*schema, R"xml(
    <system xmlns="urn:edit" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0">
      <enabled nc:operation="remove"/></system>)xml");
  ASSERT_TRUE(remove.document);
  EditResult remove_result = editor.Apply({*schema, *target, *remove.document});
  ASSERT_TRUE(remove_result.candidate)
      << (remove_result.errors.empty() ? "no error"
                                       : remove_result.errors.front().message);

  auto delete_edit = ParseEditXml(*schema, R"xml(
    <system xmlns="urn:edit" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0">
      <enabled nc:operation="delete"/></system>)xml");
  ASSERT_TRUE(delete_edit.document);
  EditResult delete_result = editor.Apply({*schema, *target, *delete_edit.document});
  EXPECT_FALSE(delete_result.candidate);
  ASSERT_FALSE(delete_result.errors.empty());
  EXPECT_EQ(delete_result.errors.front().netconf_error_tag, "data-missing");
}

TEST(ConfigEditTest, RollsBackCandidateWhenReplacementFailsValidation) {
  VectorDiagnosticSink diagnostics;
  auto schema = EditSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto target = Target(*schema);
  ASSERT_TRUE(target);
  auto edit = ParseEditXml(*schema, R"xml(
    <system xmlns="urn:edit" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
            nc:operation="replace"><hostname>new</hostname></system>)xml");
  ASSERT_TRUE(edit.document);
  ConfigEditor editor;
  EditResult result = editor.Apply({*schema, *target, *edit.document});
  EXPECT_FALSE(result.candidate);
  EXPECT_TRUE(result.changes.empty());
  EXPECT_FALSE(result.errors.empty());
  EXPECT_EQ(target->size(), 6U);
}

TEST(ConfigEditTest, RemovesNodesWhoseWhenBecomesFalse) {
  VectorDiagnosticSink diagnostics;
  auto schema = EditSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto target = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:edit"><hostname>old</hostname><enabled>true</enabled>
      <guarded>present</guarded><tcp-port>830</tcp-port></system>)xml").document;
  ASSERT_TRUE(target);
  auto edit = ParseEditXml(*schema, R"xml(
    <system xmlns="urn:edit"><enabled>false</enabled></system>)xml");
  ASSERT_TRUE(edit.document);
  ConfigEditor editor;
  EditResult result = editor.Apply({*schema, *target, *edit.document});
  ASSERT_TRUE(result.candidate)
      << (result.errors.empty() ? "no error" : result.errors.front().message +
              " at " + result.errors.front().instance_path);
  EXPECT_FALSE(std::ranges::any_of(
      std::views::iota(ConfigNodeId{0},
                       static_cast<ConfigNodeId>(result.candidate->size())),
      [&](ConfigNodeId id) {
        return result.candidate->Get(id).name.local_name == "guarded";
      }));
}

TEST(ConfigEditTest, RequiresAndUsesContextForPartialTargets) {
  VectorDiagnosticSink diagnostics;
  auto schema = EditSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto context = Target(*schema);
  auto partial = ParseDatastoreXml(*schema,
      R"xml(<system xmlns="urn:edit"><hostname>old</hostname></system>)xml",
      {.coverage = Coverage::kSelected}).document;
  auto edit = ParseEditXml(*schema,
      R"xml(<system xmlns="urn:edit"><hostname>new</hostname></system>)xml");
  ASSERT_TRUE(context);
  ASSERT_TRUE(partial);
  ASSERT_TRUE(edit.document);
  ConfigEditor editor;
  EditResult without_context = editor.Apply({*schema, *partial, *edit.document});
  EXPECT_FALSE(without_context.candidate);
  ASSERT_FALSE(without_context.errors.empty());
  EXPECT_EQ(without_context.errors.front().code,
            ValidationCode::kContextRequired);
  EditResult with_context = editor.Apply(
      {*schema, *partial, *edit.document, EditOperation::kMerge, &*context});
  EXPECT_TRUE(with_context.candidate);
}

TEST(ConfigEditTest, AppliesOrderedByUserInsertionDirectives) {
  VectorDiagnosticSink diagnostics;
  auto schema = EditSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto target = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:edit"><hostname>old</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name></interface>
      <interface><name>en2</name></interface>
      <search-domain>a.example</search-domain>
      <search-domain>c.example</search-domain></system>)xml").document;
  ASSERT_TRUE(target);
  auto edit = ParseEditXml(*schema, R"xml(
    <system xmlns="urn:edit" xmlns:e="urn:edit"
            xmlns:yang="urn:ietf:params:xml:ns:yang:1">
      <interface yang:insert="before" yang:key="[e:name='en2']">
        <name>en1</name>
      </interface>
      <search-domain yang:insert="after" yang:value="a.example">b.example</search-domain>
    </system>)xml");
  ASSERT_TRUE(edit.document)
      << (edit.findings.empty() ? "no error" : edit.findings.front().message);
  ConfigEditor editor;
  EditResult result = editor.Apply({*schema, *target, *edit.document});
  ASSERT_TRUE(result.candidate)
      << (result.errors.empty() ? "no error" : result.errors.front().message);
  const std::string xml = result.candidate->ToXml(false);
  EXPECT_LT(xml.find("<name>en0</name>"), xml.find("<name>en1</name>"));
  EXPECT_LT(xml.find("<name>en1</name>"), xml.find("<name>en2</name>"));
  EXPECT_LT(xml.find(">a.example</"), xml.find(">b.example</"));
  EXPECT_LT(xml.find(">b.example</"), xml.find(">c.example</"));
}

TEST(ConfigEditTest, ReportsMinimalOrderedByUserMoves) {
  VectorDiagnosticSink diagnostics;
  auto schema = EditSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto before = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:edit"><hostname>old</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name></interface>
      <interface><name>en1</name></interface>
      <interface><name>en2</name></interface>
      <search-domain>a.example</search-domain>
      <search-domain>b.example</search-domain></system>)xml").document;
  auto after = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:edit"><hostname>old</hostname><tcp-port>830</tcp-port>
      <interface><name>en1</name></interface>
      <interface><name>en2</name></interface>
      <interface><name>en0</name></interface>
      <search-domain>b.example</search-domain>
      <search-domain>a.example</search-domain></system>)xml").document;
  ASSERT_TRUE(before);
  ASSERT_TRUE(after);

  const std::vector<ChangeEvent> changes =
      DiffConfigDocuments(*schema, *before, *after);
  ASSERT_EQ(changes.size(), 2u)
      << (changes.empty() ? "no changes" : changes.front().instance_path);
  EXPECT_EQ(changes[0].kind, ChangeKind::kMoved);
  EXPECT_NE(changes[0].instance_path.find("interface"), std::string::npos);
  EXPECT_EQ(changes[0].before, "position 1");
  EXPECT_EQ(changes[0].after, "position 3");
  EXPECT_EQ(changes[1].kind, ChangeKind::kMoved);
  EXPECT_NE(changes[1].instance_path.find("search-domain"), std::string::npos);
  EXPECT_EQ(changes[1].before, "position 2");
  EXPECT_EQ(changes[1].after, "position 1");
}

TEST(ConfigEditTest, RejectsInsertionForSystemOrderedCollections) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("system.yang", R"yang(module system {
    yang-version 1.1; namespace "urn:system"; prefix s;
    leaf-list value { type string; }
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  const RuntimeSchema schema = RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto edit = ParseEditXml(schema, R"xml(
    <value xmlns="urn:system" xmlns:y="urn:ietf:params:xml:ns:yang:1"
           y:insert="first">x</value>)xml");
  EXPECT_FALSE(edit.document);
  ASSERT_FALSE(edit.findings.empty());
  EXPECT_EQ(edit.findings.front().netconf_error_tag, "unknown-attribute");
}

}  // namespace
}  // namespace yang::config
