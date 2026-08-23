// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/application.h"

#include <array>
#include <atomic>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include <unistd.h>

#include <pugixml.hpp>
#include <openssl/evp.h>

#include "yang/compiler.h"
#include "yang/diagnostic.h"
#include "yang/module_resolver.h"
#include "yang/netconf_framing.h"
#include "yang/netconf_persistence.h"
#include "yang/resource_limits.h"
#include "yang/source_file.h"
#include "yang/xml_security.h"

namespace dangd {
namespace {

std::string_view LocalName(std::string_view name) {
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

std::string AuditField(std::string_view value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string escaped;
  for (const char character : value) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 0x21 && byte <= 0x7e && byte != '%' && byte != '=') {
      escaped.push_back(static_cast<char>(byte));
    } else {
      escaped.push_back('%');
      escaped.push_back(kHex[byte >> 4]);
      escaped.push_back(kHex[byte & 0x0f]);
    }
  }
  return escaped;
}

class OverlayRepository final : public yang::ModuleSourceRepository {
 public:
  explicit OverlayRepository(std::vector<std::filesystem::path> search_paths)
      : filesystem_(std::move(search_paths)) {}

  void Add(const PluginYangSource& source) {
    memory_.Add(source.module_name, source.source, source.revision);
  }

  void Add(std::string name, std::string source,
           std::optional<std::string> revision) {
    memory_.Add(std::move(name), std::move(source), std::move(revision));
  }

  std::shared_ptr<const yang::SourceFile> Load(
      std::string_view name, std::optional<std::string_view> revision,
      yang::ModuleKind kind, yang::DiagnosticSink& diagnostics) override {
    yang::VectorDiagnosticSink ignored;
    if (auto source = memory_.Load(name, revision, kind, ignored)) return source;
    return filesystem_.Load(name, revision, kind, diagnostics);
  }

