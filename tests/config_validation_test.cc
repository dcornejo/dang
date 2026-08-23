// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <future>
#include <optional>
#include <ranges>
#include <string>

#include <gtest/gtest.h>

#include "yang/config_validation.h"
#include "yang/module_resolver.h"
#include "yang/source_file.h"

namespace yang::config {
namespace {

std::optional<RuntimeSchema> CompileSchema(VectorDiagnosticSink* diagnostics) {
  auto source = SourceFile::Create("device.yang", R"yang(module device {
    yang-version 1.1;
    namespace "urn:device";
    prefix d;
    import identities { prefix ids; }
    identity unrelated;
    container system {
      leaf hostname { type string { length "1..32"; } mandatory true; }
      leaf enabled { type boolean; }
      leaf guarded {
        when "../enabled = 'true'";
        type string;
        must "string-length(.) >= 2" {
          error-message "guarded value is too short";
          error-app-tag "guarded-too-short";
        }
      }
      leaf observed { config false; type uint32; }
      leaf endpoint-kind { type identityref { base ids:endpoint; } }
      leaf hostname-ref { type leafref { path "../hostname"; } }
      leaf selected-interface { type string; }
      leaf selected-mtu {
        type leafref {
          path "../interface[name = current()/../selected-interface]/mtu";
        }
      }
      leaf optional-hostname-ref {
        type leafref { path "../hostname"; require-instance false; }
      }
      leaf selected-node { type instance-identifier; }
      leaf optional-node {
        type instance-identifier { require-instance false; }
      }
      leaf union-reference {
        type union {
          type identityref { base ids:endpoint; }
          type instance-identifier { require-instance false; }
          type leafref { path "../hostname"; }
          type uint16;
        }
      }
      list interface {
        key "name";
        unique "profile/email";
        min-elements 1;
        leaf name { type string; }
        leaf mtu { type uint16 { range "576..9216"; } }
        container profile {
          leaf email { type string; default "unset@example.test"; }
        }
      }
      choice transport {
        mandatory true;
        case tcp { leaf tcp-port { type uint16; } }
        case unix { leaf socket { type string; } }
      }
    }
  })yang", *diagnostics);
  if (!source) return std::nullopt;
  InMemoryModuleRepository repository;
  repository.Add("identities", R"yang(module identities {
    namespace "urn:identities"; prefix ids;
    identity endpoint;
    identity ethernet { base endpoint; }
  })yang");
  Compiler compiler(repository, *diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  return RuntimeSchemaBuilder::FromCompilation(*compilation);
}

bool HasCode(const std::vector<ValidationFinding>& findings, ValidationCode code,
             std::optional<FindingState> state = std::nullopt) {
  return std::ranges::any_of(findings, [&](const ValidationFinding& finding) {
    return finding.code == code && (!state || finding.state == *state);
  });
}

TEST(ConfigValidationTest, ParsesAndValidatesCompleteNetconfConfig) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0"
            xmlns:d="urn:device" xmlns:i="urn:identities">
      <system xmlns="urn:device">
        <hostname>edge-1</hostname>
        <endpoint-kind>i:ethernet</endpoint-kind>
        <hostname-ref>edge-1</hostname-ref>
        <selected-node>/d:system/d:interface[d:name='en0']/d:mtu</selected-node>
        <interface><name>en0</name><mtu>1500</mtu></interface>
        <tcp-port>830</tcp-port>
      </system>
    </config>)xml");
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate({*schema, *parsed.document});
  EXPECT_TRUE(result.valid);
  EXPECT_TRUE(result.complete);
  EXPECT_TRUE(result.findings.empty());
}

TEST(ConfigValidationTest, UsesExpandedNamesAndRejectsUnknownNodes) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto prefixed = ParseDatastoreXml(*schema,
      R"xml(<nc:config xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
               xmlns:d="urn:device"><d:system><d:bogus/></d:system></nc:config>)xml");
  EXPECT_FALSE(prefixed.document);
  EXPECT_TRUE(HasCode(prefixed.findings, ValidationCode::kUnknownDataNode));
  EXPECT_NE(prefixed.findings.front().instance_path.find("{urn:device}bogus"),
            std::string::npos);
}

