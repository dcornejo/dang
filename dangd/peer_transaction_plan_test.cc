// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/peer_transaction_plan.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "yang/module_resolver.h"
#include "yang/source_file.h"

namespace dangd {
namespace {

std::optional<yang::config::RuntimeSchema> Schema() {
  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create("alpha.yang", R"yang(module alpha {
        yang-version 1.1; namespace "urn:test:alpha"; prefix a;
        import beta { prefix b; }
        container alpha { leaf value { type string; mandatory true; } }
      })yang",
                                         diagnostics);
  if (!source) return std::nullopt;
  yang::InMemoryModuleRepository repository;
  repository.Add("beta", R"yang(module beta {
    yang-version 1.1; namespace "urn:test:beta"; prefix b;
    container beta { leaf value { type string; mandatory true; } }
  })yang");
  yang::Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation) return std::nullopt;
  return yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
}

PluginPeerCandidate Candidate(std::string provider, std::string participant,
                              std::uint32_t role, std::string module,
                              std::string body) {
  return {std::move(provider),
          "pair",
          std::move(participant),
          role,
          60,
          std::move(module),
          "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">" + body +
              "</config>",
          "{}"};
}

std::vector<PluginPeerCandidate> CompletePlan() {
  std::vector<PluginPeerCandidate> result;
  for (const auto& [participant, role] :
       std::vector<std::pair<std::string, std::uint32_t>>{
           {"primary", DANG_PEER_PRIMARY_V1},
           {"standby", DANG_PEER_STANDBY_V1}}) {
    result.push_back(
        Candidate("alpha-plugin", participant, role, "alpha",
                  "<alpha xmlns=\"urn:test:alpha\"><value>A</value></alpha>"));
    result.push_back(
        Candidate("beta-plugin", participant, role, "beta",
                  "<beta xmlns=\"urn:test:beta\"><value>B</value></beta>"));
  }
  return result;
}

TEST(PeerTransactionPlanTest, ComposesNonOverlappingCompleteModuleImages) {
  const auto schema = Schema();
  ASSERT_TRUE(schema.has_value());
  const auto composed = ComposePeerTransactionPlan(*schema, CompletePlan());
  ASSERT_FALSE(composed.error.has_value()) << composed.error->message;
  ASSERT_EQ(composed.groups.size(), 1u);
  ASSERT_EQ(composed.groups.front().participants.size(), 2u);
  for (const auto& participant : composed.groups.front().participants) {
    EXPECT_NE(participant.candidate_configuration.find("urn:test:alpha"),
              std::string::npos);
    EXPECT_NE(participant.candidate_configuration.find("urn:test:beta"),
              std::string::npos);
    EXPECT_EQ(participant.verifiers.size(), 2u);
  }
}

TEST(PeerTransactionPlanTest, RejectsOverlappingModuleOwnership) {
  const auto schema = Schema();
  ASSERT_TRUE(schema.has_value());
  auto contributions = CompletePlan();
  contributions.push_back(contributions.front());
  contributions.back().provider = "other-plugin";
  const auto composed = ComposePeerTransactionPlan(*schema, contributions);
  ASSERT_TRUE(composed.error.has_value());
  EXPECT_NE(composed.error->message.find("same peer module image"),
            std::string::npos);
}

TEST(PeerTransactionPlanTest, RejectsIncompleteParticipantModuleSet) {
  const auto schema = Schema();
  ASSERT_TRUE(schema.has_value());
  auto contributions = CompletePlan();
  contributions.pop_back();
  const auto composed = ComposePeerTransactionPlan(*schema, contributions);
  ASSERT_TRUE(composed.error.has_value());
  EXPECT_NE(composed.error->message.find("same complete module set"),
            std::string::npos);
}

TEST(PeerTransactionPlanTest, RejectsCrossModuleDataInContribution) {
  const auto schema = Schema();
  ASSERT_TRUE(schema.has_value());
  auto contributions = CompletePlan();
  contributions.front().configuration_xml =
      "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<beta xmlns=\"urn:test:beta\"><value>B</value></beta></config>";
  const auto composed = ComposePeerTransactionPlan(*schema, contributions);
  ASSERT_TRUE(composed.error.has_value());
  EXPECT_NE(composed.error->message.find("data owned by beta"),
            std::string::npos);
}

}  // namespace
}  // namespace dangd