 private:
  yang::InMemoryModuleRepository memory_;
  yang::FilesystemModuleRepository filesystem_;
};

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

bool HasManagedNacm(const yang::config::RuntimeSchema& schema) {
  return schema.FindRoot({
      "urn:ietf:params:xml:ns:yang:ietf-netconf-acm", "nacm"}).has_value();
}

std::string NamespaceFor(pugi::xml_node node, std::string_view prefix) {
  const std::string attribute =
      prefix.empty() ? "xmlns" : "xmlns:" + std::string(prefix);
  for (pugi::xml_node current = node; current;
       current = current.parent()) {
    if (const pugi::xml_attribute found = current.attribute(attribute.c_str()))
      return found.as_string();
  }
  return {};
}

yang::config::QualifiedXmlName ExpandedName(pugi::xml_node node) {
  const std::string_view name = node.name();
  const std::size_t colon = name.find(':');
  const std::string_view prefix = colon == std::string_view::npos
      ? std::string_view() : name.substr(0, colon);
  return {NamespaceFor(node, prefix), std::string(LocalName(name))};
}

bool FragmentMatchesSchema(const yang::config::RuntimeSchema& schema,
                           pugi::xml_node node,
                           std::optional<yang::config::RuntimeSchemaNodeId>
                               parent = std::nullopt) {
  const auto schema_id = parent ? schema.FindChild(*parent, ExpandedName(node))
                                : schema.FindRoot(ExpandedName(node));
  if (!schema_id) return false;
  for (const pugi::xml_node child : node.children()) {
    if (child.type() == pugi::node_element &&
        !FragmentMatchesSchema(schema, child, *schema_id)) return false;
  }
  return true;
}

std::string SeedNacm(std::string configuration, std::string_view nacm) {
  pugi::xml_document config_document;
  pugi::xml_document nacm_document;
  if (!config_document.load_buffer(configuration.data(), configuration.size()) ||
      !nacm_document.load_buffer(nacm.data(), nacm.size())) return configuration;
  pugi::xml_node root = config_document.document_element();
  for (const pugi::xml_node child : root.children()) {
    const std::string_view name = child.name();
    const std::size_t colon = name.find(':');
    const std::string_view prefix =
        colon == std::string_view::npos ? std::string_view() : name.substr(0, colon);
    const std::string_view local =
        colon == std::string_view::npos ? name : name.substr(colon + 1);
    if (local == "nacm" &&
        NamespaceFor(child, prefix) ==
            "urn:ietf:params:xml:ns:yang:ietf-netconf-acm")
      return configuration;
  }
  root.append_copy(nacm_document.document_element());
  std::ostringstream output;
  config_document.save(output, "  ", pugi::format_raw);
  return output.str();
}

std::string NacmSubtree(const yang::config::ConfigDocument& configuration) {
  pugi::xml_document document;
  const std::string xml = configuration.ToXml();
  if (!document.load_buffer(xml.data(), xml.size())) return {};
  for (const pugi::xml_node child : document.document_element().children()) {
    const std::string_view name = child.name();
    const std::size_t colon = name.find(':');
    const std::string_view local =
        colon == std::string_view::npos ? name : name.substr(colon + 1);
    if (local != "nacm") continue;
    const std::string_view prefix =
        colon == std::string_view::npos ? std::string_view() : name.substr(0, colon);
    if (NamespaceFor(child, prefix) !=
        "urn:ietf:params:xml:ns:yang:ietf-netconf-acm")
      continue;
    std::ostringstream output;
    child.print(output, "  ", pugi::format_raw);
    return output.str();
  }
  return {};
}

std::string Sha256(std::string_view input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int size = 0;
  EVP_Digest(input.data(), input.size(), digest.data(), &size, EVP_sha256(),
             nullptr);
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(static_cast<std::size_t>(size) * 2);
  for (unsigned int index = 0; index < size; ++index) {
    result += hex[digest[index] >> 4];
    result += hex[digest[index] & 0x0f];
  }
  return result;
}

std::string BuildYangLibraryXml(
    const yang::Compilation& compilation,
    const std::set<std::string>& implemented,
    const std::vector<PluginYangSource>& plugin_sources) {
  std::map<std::string, std::set<std::string>> deviations;
  for (const yang::ResolvedModule* module : compilation.schemas.modules()) {
    const auto inspect = [&](const yang::ResolvedModule& source) {
      if (!source.syntax || source.syntax->roots().empty()) return;
      const yang::Statement& root =
          source.syntax->Get(source.syntax->roots().front());
      for (const yang::StatementId id : root.children) {
        const yang::Statement& statement = source.syntax->Get(id);
        if (statement.keyword != "deviation" || !statement.argument ||
            !statement.argument->starts_with('/')) continue;
        const std::string_view path = *statement.argument;
        const std::size_t slash = path.find('/', 1);
        const std::string_view first = path.substr(1, slash - 1);
        const std::size_t colon = first.find(':');
        const std::string deviation_module =
            source.belongs_to.value_or(source.name);
        if (colon == std::string_view::npos) {
          deviations[source.belongs_to.value_or(source.name)].insert(
              deviation_module);
          continue;
        }
        const auto imported =
            source.imports.find(std::string(first.substr(0, colon)));
        if (imported != source.imports.end())
          deviations[imported->second->name].insert(deviation_module);
      }
    };
    inspect(*module);
    for (const auto& submodule : module->includes) inspect(*submodule);
  }
  pugi::xml_document document;
  pugi::xml_node library = document.append_child("yang-library");
  library.append_attribute("xmlns") =
      "urn:ietf:params:xml:ns:yang:ietf-yang-library";
  library.append_attribute("xmlns:ds") =
      "urn:ietf:params:xml:ns:yang:ietf-datastores";
  pugi::xml_node set = library.append_child("module-set");
  set.append_child("name").text() = "dangd-modules";
  std::vector<const yang::ResolvedModule*> modules = compilation.schemas.modules();
  std::ranges::sort(modules, {}, [](const yang::ResolvedModule* module) {
    return std::pair(module->name, module->revision.value_or(""));
  });
  for (const yang::ResolvedModule* module : modules) {
    if (module->name == "dangd-aggregate") continue;
    const bool is_implemented = implemented.contains(module->name);
    pugi::xml_node entry = set.append_child(
        is_implemented ? "module" : "import-only-module");
    entry.append_child("name").text() = module->name;
    if (module->revision)
      entry.append_child("revision").text() = *module->revision;
    else if (!is_implemented)
      entry.append_child("revision").text() = "";
    entry.append_child("namespace").text() = module->namespace_uri;
    const auto source = std::ranges::find_if(
        plugin_sources, [&](const PluginYangSource& candidate) {
          return candidate.module_name == module->name &&
                 candidate.revision == module->revision;
        });
    if (source != plugin_sources.end() && !source->source_uri.empty())
      entry.append_child("location").text() = source->source_uri;
    if (source != plugin_sources.end() && is_implemented) {
      for (const std::string& feature : source->enabled_features)
        entry.append_child("feature").text() = feature;
    }
    if (is_implemented && module->name == "ietf-netconf") {
      for (const char* feature : {"writable-running", "candidate",
                                  "confirmed-commit", "rollback-on-error",
                                  "validate", "startup", "xpath"})
        entry.append_child("feature").text() = feature;
    }
    if (is_implemented && module->name == "ietf-netconf-nmda")
      entry.append_child("feature").text() = "origin";
    if (is_implemented) {
      for (const std::string& deviation : deviations[module->name])
        entry.append_child("deviation").text() = deviation;
    }
    for (const auto& submodule : module->includes) {
      pugi::xml_node child = entry.append_child("submodule");
      child.append_child("name").text() = submodule->name;
      if (submodule->revision)
        child.append_child("revision").text() = *submodule->revision;
    }
  }
  pugi::xml_node schema = library.append_child("schema");
  schema.append_child("name").text() = "dangd-schema";
  schema.append_child("module-set").text() = "dangd-modules";
  for (const char* datastore : {"running", "candidate", "startup",
                                "intended", "operational"}) {
    pugi::xml_node entry = library.append_child("datastore");
    entry.append_child("name").text() =
        (std::string("ds:") + datastore).c_str();
    entry.append_child("schema").text() = "dangd-schema";
  }
  std::ostringstream without_id;
  library.print(without_id, "  ", pugi::format_raw);
  library.append_child("content-id").text() = Sha256(without_id.str());
  std::ostringstream output;
  library.print(output, "  ", pugi::format_raw);
  return output.str();
}

std::vector<DangdOperationalData::ModelSource> BuildModelSources(
    const yang::Compilation& compilation) {
  std::vector<DangdOperationalData::ModelSource> result;
  const auto add = [&](const yang::ResolvedModule& module,
                       std::string_view namespace_uri) {
    if (!module.syntax || !module.syntax->source()) return;
    const std::string version = module.revision.value_or("");
    if (std::ranges::any_of(result, [&](const auto& existing) {
          return existing.identifier == module.name &&
                 existing.version == version;
        })) return;
    result.push_back({module.name, version, std::string(namespace_uri),
                      std::string(module.syntax->source()->contents())});
  };
  for (const yang::ResolvedModule* module : compilation.schemas.modules()) {
    if (module->name == "dangd-aggregate") continue;
    add(*module, module->namespace_uri);
    for (const auto& submodule : module->includes)
      add(*submodule, module->namespace_uri);
  }
  std::ranges::sort(result, {}, [](const auto& source) {
    return std::pair(source.identifier, source.version);
  });
  return result;
}

}  // namespace