TEST(ConfigValidationTest, RejectsAttributesInDatastoreConfiguration) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema,
      R"xml(<system xmlns="urn:device" xmlns:nc="urn:ietf:params:xml:ns:netconf:base:1.0"
                    nc:operation="merge"/>)xml");
  EXPECT_FALSE(parsed.document);
  EXPECT_TRUE(HasCode(parsed.findings, ValidationCode::kInvalidNodeShape));
  ASSERT_FALSE(parsed.findings.empty());
  EXPECT_EQ(parsed.findings.front().netconf_error_tag, "unknown-attribute");
}

TEST(ConfigValidationTest, ReportsStructuralAndScalarErrors) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device">
      <hostname/>
      <observed>3</observed>
      <interface><name>en0</name><mtu>1</mtu></interface>
      <interface><name>en0</name></interface>
      <tcp-port>830</tcp-port><socket>/tmp/device</socket>
    </system>)xml");
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate({*schema, *parsed.document});
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kInvalidValue));
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kStateDataInConfiguration));
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kDuplicateListKey));
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kChoiceConflict));
}

TEST(ConfigValidationTest, DistinguishesCompleteAndSelectedCoverage) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto complete = ParseDatastoreXml(*schema, R"xml(<system xmlns="urn:device"/>)xml");
  ASSERT_TRUE(complete.document);
  ConfigValidator validator;
  const ValidationResult complete_result = validator.Validate({*schema, *complete.document});
  EXPECT_FALSE(complete_result.valid);
  EXPECT_TRUE(HasCode(complete_result.findings, ValidationCode::kMissingMandatoryNode,
                      FindingState::kInvalid));
  EXPECT_TRUE(HasCode(complete_result.findings, ValidationCode::kElementCount,
                      FindingState::kInvalid));

  auto selected = ParseDatastoreXml(*schema,
      R"xml(<system xmlns="urn:device"/>)xml", {.coverage = Coverage::kSelected});
  ASSERT_TRUE(selected.document);
  const ValidationResult partial = validator.Validate(
      {*schema, *selected.document, ValidationScope::kPartialStandalone});
  EXPECT_TRUE(partial.valid);
  EXPECT_FALSE(partial.complete);
  EXPECT_TRUE(HasCode(partial.findings, ValidationCode::kMissingMandatoryNode,
                      FindingState::kIndeterminate));
}

TEST(ConfigValidationTest, SupportsMixedCollectionCoverage) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><hostname>x</hostname><tcp-port>830</tcp-port></system>)xml");
  ASSERT_TRUE(parsed.document);
  const ConfigNodeId system = parsed.document->roots().front();
  const auto interface_schema = schema->FindChild(
      parsed.document->Get(system).schema, {"urn:device", "interface"});
  ASSERT_TRUE(interface_schema);
  ConfigDocument mixed = parsed.document->WithCollectionCoverage(
      system, *interface_schema, Coverage::kSelected);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate({*schema, mixed});
  EXPECT_TRUE(result.valid);
  EXPECT_FALSE(result.complete);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kElementCount,
                      FindingState::kIndeterminate));
}

TEST(ConfigValidationTest, RequiresKeysEvenInPartialListEntries) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><interface><mtu>1500</mtu></interface></system>)xml",
    {.coverage = Coverage::kSelected});
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate(
      {*schema, *parsed.document, ValidationScope::kPartialStandalone});
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kMissingKey));
}

TEST(ConfigValidationTest, EnforcesUniqueWithExplicitAndVirtualDefaultValues) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  ConfigValidator validator;

  auto explicit_duplicate = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device">
      <hostname>x</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name><profile><email>a@example.test</email></profile></interface>
      <interface><name>en1</name><profile><email>a@example.test</email></profile></interface>
    </system>)xml");
  ASSERT_TRUE(explicit_duplicate.document);
  auto explicit_result = validator.Validate({*schema, *explicit_duplicate.document});
  EXPECT_FALSE(explicit_result.valid);
  EXPECT_TRUE(HasCode(explicit_result.findings, ValidationCode::kUniqueViolation));
  const auto explicit_finding = std::ranges::find_if(
      explicit_result.findings, [](const ValidationFinding& finding) {
        return finding.code == ValidationCode::kUniqueViolation;
      });
  ASSERT_NE(explicit_finding, explicit_result.findings.end());
  EXPECT_EQ(explicit_finding->netconf_error_app_tag, "data-not-unique");

  auto default_duplicate = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device">
      <hostname>x</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name></interface>
      <interface><name>en1</name></interface>
    </system>)xml");
  ASSERT_TRUE(default_duplicate.document);
  auto default_result = validator.Validate({*schema, *default_duplicate.document});
  EXPECT_FALSE(default_result.valid);
  EXPECT_TRUE(HasCode(default_result.findings, ValidationCode::kUniqueViolation));
}

