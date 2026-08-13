// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/application.h"

#include <array>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

#include "yang/compiler.h"
#include "yang/diagnostic.h"
#include "yang/module_resolver.h"
#include "yang/netconf_framing.h"
#include "yang/netconf_persistence.h"
#include "yang/resource_limits.h"
#include "yang/source_file.h"

namespace dangd {
namespace {

std::optional<std::string> ReadFile(const std::filesystem::path& path,
                                    std::uintmax_t maximum_bytes,
                                    std::string_view description,
                                    std::vector<std::string>* errors) {
  std::error_code size_error;
  const std::uintmax_t size = std::filesystem::file_size(path, size_error);
  if (!size_error && size > maximum_bytes) {
    errors->push_back(std::string(description) + " exceeds the byte limit: " +
                      path.string());
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errors->push_back("cannot open " + std::string(description) + ": " +
                      path.string());
    return std::nullopt;
  }
  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  if (contents.size() > maximum_bytes) {
    errors->push_back(std::string(description) + " exceeds the byte limit: " +
                      path.string());
    return std::nullopt;
  }
  return contents;
}

void AppendDiagnostics(const yang::VectorDiagnosticSink& diagnostics,
                       const yang::SourceFile* source,
                       std::vector<std::string>* errors) {
  for (const yang::Diagnostic& diagnostic : diagnostics.diagnostics()) {
    errors->push_back(yang::FormatDiagnostic(diagnostic, source));
  }
}

void AppendFindings(const std::vector<yang::config::ValidationFinding>& findings,
                    std::vector<std::string>* errors) {
  for (const auto& finding : findings) {
    std::string message;
    if (!finding.instance_path.empty()) message = finding.instance_path + ": ";
    message += finding.message;
    errors->push_back(std::move(message));
  }
}

bool WriteAll(std::ostream& output, const std::string& bytes) {
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.flush();
  return output.good();
}

}  // namespace

Application::Application(yang::config::RuntimeSchema schema,
                         yang::config::ConfigDocument configuration,
                         std::optional<std::filesystem::path> state_file,
                         std::optional<yang::netconf::NacmPolicy> nacm)
    : schema_(std::move(schema)),
      backend_(configuration),
      datastores_(schema_, std::move(configuration), std::nullopt, &backend_),
      nacm_(std::move(nacm)),
      server_(datastores_, nacm_ ? &*nacm_ : nullptr),
      state_file_(std::move(state_file)) {}

LoadResult Application::Load(const ApplicationOptions& options) {
  LoadResult result;
  if (options.model.empty())
    result.errors.push_back("a root YANG model is required");
  if (options.configuration.empty())
    result.errors.push_back("an initial XML configuration is required");
  if (!result.errors.empty()) return result;

  const auto model_text = ReadFile(
      options.model, yang::DefaultResourceLimits().maximum_source_bytes,
      "YANG model", &result.errors);
  const auto configuration_text = ReadFile(
      options.configuration, yang::DefaultResourceLimits().maximum_xml_bytes,
      "XML configuration", &result.errors);
  std::optional<std::string> nacm_text;
  if (options.nacm_configuration) {
    nacm_text = ReadFile(*options.nacm_configuration,
                         yang::DefaultResourceLimits().maximum_xml_bytes,
                         "NACM configuration", &result.errors);
  }
  if (!model_text || !configuration_text ||
      (options.nacm_configuration && !nacm_text))
    return result;

  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create(options.model.string(), *model_text,
                                         diagnostics);
  if (!source) {
    AppendDiagnostics(diagnostics, nullptr, &result.errors);
    return result;
  }
  std::vector<std::filesystem::path> search_paths = options.search_paths;
  search_paths.insert(search_paths.begin(), options.model.parent_path());
  yang::FilesystemModuleRepository repository(std::move(search_paths));
  yang::Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation || diagnostics.has_errors()) {
    AppendDiagnostics(diagnostics, source.get(), &result.errors);
    return result;
  }

  auto schema =
      yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  auto parsed = yang::config::ParseDatastoreXml(schema, *configuration_text);
  if (!parsed.document) {
    AppendFindings(parsed.findings, &result.errors);
    return result;
  }
  yang::config::ConfigValidator validator;
  auto validation = validator.Validate({schema, *parsed.document});
  if (!validation.valid || !validation.complete) {
    AppendFindings(validation.findings, &result.errors);
    return result;
  }

  std::optional<yang::netconf::NacmPolicy> nacm;
  if (nacm_text) {
    auto loaded_nacm = yang::netconf::LoadNacmPolicy(*nacm_text);
    if (!loaded_nacm.policy) {
      for (const std::string& error : loaded_nacm.errors)
        result.errors.push_back("invalid NACM configuration: " + error);
      return result;
    }
    nacm = std::move(*loaded_nacm.policy);
  }

  result.application = std::unique_ptr<Application>(new Application(
      std::move(schema), std::move(*parsed.document), options.state_file,
      std::move(nacm)));
  if (options.state_file) {
    std::error_code exists_error;
    const bool exists =
        std::filesystem::exists(*options.state_file, exists_error);
    if (exists_error) {
      result.errors.push_back("cannot inspect state file: " +
                              exists_error.message());
      result.application.reset();
    } else if (exists) {
      const auto loaded = yang::netconf::LoadDatastoreSnapshot(
          *options.state_file, result.application->datastores_);
      if (!loaded.ok) {
        result.errors.push_back("cannot restore state file: " +
                                loaded.error.value_or("unknown error"));
        result.application.reset();
      }
    }
  }
  return result;
}

std::optional<std::string> Application::SaveState() const {
  if (!state_file_) return std::nullopt;
  const auto saved =
      yang::netconf::SaveDatastoreSnapshot(*state_file_, datastores_);
  if (saved.ok) return std::nullopt;
  return saved.error.value_or("unknown persistence error");
}

int RunStreamSession(Application& application, std::istream& input,
                     std::ostream& output, std::ostream& errors,
                     std::uint32_t session_id,
                     std::string authenticated_username) {
  if (session_id == 0 || authenticated_username.empty()) {
    errors << "dangd: stream session requires a nonzero ID and username\n";
    return 2;
  }
  yang::netconf::NetconfSession session(
      application.server(), session_id, std::move(authenticated_username));
  if (!session.valid()) {
    errors << "dangd: cannot register NETCONF session\n";
    return 1;
  }
  if (!WriteAll(output, session.Start())) {
    errors << "dangd: cannot write the NETCONF server hello\n";
    return 1;
  }

  std::array<char, 16 * 1024> buffer{};
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count == 0) break;
    auto response = session.Receive(std::string_view(
        buffer.data(), static_cast<std::size_t>(count)));
    for (const std::string& bytes : response.bytes_to_send) {
      if (!WriteAll(output, bytes)) {
        errors << "dangd: cannot write NETCONF output\n";
        return 1;
      }
    }
    if (response.error) errors << "dangd: " << *response.error << '\n';
    for (const std::string& delta : application.DrainBackendDeltas())
      errors << "dangd: configuration delta: " << delta << '\n';
    if (application.has_state_file()) {
      if (const auto persistence_error = application.SaveState()) {
        errors << "dangd: cannot persist datastore state: "
               << *persistence_error << '\n';
        return 1;
      }
    }
    if (response.close_transport) return response.error ? 1 : 0;
  }
  session.TransportClosed();
  return input.bad() ? 1 : 0;
}

}  // namespace dangd