DangdOperationalData::DangdOperationalData(
    std::string yang_library_xml, std::vector<ModelSource> model_sources,
    const yang::netconf::NacmPolicy* nacm, const PluginManager* plugins,
    const yang::config::RuntimeSchema* runtime_schema)
    : yang_library_xml_(std::move(yang_library_xml)),
      model_sources_(std::move(model_sources)), nacm_(nacm),
      plugins_(plugins), schema_(runtime_schema) {
  pugi::xml_document library;
  pugi::xml_document legacy;
  if (!library.load_buffer(yang_library_xml_.data(), yang_library_xml_.size()))
    return;
  const pugi::xml_node set =
      library.document_element().child("module-set");
  pugi::xml_node state = legacy.append_child("modules-state");
  state.append_attribute("xmlns") =
      "urn:ietf:params:xml:ns:yang:ietf-yang-library";
  state.append_child("module-set-id").text() =
      library.document_element().child("content-id").text().as_string();
  const auto append_modules = [&](std::string_view element,
                                  std::string_view conformance) {
    for (const pugi::xml_node source : set.children(element.data())) {
      pugi::xml_node module = state.append_child("module");
      module.append_copy(source.child("name"));
      if (source.child("revision"))
        module.append_copy(source.child("revision"));
      else
        module.append_child("revision");
      if (source.child("namespace"))
        module.append_copy(source.child("namespace"));
      for (const pugi::xml_node feature : source.children("feature"))
        module.append_copy(feature);
      for (const pugi::xml_node deviation : source.children("deviation")) {
        pugi::xml_node legacy_deviation = module.append_child("deviation");
        legacy_deviation.append_copy(deviation).set_name("name");
        for (const pugi::xml_node candidate : set.children("module")) {
          if (std::string_view(candidate.child("name").text().as_string()) !=
              deviation.text().as_string()) continue;
          if (candidate.child("revision"))
            legacy_deviation.append_copy(candidate.child("revision"));
          else
            legacy_deviation.append_child("revision");
          break;
        }
      }
      module.append_child("conformance-type").text() = conformance.data();
      for (const pugi::xml_node submodule : source.children("submodule")) {
        pugi::xml_node copied = module.append_copy(submodule);
        if (!copied.child("revision")) copied.append_child("revision");
      }
    }
  };
  append_modules("module", "implement");
  append_modules("import-only-module", "import");
  std::ostringstream output;
  state.print(output, "  ", pugi::format_raw);
  modules_state_xml_ = output.str();

  pugi::xml_document monitoring;
  pugi::xml_node monitoring_state = monitoring.append_child("netconf-state");
  monitoring_state.append_attribute("xmlns") =
      "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring";
  pugi::xml_node schemas = monitoring_state.append_child("schemas");
  for (const ModelSource& source : model_sources_) {
    pugi::xml_node schema = schemas.append_child("schema");
    schema.append_child("identifier").text() = source.identifier;
    schema.append_child("version").text() = source.version;
    schema.append_child("format").text() = "yang";
    schema.append_child("namespace").text() = source.namespace_uri;
    schema.append_child("location").text() = "NETCONF";
  }
  std::ostringstream monitoring_output;
  monitoring_state.print(monitoring_output, "  ", pugi::format_raw);
  monitoring_xml_ = monitoring_output.str();
}