TEST(ConfigValidationTest, BuildsEffectiveViewWithoutMutatingExplicitTree) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><hostname>x</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name></interface></system>)xml");
  ASSERT_TRUE(parsed.document);
  const std::size_t explicit_size = parsed.document->size();
  const EffectiveDataView effective =
      EffectiveDataView::Build(*schema, *parsed.document);
  EXPECT_GT(effective.size(), explicit_size);
  EXPECT_EQ(parsed.document->size(), explicit_size);
  EXPECT_TRUE(std::ranges::any_of(
      std::views::iota(EffectiveNodeId{0},
                       static_cast<EffectiveNodeId>(effective.size())),
      [&](EffectiveNodeId id) {
        const EffectiveNode& node = effective.Get(id);
        return !node.explicit_node && node.value == "unset@example.test";
      }));
}

TEST(ConfigValidationTest, ValidatesIdentityrefLeafrefAndInstanceIdentifier) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  EXPECT_TRUE(schema->IdentityIsDerivedFrom(
      {"urn:identities", "ethernet"}, {"urn:identities", "endpoint"}));
  EXPECT_FALSE(schema->IdentityIsDerivedFrom(
      {"urn:identities", "endpoint"}, {"urn:identities", "ethernet"}));
  EXPECT_FALSE(schema->IdentityIsDerivedFrom(
      {"urn:device", "unrelated"}, {"urn:identities", "endpoint"}));
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device" xmlns:d="urn:device">
      <hostname>edge-1</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name><mtu>1500</mtu></interface>
      <endpoint-kind>d:unrelated</endpoint-kind>
      <hostname-ref>missing</hostname-ref>
      <optional-hostname-ref>missing</optional-hostname-ref>
      <selected-node>/d:system/d:interface[d:name='missing']/d:mtu</selected-node>
      <optional-node>relative-path-is-not-an-instance-identifier</optional-node>
    </system>)xml");
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate({*schema, *parsed.document});
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kInvalidValue));
  EXPECT_EQ(std::ranges::count_if(result.findings,
      [](const ValidationFinding& finding) {
        return finding.code == ValidationCode::kUnresolvedReference;
      }), 2);
  EXPECT_TRUE(std::ranges::all_of(result.findings,
      [](const ValidationFinding& finding) {
        return finding.code != ValidationCode::kUnresolvedReference ||
               finding.netconf_error_app_tag == "instance-required";
      }));
}

TEST(ConfigValidationTest, MakesMissingReferencesIndeterminateForPartialTrees) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device" xmlns:d="urn:device">
      <hostname-ref>possibly-elsewhere</hostname-ref>
      <selected-node>/d:system/d:interface[d:name='elsewhere']</selected-node>
    </system>)xml", {.coverage = Coverage::kSelected});
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate(
      {*schema, *parsed.document, ValidationScope::kPartialStandalone});
  EXPECT_TRUE(result.valid);
  EXPECT_FALSE(result.complete);
  EXPECT_EQ(std::ranges::count_if(result.findings,
      [](const ValidationFinding& finding) {
        return finding.code == ValidationCode::kUnresolvedReference &&
               finding.state == FindingState::kIndeterminate;
      }), 2);
}

