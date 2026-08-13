// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "yang/compiler.h"
#include "yang/config_validation.h"
#include "yang/module_resolver.h"
#include "yang/netconf_datastore.h"
#include "yang/netconf_framing.h"
#include "yang/netconf_server.h"
#include "yang/source_file.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kLeafCount = 500;
constexpr std::size_t kCompileIterations = 20;
constexpr std::size_t kDataIterations = 200;
constexpr std::size_t kProtocolIterations = 10'000;
constexpr std::size_t kSamples = 5;

struct Timing {
  double minimum_ms = 0;
  double median_ms = 0;
  double maximum_ms = 0;
};

std::string BuildYang() {
  std::string source = R"yang(module benchmark {
    yang-version 1.1;
    namespace "urn:yang-cpp:benchmark";
    prefix b;
    container root {
  )yang";
  for (std::size_t index = 0; index < kLeafCount; ++index) {
    source += "leaf value-" + std::to_string(index) +
              " { type uint32; default 0; }\n";
  }
  source += "}}";
  return source;
}

std::string BuildConfiguration() {
  std::string xml = "<root xmlns=\"urn:yang-cpp:benchmark\">";
  for (std::size_t index = 0; index < kLeafCount; ++index) {
    xml += "<value-" + std::to_string(index) + ">" +
           std::to_string(index) + "</value-" + std::to_string(index) + ">";
  }
  xml += "</root>";
  return xml;
}

std::optional<yang::Compilation> Compile(std::string_view source_text) {
  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create(
      "benchmark.yang", std::string(source_text), diagnostics);
  if (!source) return std::nullopt;
  yang::InMemoryModuleRepository repository;
  yang::Compiler compiler(repository, diagnostics);
  return compiler.Compile(source);
}

template <typename Function>
Timing Measure(std::size_t iterations, Function function) {
  function();
  std::array<double, kSamples> samples{};
  for (double& sample : samples) {
    const auto begin = Clock::now();
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
      function();
    }
    const auto elapsed = std::chrono::duration<double>(Clock::now() - begin);
    sample = elapsed.count() * 1000.0 / static_cast<double>(iterations);
  }
  std::ranges::sort(samples);
  return {samples.front(), samples.at(kSamples / 2), samples.back()};
}

nlohmann::json ToJson(const Timing& timing) {
  return {{"minimum", timing.minimum_ms},
          {"median", timing.median_ms},
          {"maximum", timing.maximum_ms}};
}

}  // namespace

int main() {
  const std::string yang_source = BuildYang();
  const std::string configuration = BuildConfiguration();
  auto compilation = Compile(yang_source);
  if (!compilation) return EXIT_FAILURE;
  const yang::config::RuntimeSchema schema =
      yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto initial = yang::config::ParseDatastoreXml(schema, configuration).document;
  if (!initial) return EXIT_FAILURE;

  std::size_t observed = 0;
  const Timing compile_ms = Measure(kCompileIterations, [&] {
    auto result = Compile(yang_source);
    if (result) observed += result->schemas.root().size();
  });
  const Timing parse_validate_ms = Measure(kDataIterations, [&] {
    auto parsed = yang::config::ParseDatastoreXml(schema, configuration);
    if (parsed.document) {
      const yang::config::ConfigValidator validator;
      const auto validated = validator.Validate(
          {schema, *parsed.document,
           yang::config::ValidationScope::kComplete, nullptr, std::nullopt});
      observed += validated.valid ? parsed.document->size() : 0;
    }
  });

  const std::string framed = yang::netconf::FrameMessage(
      yang::netconf::BaseVersion::kBase11, "<rpc message-id=\"1\"/>");
  const Timing framing_ms = Measure(kProtocolIterations, [&] {
    yang::netconf::FramingDecoder decoder(yang::netconf::BaseVersion::kBase11);
    observed += decoder.Feed(framed).messages.size();
  });

  yang::netconf::DatastoreManager datastores(schema, *initial);
  yang::netconf::NetconfServer server(datastores);
  constexpr std::string_view request = R"xml(
    <rpc xmlns="urn:ietf:params:xml:ns:netconf:base:1.0" message-id="1">
      <get-config><source><running/></source></get-config>
    </rpc>)xml";
  const Timing retrieval_ms = Measure(kDataIterations, [&] {
    observed += server.Process("benchmark", request).xml.size();
  });

  nlohmann::json output = {
      {"format", "yang-cpp-benchmark-v1"},
      {"workload",
       {{"leaf-count", kLeafCount},
        {"compile-iterations", kCompileIterations},
        {"data-iterations", kDataIterations},
        {"protocol-iterations", kProtocolIterations},
        {"samples", kSamples}}},
      {"milliseconds-per-operation",
       {{"compile-schema", ToJson(compile_ms)},
        {"parse-and-validate-config", ToJson(parse_validate_ms)},
        {"decode-framed-message", ToJson(framing_ms)},
        {"get-config", ToJson(retrieval_ms)}}},
      {"observation-checksum", observed}};
  std::cout << output.dump(2) << '\n';
  return observed == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