std::string DangdOperationalData::AugmentDataXml(
    std::string_view configuration_data_xml) const {
  pugi::xml_document document;
  if (!document.load_buffer(configuration_data_xml.data(),
                            configuration_data_xml.size()))
    return {};
  pugi::xml_node data = document.document_element();
  pugi::xml_document library;
  if (library.load_buffer(yang_library_xml_.data(), yang_library_xml_.size()))
    data.append_copy(library.document_element());
  pugi::xml_document modules_state;
  if (modules_state.load_buffer(modules_state_xml_.data(),
                                modules_state_xml_.size()))
    data.append_copy(modules_state.document_element());
  pugi::xml_document monitoring;
  if (monitoring.load_buffer(monitoring_xml_.data(), monitoring_xml_.size()))
    data.append_copy(monitoring.document_element());
  if (plugins_) {
    for (const std::string& fragment : plugins_->OperationalData()) {
      pugi::xml_document plugin_data;
      if (!yang::ParseUntrustedXml(fragment, &plugin_data).ok) continue;
      const pugi::xml_node root = plugin_data.document_element();
      if (std::string_view(LocalName(root.name())) == "data") {
        for (const pugi::xml_node child : root.children())
          if (child.type() == pugi::node_element && schema_ &&
              FragmentMatchesSchema(*schema_, child)) data.append_copy(child);
      } else if (schema_ && FragmentMatchesSchema(*schema_, root)) {
        data.append_copy(root);
      }
    }
  }
  pugi::xml_node nacm;
  for (const pugi::xml_node child : data.children()) {
    const std::string_view name = child.name();
    const std::size_t colon = name.find(':');
    const std::string_view prefix = colon == std::string_view::npos
                                        ? std::string_view()
                                        : name.substr(0, colon);
    const std::string_view local = colon == std::string_view::npos
                                       ? name
                                       : name.substr(colon + 1);
    if (local == "nacm" && NamespaceFor(child, prefix) ==
                               "urn:ietf:params:xml:ns:yang:ietf-netconf-acm") {
      nacm = child;
      break;
    }
  }
  if (!nacm) {
    nacm = data.append_child("nacm");
    nacm.append_attribute("xmlns") =
        "urn:ietf:params:xml:ns:yang:ietf-netconf-acm";
  }
  const yang::netconf::NacmCounters counters = nacm_->counters();
  nacm.append_child("denied-operations").text() =
      counters.denied_operations;
  nacm.append_child("denied-data-writes").text() =
      counters.denied_data_writes;
  nacm.append_child("denied-notifications").text() =
      counters.denied_notifications;
  std::ostringstream output;
  data.print(output, "  ", pugi::format_raw);
  return output.str();
}