TEST(ConfigValidationTest, ValidatesReferenceAlternativesNestedInUnion) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  ConfigValidator validator;
  const auto validate_value = [&](std::string_view value) {
    const std::string xml =
        "<system xmlns=\"urn:device\" xmlns:i=\"urn:identities\">"
        "<hostname>x</hostname><tcp-port>830</tcp-port>"
        "<interface><name>en0</name></interface><union-reference>" +
        std::string(value) + "</union-reference></system>";
    auto parsed = ParseDatastoreXml(*schema, xml);
    EXPECT_TRUE(parsed.document.has_value());
    return parsed.document
        ? validator.Validate({*schema, *parsed.document}).valid
        : false;
  };
  EXPECT_TRUE(validate_value("i:ethernet"));
  EXPECT_TRUE(validate_value("42"));
  EXPECT_TRUE(validate_value("x"));
  EXPECT_TRUE(validate_value("/system/interface[name='missing']"));
  EXPECT_FALSE(validate_value("not-a-reference"));
}

TEST(ConfigValidationTest, AppliesLeafrefListKeyPredicates) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device">
      <hostname>x</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name><mtu>1500</mtu>
        <profile><email>en0@example.test</email></profile></interface>
      <interface><name>en1</name><mtu>9000</mtu>
        <profile><email>en1@example.test</email></profile></interface>
      <selected-interface>en1</selected-interface>
      <selected-mtu>1500</selected-mtu>
    </system>)xml");
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate({*schema, *parsed.document});
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kUnresolvedReference));
}

TEST(ConfigValidationTest, EvaluatesMustAndWhenWithDeclaredErrors) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><hostname>x</hostname><enabled>false</enabled>
      <guarded>x</guarded><tcp-port>830</tcp-port>
      <interface><name>en0</name></interface></system>)xml");
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate({*schema, *parsed.document});
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kWhenViolation));
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kMustViolation));
  const auto must = std::ranges::find_if(result.findings,
      [](const ValidationFinding& finding) {
        return finding.code == ValidationCode::kMustViolation;
      });
  ASSERT_NE(must, result.findings.end());
  EXPECT_EQ(must->message, "guarded value is too short");
  EXPECT_EQ(must->netconf_error_app_tag, "guarded-too-short");
}

TEST(ConfigValidationTest, EvaluatesDynamicPredicateContextAndDescendants) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("xpath-runtime.yang", R"yang(
    module xpath-runtime {
      yang-version 1.1;
      namespace "urn:xpath-runtime";
      prefix xr;
      container root {
        list item {
          key "id";
          leaf id { type string; }
          leaf score { type uint16; }
          container details { leaf value { type string; } }
        }
        leaf selected {
          type string;
          must "../item[position() = last()]/id = current()";
          must "count(../item[1]/details/value) = 1";
          must "count(/root//value) = 2";
          must "../item/id != 'a'";
          must "../item/score > 10";
        }
      }
    })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  const RuntimeSchema schema =
      RuntimeSchemaBuilder::FromCompilation(*compilation);

  const auto validate = [&](std::string_view selected) {
    const std::string xml =
        "<root xmlns=\"urn:xpath-runtime\">"
        "<item><id>a</id><score>5</score>"
        "<details><value>one</value></details></item>"
        "<item><id>b</id><score>20</score>"
        "<details><value>two</value></details></item>"
        "<selected>" + std::string(selected) + "</selected></root>";
    auto parsed = ParseDatastoreXml(schema, xml);
    EXPECT_TRUE(parsed.document.has_value());
    return parsed.document
               ? ConfigValidator().Validate({schema, *parsed.document})
               : ValidationResult{};
  };

  EXPECT_TRUE(validate("b").valid);
  const ValidationResult invalid = validate("a");
  ASSERT_FALSE(invalid.findings.empty());
  EXPECT_FALSE(invalid.valid) << static_cast<int>(invalid.findings.front().code);
  EXPECT_TRUE(HasCode(invalid.findings, ValidationCode::kMustViolation))
      << static_cast<int>(invalid.findings.front().code);
}

TEST(ConfigValidationTest, KeepsMissingPredicateDataIndeterminateInPartialTree) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("partial-xpath.yang", R"yang(
    module partial-xpath {
      yang-version 1.1;
      namespace "urn:partial-xpath";
      prefix px;
      container root {
        list item { key "id"; leaf id { type string; } }
        leaf selected {
          type string;
          must "../item[id = current()]";
        }
      }
    })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  const RuntimeSchema schema =
      RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto parsed = ParseDatastoreXml(schema,
      R"xml(<root xmlns="urn:partial-xpath"><selected>a</selected></root>)xml");
  ASSERT_TRUE(parsed.document);
  const ValidationResult result = ConfigValidator().Validate(
      {schema, *parsed.document, ValidationScope::kPartialStandalone});
  EXPECT_TRUE(result.valid);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kXPathIndeterminate,
                      FindingState::kIndeterminate));
}

