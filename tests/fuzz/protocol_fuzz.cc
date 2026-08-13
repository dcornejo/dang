// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "yang/compiler.h"
#include "yang/config_validation.h"
#include "yang/module_resolver.h"
#include "yang/nacm.h"
#include "yang/netconf_datastore.h"
#include "yang/netconf_filter.h"
#include "yang/netconf_framing.h"
#include "yang/netconf_persistence.h"
#include "yang/source_file.h"

namespace {

constexpr std::size_t kMaximumFuzzInput = 1024 * 1024;
constexpr std::string_view kFilterData = R"xml(
  <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
    <system xmlns="urn:fuzz"><name>router</name></system>
  </data>)xml";

struct PersistenceFixture {
  yang::config::RuntimeSchema schema;
  yang::config::ConfigDocument initial;
};

const std::optional<PersistenceFixture>& GetPersistenceFixture() {
  static const std::optional<PersistenceFixture> fixture = [] {
    yang::VectorDiagnosticSink diagnostics;
    auto source = yang::SourceFile::Create("fuzz.yang", R"yang(module fuzz {
      yang-version 1.1;
      namespace "urn:fuzz";
      prefix f;
      leaf value { type string; mandatory true; }
    })yang", diagnostics);
    if (!source) return std::optional<PersistenceFixture>{};
    yang::InMemoryModuleRepository repository;
    yang::Compiler compiler(repository, diagnostics);
    auto compilation = compiler.Compile(source);
    if (!compilation) return std::optional<PersistenceFixture>{};
    auto schema =
        yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
    auto initial = yang::config::ParseDatastoreXml(
                       schema, "<value xmlns=\"urn:fuzz\">seed</value>")
                       .document;
    if (!initial) return std::optional<PersistenceFixture>{};
    return std::optional<PersistenceFixture>(
        PersistenceFixture{std::move(schema), std::move(*initial)});
  }();
  return fixture;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (size == 0 || size > kMaximumFuzzInput) return 0;
  const std::uint8_t mode = data[0] % 4;
  const std::string input(reinterpret_cast<const char*>(data + 1), size - 1);

  if (mode == 0) {
    const auto version = (data[0] & 4U) == 0
                             ? yang::netconf::BaseVersion::kBase10
                             : yang::netconf::BaseVersion::kBase11;
    yang::netconf::FramingDecoder decoder(version, kMaximumFuzzInput);
    const std::size_t split = input.size() / 2;
    (void)decoder.Feed(std::string_view(input).substr(0, split));
    (void)decoder.Feed(std::string_view(input).substr(split));
    return 0;
  }

  if (mode == 1) {
    const std::size_t separator = input.find('\0');
    const std::string_view data_xml = separator == std::string::npos
                                          ? kFilterData
                                          : std::string_view(input).substr(0, separator);
    const std::string_view filter_xml =
        separator == std::string::npos
            ? std::string_view(input)
            : std::string_view(input).substr(separator + 1);
    (void)yang::netconf::ApplySubtreeFilter(data_xml, filter_xml);
    (void)yang::netconf::ApplyXPathFilter(data_xml, filter_xml);
    return 0;
  }

  if (mode == 2) {
    (void)yang::netconf::LoadNacmPolicy(input);
    return 0;
  }

  const auto& fixture = GetPersistenceFixture();
  if (fixture) {
    yang::netconf::DatastoreManager datastores(fixture->schema,
                                               fixture->initial);
    (void)yang::netconf::LoadDatastoreSnapshotJson(input, datastores);
  }
  return 0;
}