std::vector<std::string> DangdOperationalData::Capabilities() const {
  pugi::xml_document document;
  if (!document.load_buffer(yang_library_xml_.data(), yang_library_xml_.size()))
    return {};
  const pugi::xml_node content =
      document.document_element().child("content-id");
  if (!content) return {};
  return {
      "urn:ietf:params:netconf:capability:yang-library:1.1?revision="
      "2019-01-04&content-id=" + std::string(content.text().as_string()),
      "urn:ietf:params:xml:ns:yang:ietf-netconf-monitoring?"
      "module=ietf-netconf-monitoring&revision=2010-10-04"};
}

yang::netconf::OperationalDataProvider::SchemaLookup
DangdOperationalData::GetSchema(
    std::string_view identifier, std::optional<std::string_view> version,
    std::string_view format) const {
  using Status = SchemaLookup::Status;
  if (format != "yang" && format != "ncm:yang")
    return {Status::kUnsupportedFormat, {}};
  std::vector<const ModelSource*> matches;
  for (const ModelSource& source : model_sources_) {
    if (source.identifier == identifier &&
        (!version || source.version == *version))
      matches.push_back(&source);
  }
  if (matches.empty()) return {Status::kNotFound, {}};
  if (matches.size() != 1) return {Status::kNotUnique, {}};
  return {Status::kFound, matches.front()->content};
}

std::string DangdOperationalData::content_id() const {
  pugi::xml_document document;
  if (!document.load_buffer(yang_library_xml_.data(), yang_library_xml_.size()))
    return {};
  return document.document_element().child("content-id").text().as_string();
}

std::string DangdOperationalData::source_digest() const {
  std::string material;
  for (const ModelSource& source : model_sources_) {
    material += source.identifier;
    material.push_back('\0');
    material += source.version;
    material.push_back('\0');
    material += source.namespace_uri;
    material.push_back('\0');
    material += source.content;
    material.push_back('\0');
  }
  return Sha256(material);
}