TEST(ConfigValidationTest, ReportsMalformedXmlAndMissingContext) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  EXPECT_TRUE(HasCode(ParseDatastoreXml(*schema, "<system>").findings,
                      ValidationCode::kMalformedXml));
  const ConfigParseResult multiple_roots = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><hostname>visible</hostname></system>
    <system xmlns="urn:device"><hostname>ignored</hostname></system>)xml");
  EXPECT_FALSE(multiple_roots.document);
  EXPECT_TRUE(HasCode(multiple_roots.findings, ValidationCode::kMalformedXml));
  auto parsed = ParseDatastoreXml(*schema,
      R"xml(<system xmlns="urn:device"><hostname>x</hostname></system>)xml",
      {.coverage = Coverage::kSelected});
  ASSERT_TRUE(parsed.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate(
      {*schema, *parsed.document, ValidationScope::kPartialWithContext});
  EXPECT_FALSE(result.valid);
  EXPECT_TRUE(HasCode(result.findings, ValidationCode::kContextRequired));
}

TEST(ConfigValidationTest, ComposesPartialFragmentWithContext) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto context = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><hostname>edge-1</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name></interface></system>)xml");
  auto fragment = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><hostname-ref>edge-1</hostname-ref></system>)xml",
    {.coverage = Coverage::kSelected});
  ASSERT_TRUE(context.document);
  ASSERT_TRUE(fragment.document);
  ConfigValidator validator;
  const ValidationResult result = validator.Validate(
      {*schema, *fragment.document, ValidationScope::kPartialWithContext,
       &*context.document});
  EXPECT_TRUE(result.valid);
  EXPECT_TRUE(result.complete);
  EXPECT_TRUE(result.findings.empty());
}

TEST(ConfigValidationTest, BuildsSameRuntimeSchemaFromSourceEquivalentYin) {
  pugi::xml_document yin;
  ASSERT_TRUE(yin.load_string(R"xml(
    <module name="yin-device" xmlns="urn:ietf:params:xml:ns:yang:yin:1">
      <yang-version value="1.1"/>
      <namespace uri="urn:yin-device"/>
      <prefix value="yd"/>
      <container name="system">
        <leaf name="hostname"><type name="string"/></leaf>
      </container>
    </module>)xml"));
  VectorDiagnosticSink diagnostics;
  InMemoryModuleRepository repository;
  auto schema = RuntimeSchemaBuilder::FromYin(yin, repository, diagnostics);
  ASSERT_TRUE(schema);
  ASSERT_EQ(schema->roots().size(), 1U);
  EXPECT_EQ(schema->Get(schema->roots().front()).name,
            (QualifiedXmlName{"urn:yin-device", "system"}));
  EXPECT_FALSE(diagnostics.has_errors());
}

TEST(ConfigValidationTest, SupportsConcurrentReadOnlyValidation) {
  VectorDiagnosticSink diagnostics;
  auto schema = CompileSchema(&diagnostics);
  ASSERT_TRUE(schema);
  auto parsed = ParseDatastoreXml(*schema, R"xml(
    <system xmlns="urn:device"><hostname>x</hostname><tcp-port>830</tcp-port>
      <interface><name>en0</name></interface></system>)xml");
  ASSERT_TRUE(parsed.document);
  std::vector<std::future<bool>> validations;
  for (int index = 0; index < 8; ++index) {
    validations.push_back(std::async(std::launch::async, [&] {
      ConfigValidator validator;
      return validator.Validate({*schema, *parsed.document}).valid;
    }));
  }
  for (auto& validation : validations) EXPECT_TRUE(validation.get());
}

TEST(ConfigValidationTest, PropagatesNacmDefaultDenyAnnotations) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("secure.yang", R"yang(module secure {
    yang-version 1.1; namespace "urn:secure"; prefix s;
    import ietf-netconf-acm { prefix nacm; }
    container secrets {
      nacm:default-deny-all;
      leaf password { type string; }
    }
    container settings {
      nacm:default-deny-write;
      leaf mode { type string; }
    }
  })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  repository.Add("ietf-netconf-acm", R"yang(module ietf-netconf-acm {
    yang-version 1.1;
    namespace "urn:ietf:params:xml:ns:yang:ietf-netconf-acm";
    prefix nacm;
    extension default-deny-all;
    extension default-deny-write;
  })yang");
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  RuntimeSchema schema = RuntimeSchemaBuilder::FromCompilation(*compilation);
  const auto secrets = schema.FindRoot({"urn:secure", "secrets"});
  const auto settings = schema.FindRoot({"urn:secure", "settings"});
  ASSERT_TRUE(secrets);
  ASSERT_TRUE(settings);
  const auto password = schema.FindChild(*secrets, {"urn:secure", "password"});
  const auto mode = schema.FindChild(*settings, {"urn:secure", "mode"});
  ASSERT_TRUE(password);
  ASSERT_TRUE(mode);
  EXPECT_TRUE(schema.Get(*secrets).nacm_default_deny_all);
  EXPECT_TRUE(schema.Get(*password).nacm_default_deny_all);
  EXPECT_FALSE(schema.Get(*settings).nacm_default_deny_all);
  EXPECT_TRUE(schema.Get(*settings).nacm_default_deny_write);
  EXPECT_TRUE(schema.Get(*mode).nacm_default_deny_write);
  EXPECT_EQ(schema.Get(*mode).module_name, "secure");
}