Application::Application(yang::config::RuntimeSchema schema,
                         yang::config::ConfigDocument configuration,
                         std::optional<std::filesystem::path> state_file,
                         yang::netconf::SnapshotSaveCheckpoint
                             snapshot_save_checkpoint,
                         yang::netconf::NacmPolicy nacm, bool managed_nacm,
                         std::unique_ptr<PluginManager> plugins,
                         std::string yang_library_xml,
                         std::vector<DangdOperationalData::ModelSource>
                             model_sources)
    : schema_(std::move(schema)),
      plugins_(std::move(plugins)),
      nacm_(std::move(nacm)),
      notifications_(&nacm_, 1024, 16 * 1024 * 1024, &schema_),
      operational_(std::move(yang_library_xml), std::move(model_sources),
                   &nacm_, plugins_.get(), &schema_),
      backend_(configuration, plugins_.get(), &nacm_, managed_nacm),
      datastores_(schema_, std::move(configuration), std::nullopt, &backend_),
      server_(datastores_, &nacm_, nullptr, &notifications_, std::nullopt,
              &operational_, plugins_.get()),
      state_file_(std::move(state_file)),
      snapshot_save_checkpoint_(std::move(snapshot_save_checkpoint)) {
  server_.SetRecoveryAuditSink(
      [this](const yang::netconf::RecoveryAuditRecord& record) {
        std::lock_guard lock(recovery_audit_mutex_);
        recovery_audit_records_.push_back(
            "recovery RPC attempt: session=" +
            std::to_string(record.session_id) + " user=" +
            AuditField(record.username) +
            " bytes=" + std::to_string(record.rpc_bytes));
      });
  notifications_.SetInstanceDataProvider([this] {
    const std::string data =
        "<data>" +
        datastores_.Read(yang::netconf::Datastore::kRunning).ToXml(false) +
        "</data>";
    return operational_.AugmentDataXml(data);
  });
  (void)notifications_.AddStream({});
}

std::vector<std::string> Application::DrainRecoveryAuditRecords() {
  std::lock_guard lock(recovery_audit_mutex_);
  std::vector<std::string> records;
  records.swap(recovery_audit_records_);
  return records;
}

bool Application::PublishYangLibraryUpdate(std::string_view content_id) {
  if (content_id == operational_.content_id()) return true;
  const std::string content =
      "<yang-library-update "
      "xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">"
      "<content-id>" + std::string(content_id) +
      std::string("</content-id></yang-library-update>");
  const bool current = notifications_.Publish(
      "NETCONF", "ietf-yang-library", "yang-library-update", content);
  const std::string legacy =
      "<yang-library-change "
      "xmlns=\"urn:ietf:params:xml:ns:yang:ietf-yang-library\">"
      "<module-set-id>" + std::string(content_id) +
      "</module-set-id></yang-library-change>";
  const bool compatible = notifications_.Publish(
      "NETCONF", "ietf-yang-library", "yang-library-change", legacy);
  return current && compatible;
}

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
  const auto configuration_text = options.configuration_override
      ? options.configuration_override
      : ReadFile(options.configuration,
                 yang::DefaultResourceLimits().maximum_xml_bytes,
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

  auto plugins = std::make_unique<PluginManager>();
  for (const auto& plugin : options.plugins)
    (void)plugins->Load(plugin, &result.errors);
  (void)plugins->ValidateDependencies(&result.errors);
  if (!result.errors.empty()) return result;

  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create(options.model.string(), *model_text,
                                         diagnostics);
  if (!source) {
    AppendDiagnostics(diagnostics, nullptr, &result.errors);
    return result;
  }
  std::vector<std::filesystem::path> search_paths = options.search_paths;
  search_paths.insert(search_paths.begin(), options.model.parent_path());
  search_paths.emplace_back(std::filesystem::path(DANGD_SOURCE_DIR) /
                            "dangd/models");
  search_paths.emplace_back(DANGD_INSTALL_MODEL_DIR);
  OverlayRepository repository(std::move(search_paths));
  for (const PluginYangSource& plugin_source : plugins->yang_sources())
    repository.Add(plugin_source);
  yang::Compiler compiler(repository, diagnostics);
  auto compilation = compiler.Compile(source);
  if (!compilation || diagnostics.has_errors()) {
    AppendDiagnostics(diagnostics, source.get(), &result.errors);
    return result;
  }

  const std::string root_module_name = compilation->module->name;
  repository.Add(compilation->module->name, std::string(source->contents()),
                 compilation->module->revision);
  {
    std::set<std::string> imported;
    std::ostringstream aggregate;
    aggregate << "module dangd-aggregate { yang-version 1.1; "
                 "namespace \"urn:dangd:aggregate\"; prefix da;\n";
    std::size_t prefix = 0;
    const auto add_import = [&](std::string_view name,
                                const std::optional<std::string>& revision) {
      if (!imported.insert(std::string(name)).second) return;
      aggregate << "import " << name << " { prefix p" << prefix++ << ";";
      if (revision) aggregate << " revision-date " << *revision << ";";
      aggregate << " }\n";
    };
    add_import(compilation->module->name, compilation->module->revision);
    add_import("ietf-netconf-acm", std::string("2018-02-14"));
    add_import("ietf-yang-library", std::string("2019-01-04"));
    add_import("ietf-netconf-monitoring", std::string("2010-10-04"));
    add_import("ietf-netconf-nmda", std::string("2019-01-07"));
    for (const PluginYangSource& plugin_source : plugins->yang_sources()) {
      if (plugin_source.role != DANG_YANG_IMPORT_ONLY_V1)
        add_import(plugin_source.module_name, plugin_source.revision);
    }
    aggregate << "}";
    diagnostics = {};
    auto aggregate_source = yang::SourceFile::Create(
        "dangd-aggregate.yang", aggregate.str(), diagnostics);
    if (!aggregate_source) {
      AppendDiagnostics(diagnostics, nullptr, &result.errors);
      return result;
    }
    std::vector<yang::semantic::QualifiedSymbolName> features{
        {"ietf-netconf", "writable-running"},
        {"ietf-netconf", "candidate"},
        {"ietf-netconf", "confirmed-commit"},
        {"ietf-netconf", "rollback-on-error"},
        {"ietf-netconf", "validate"},
        {"ietf-netconf", "startup"},
        {"ietf-netconf", "xpath"},
        {"ietf-netconf-nmda", "origin"}};
    for (const PluginYangSource& plugin_source : plugins->yang_sources()) {
      for (const std::string& feature : plugin_source.enabled_features)
        features.push_back({plugin_source.module_name, feature});
    }
    compilation = compiler.Compile(aggregate_source, features);
    if (!compilation || diagnostics.has_errors()) {
      AppendDiagnostics(diagnostics, aggregate_source.get(), &result.errors);
      return result;
    }
  }
  auto schema = yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
  std::set<std::string> implemented{root_module_name, "ietf-netconf-acm",
                                    "ietf-yang-library",
                                    "ietf-netconf-monitoring",
                                    "ietf-netconf", "ietf-netconf-nmda"};
  for (const PluginYangSource& plugin_source : plugins->yang_sources()) {
    if (plugin_source.role != DANG_YANG_IMPORT_ONLY_V1)
      implemented.insert(plugin_source.module_name);
  }
  const std::string yang_library_xml =
      BuildYangLibraryXml(*compilation, implemented, plugins->yang_sources());
  auto model_sources = BuildModelSources(*compilation);
  const bool managed_nacm = HasManagedNacm(schema);
  std::string seeded_configuration = *configuration_text;
  if (managed_nacm && nacm_text)
    seeded_configuration = SeedNacm(std::move(seeded_configuration), *nacm_text);
  auto parsed = yang::config::ParseDatastoreXml(schema, seeded_configuration);
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

  yang::netconf::NacmPolicy nacm;
  if (managed_nacm || nacm_text) {
    std::string policy_xml = managed_nacm ? NacmSubtree(*parsed.document)
                                           : *nacm_text;
    if (!policy_xml.empty()) {
      auto loaded_nacm = yang::netconf::LoadNacmPolicy(policy_xml);
      if (!loaded_nacm.policy) {
        for (const std::string& error : loaded_nacm.errors)
          result.errors.push_back("invalid NACM configuration: " + error);
        return result;
      }
      nacm = std::move(*loaded_nacm.policy);
    }
  } else {
    nacm.set_enabled(true);
  }
  for (const std::string& recovery_user : options.recovery_users) {
    if (!nacm.AddRecoveryUser(recovery_user)) {
      result.errors.push_back(
          "NACM recovery users must be unique canonical UTF-8 identities");
      return result;
    }
  }

  result.application = std::unique_ptr<Application>(new Application(
      std::move(schema), std::move(*parsed.document), options.state_file,
      options.snapshot_save_checkpoint, std::move(nacm), managed_nacm,
      std::move(plugins), yang_library_xml, std::move(model_sources)));
  if (options.state_file && !options.configuration_override) {
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
    } else if (const auto persistence_error =
                   result.application->SaveState()) {
      result.errors.push_back("cannot persist initial state file: " +
                              *persistence_error);
      result.application.reset();
    }
  }
  if (result.application && options.state_file) {
    Application* application = result.application.get();
    application->datastores_.SetPersistentStateCommitter(
        [application](const yang::netconf::PersistentDatastoreState& before,
                      const yang::netconf::PersistentDatastoreState& after)
            -> std::optional<yang::config::ValidationFinding> {
          const auto saved = yang::netconf::SaveDatastoreSnapshot(
              *application->state_file_, after,
              application->snapshot_save_checkpoint_);
          if (saved.ok) return std::nullopt;
          const auto compensated = yang::netconf::SaveDatastoreSnapshot(
              *application->state_file_, before);
          yang::config::ValidationFinding finding;
          finding.code = yang::config::ValidationCode::kInvalidValue;
          finding.state = yang::config::FindingState::kInvalid;
          finding.netconf_error_tag = "operation-failed";
          finding.message = "datastore persistence failed: " +
              saved.error.value_or("unknown persistence error");
          if (!compensated.ok) {
            finding.message += "; prior snapshot restoration failed: " +
                compensated.error.value_or("unknown persistence error");
          }
          return finding;
        });
  }
  return result;
}