TEST(ConfigValidationTest, PreservesNacmAnnotationsFromYin) {
  pugi::xml_document yin;
  ASSERT_TRUE(yin.load_string(R"xml(
    <module name="secure-yin" xmlns="urn:ietf:params:xml:ns:yang:yin:1"
            xmlns:nacm="urn:ietf:params:xml:ns:yang:ietf-netconf-acm">
      <yang-version value="1.1"/>
      <namespace uri="urn:secure-yin"/><prefix value="s"/>
      <import module="ietf-netconf-acm"><prefix value="nacm"/></import>
      <container name="secrets"><nacm:default-deny-all/>
        <leaf name="password"><type name="string"/></leaf>
      </container>
    </module>)xml"));
  InMemoryModuleRepository repository;
  repository.Add("ietf-netconf-acm", R"yang(module ietf-netconf-acm {
    yang-version 1.1;
    namespace "urn:ietf:params:xml:ns:yang:ietf-netconf-acm";
    prefix nacm; extension default-deny-all; extension default-deny-write;
  })yang");
  VectorDiagnosticSink diagnostics;
  auto schema = RuntimeSchemaBuilder::FromYin(yin, repository, diagnostics);
  ASSERT_TRUE(schema);
  const auto secrets = schema->FindRoot({"urn:secure-yin", "secrets"});
  ASSERT_TRUE(secrets);
  const auto password =
      schema->FindChild(*secrets, {"urn:secure-yin", "password"});
  ASSERT_TRUE(password);
  EXPECT_TRUE(schema->Get(*secrets).nacm_default_deny_all);
  EXPECT_TRUE(schema->Get(*password).nacm_default_deny_all);
}

TEST(ConfigValidationTest,
     DoesNotRequireOperationalOrInactiveCaseMandatoryNodes) {
  VectorDiagnosticSink diagnostics;
  auto source = SourceFile::Create("conditional.yang", R"yang(
    module conditional {
      yang-version 1.1; namespace "urn:conditional"; prefix c;
      container settings {
        leaf counter { config false; type uint32; mandatory true; }
        choice selector {
          case named { leaf name { type string; } }
          case targeted { leaf path { type string; mandatory true; } }
        }
      }
    })yang", diagnostics);
  ASSERT_TRUE(source);
  InMemoryModuleRepository repository;
  Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  ASSERT_TRUE(compilation);
  const RuntimeSchema schema =
      RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto parsed = ParseDatastoreXml(schema, R"xml(
    <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
      <settings xmlns="urn:conditional"><name>selected</name></settings>
    </config>)xml");
  ASSERT_TRUE(parsed.document);
  const ValidationResult result =
      ConfigValidator().Validate({schema, *parsed.document});
  EXPECT_TRUE(result.valid) << testing::PrintToString(result.findings);
}

}  // namespace
}  // namespace yang::config