LoadResult Application::Reload(const ApplicationOptions& options,
                               const Application& current) {
  ApplicationOptions replacement = options;
  replacement.configuration_override =
      current.datastores_.Read(yang::netconf::Datastore::kRunning).ToXml();
  static std::atomic<unsigned long> sequence = 0;
  std::vector<std::filesystem::path> staged;
  for (const std::filesystem::path& plugin : options.plugins) {
    const std::filesystem::path copy =
        std::filesystem::temp_directory_path() /
        ("dangd-plugin-" + std::to_string(getpid()) + "-" +
         std::to_string(++sequence) + plugin.extension().string());
    std::error_code error;
    std::filesystem::copy_file(plugin, copy,
                               std::filesystem::copy_options::overwrite_existing,
                               error);
    if (error) {
      for (const auto& path : staged) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
      }
      return {nullptr, {"cannot stage plugin for atomic reload: " +
                        plugin.string() + ": " + error.message()}};
    }
    staged.push_back(copy);
  }
  replacement.plugins = staged;
  LoadResult result = Load(replacement);
  for (const auto& path : staged) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  if (result.application &&
      result.application->operational_.content_id() ==
          current.operational_.content_id() &&
      result.application->operational_.source_digest() !=
          current.operational_.source_digest()) {
    result.application.reset();
    result.errors.push_back(
        "model source changed without a YANG Library identity change; "
        "update the module revision before reloading");
  }
  return result;
}

std::optional<std::string> Application::SaveState() const {
  if (!state_file_) return std::nullopt;
  const auto saved =
      yang::netconf::SaveDatastoreSnapshot(
          *state_file_, datastores_, snapshot_save_checkpoint_);
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
    for (const std::string& audit : application.DrainRecoveryAuditRecords())
      errors << "dangd: audit: " << audit << '\n';
    if (response.close_transport) return response.error ? 1 : 0;
  }
  session.TransportClosed();
  return input.bad() ? 1 : 0;
}

}  // namespace dangd
