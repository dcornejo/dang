// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/application.h"
#include "dangd/peer_recovery_config.h"
#include "dangd/peer_transaction_tls.h"
#include "dangd/peer_transaction_journal.h"
#include "dangd/plugin_worker_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
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

/** Process-lifetime exclusion guard for one atomically replaced state file. */
class StateFileLock {
 public:
  static std::shared_ptr<StateFileLock> Acquire(
      std::filesystem::path state_file, std::string* error,
      std::string_view description = "state file") {
    std::error_code path_error;
    state_file = std::filesystem::weakly_canonical(state_file, path_error);
    if (path_error) {
      *error = "cannot resolve " + std::string(description) + " path: " +
               path_error.message();
      return nullptr;
    }
    std::filesystem::path lock_file = state_file;
    lock_file += ".lock";
    const int descriptor =
        open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
             S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      *error = "cannot open " + std::string(description) + " lock " +
               lock_file.string() + ": " + std::strerror(errno);
      return nullptr;
    }
    struct stat metadata {};
    if (fstat(descriptor, &metadata) != 0) {
      const int saved_errno = errno;
      close(descriptor);
      *error = "cannot inspect " + std::string(description) + " lock " +
               lock_file.string() + ": " + std::strerror(saved_errno);
      return nullptr;
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
        (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
      close(descriptor);
      *error = std::string(description) +
               " lock must be a private regular file owned by the effective "
               "user: " +
               lock_file.string();
      return nullptr;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
      const int saved_errno = errno;
      close(descriptor);
      if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
        *error = std::string(description) +
                 " is already in use by another dangd instance: " +
                 state_file.string();
      } else {
        *error = "cannot lock " + std::string(description) + " " +
                 state_file.string() + ": " + std::strerror(saved_errno);
      }
      return nullptr;
    }
    return std::shared_ptr<StateFileLock>(
        new StateFileLock(std::move(state_file), descriptor));
  }

  ~StateFileLock() {
    (void)flock(descriptor_, LOCK_UN);
    (void)close(descriptor_);
  }

  [[nodiscard]] bool Protects(const std::filesystem::path& state_file,
                              std::string* error) const {
    std::error_code path_error;
    const std::filesystem::path normalized =
        std::filesystem::weakly_canonical(state_file, path_error);
    if (path_error) {
      *error = "cannot resolve state file path: " + path_error.message();
      return false;
    }
    return normalized == state_file_;
  }

 private:
  StateFileLock(std::filesystem::path state_file, int descriptor)
      : state_file_(std::move(state_file)), descriptor_(descriptor) {}

  std::filesystem::path state_file_;
  int descriptor_;
};

namespace {

std::string AuditField(std::string_view value);

bool RecoverOrRejectPendingPeerTransaction(const ApplicationOptions& options,
                                           std::vector<std::string>* errors,
                                           std::vector<PeerRecoveryTarget>*
                                               configured_targets) {
  configured_targets->clear();
  if (options.peer_recovery_configuration &&
      !options.peer_transaction_journal) {
    errors->push_back(
        "peer recovery configuration requires a peer transaction journal");
    return true;
  }
  if (!options.peer_transaction_journal) return false;

  std::vector<std::filesystem::path> persistence_paths = {
      *options.peer_transaction_journal};
  if (options.state_file) persistence_paths.push_back(*options.state_file);
  if (options.peer_recovery_configuration)
    persistence_paths.push_back(*options.peer_recovery_configuration);
  std::set<std::filesystem::path> normalized_paths;
  for (const std::filesystem::path& path : persistence_paths) {
    std::error_code path_error;
    const auto normalized =
        std::filesystem::weakly_canonical(path, path_error);
    if (path_error) {
      errors->push_back("cannot resolve configured persistence paths");
      return true;
    }
    if (!normalized_paths.insert(normalized).second) {
      errors->push_back(
          "datastore state, peer journal, and peer recovery paths must differ");
      return true;
    }
  }

  if (options.peer_recovery_configuration) {
    std::string recovery_error;
    auto recovery_targets = LoadPeerRecoveryConfig(
        *options.peer_recovery_configuration, &recovery_error);
    if (!recovery_targets) {
      errors->push_back("cannot load peer recovery configuration: " +
                        recovery_error);
      return true;
    }
    *configured_targets = std::move(*recovery_targets);
  }

  std::error_code status_error;
  const auto status = std::filesystem::symlink_status(
      *options.peer_transaction_journal, status_error);
  if (status_error == std::errc::no_such_file_or_directory) return false;
  if (status_error) {
    errors->push_back("cannot inspect peer transaction journal: " +
                      status_error.message());
    return true;
  }
  if (!std::filesystem::exists(status)) return false;

  std::string recovery_lock_error;
  const auto recovery_lock = StateFileLock::Acquire(
      *options.peer_transaction_journal, &recovery_lock_error,
      "peer transaction journal");
  if (!recovery_lock) {
    errors->push_back(std::move(recovery_lock_error));
    return true;
  }

  std::string load_error;
  auto journal = PeerTransactionFileJournal::Load(
      *options.peer_transaction_journal, &load_error);
  if (!journal) {
    errors->push_back("cannot load peer transaction journal: " + load_error);
    return true;
  }
  std::map<std::string, TlsClientOptions> configured;
  if (!configured_targets->empty()) {
    for (const PeerRecoveryTarget& target : *configured_targets)
      configured.emplace(PeerRecoveryTargetId(target), target.transport);
    for (const PeerJournalParticipant& participant :
         journal->state().participants) {
      if (!configured.contains(participant.id)) {
        errors->push_back("peer recovery configuration has no target for " +
                          AuditField(participant.id));
        return true;
      }
    }
  }

  if (!configured_targets->empty()) {
    std::vector<PeerTransactionParticipant> participants;
    std::vector<std::string> confirmed;
    participants.reserve(journal->state().participants.size());
    for (const PeerJournalParticipant& participant :
         journal->state().participants) {
      participants.push_back(
          MakeTlsRecoveryParticipant(participant, configured.at(participant.id)));
      if (participant.confirmed) confirmed.push_back(participant.id);
    }
    const PeerTransactionResult recovered =
        PeerTransactionCoordinator().ResumeCommit(
            std::move(participants), std::move(confirmed), journal->Callbacks());
    if (recovered.ok()) return false;

    std::ostringstream message;
    message << "peer transaction recovery did not complete";
    if (!recovered.message.empty()) message << ": " << recovered.message;
    if (!recovered.pending_confirmations.empty()) {
      message << " (pending peers:";
      for (const std::string& id : recovered.pending_confirmations)
        message << ' ' << AuditField(id);
      message << ')';
    }
    errors->push_back(message.str());
    return true;
  }
  std::vector<std::string> pending;
  std::size_t confirmed = 0;
  for (const PeerJournalParticipant& participant :
       journal->state().participants) {
    if (participant.confirmed) {
      ++confirmed;
    } else {
      pending.push_back(participant.id);
    }
  }
  std::ostringstream message;
  message << "unresolved peer transaction "
          << AuditField(journal->state().transaction_id)
          << " requires recovery before startup (" << confirmed << '/'
          << journal->state().participants.size()
          << " confirmations durable; pending peers:";
  if (pending.empty()) {
    message << " none";
  } else {
    for (const std::string& id : pending) message << ' ' << AuditField(id);
  }
  message << ')';
  errors->push_back(message.str());
  return true;
}

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

struct FragmentValidation {
  bool valid = false;
  std::string instance_path;
  std::string reason;
};

struct CompleteOperationalNode { std::string instance_path; };

FragmentValidation ValidateFragmentInstance(
    const yang::config::RuntimeSchema& schema, pugi::xml_node root,
    bool data_wrapper, bool complete = false,
    const yang::config::ConfigDocument* context = nullptr,
    std::span<const CompleteOperationalNode> complete_nodes = {}) {
  pugi::xml_document wrapped;
  pugi::xml_node data = wrapped.append_child("data");
  data.append_attribute("xmlns") =
      "urn:ietf:params:xml:ns:netconf:base:1.0";
  if (data_wrapper) {
    for (const pugi::xml_node child : root.children())
      if (child.type() == pugi::node_element) data.append_copy(child);
  } else {
    data.append_copy(root);
  }
  std::ostringstream xml;
  wrapped.print(xml, "", pugi::format_raw);
  const auto parsed = yang::config::ParseDatastoreXml(
      schema, xml.str(), {.coverage = yang::config::Coverage::kSelected,
                          .allow_origin_metadata = true});
  if (!parsed.document) {
    if (parsed.findings.empty()) return {false, {}, "fragment cannot be parsed"};
    return {false, parsed.findings.front().instance_path,
            parsed.findings.front().message};
  }
  yang::config::ConfigDocument instance = std::move(*parsed.document);
  if (complete) {
    for (yang::config::ConfigNodeId id = 0; id < instance.size(); ++id)
      instance = instance.WithChildCoverage(
          id, yang::config::Coverage::kComplete);
  } else if (!complete_nodes.empty()) {
    for (yang::config::ConfigNodeId id = 0; id < instance.size(); ++id) {
      const bool closed = std::ranges::any_of(
          complete_nodes, [&](const CompleteOperationalNode& candidate) {
            return candidate.instance_path ==
                   yang::config::ConfigNodeInstancePath(schema, instance, id);
          });
      if (closed)
        instance = instance.WithChildCoverage(
            id, yang::config::Coverage::kComplete);
      if (closed) {
        for (yang::config::RuntimeSchemaNodeId child :
             schema.DataChildren(instance.Get(id).schema))
          instance = instance.WithCollectionCoverage(
              id, child, yang::config::Coverage::kComplete);
      }
    }
  }
  const auto validation = yang::config::ConfigValidator().Validate(
      {schema, instance,
       context ? yang::config::ValidationScope::kPartialWithContext
               : yang::config::ValidationScope::kPartialStandalone,
       context,
       std::nullopt, true});
  if (validation.valid) return {true, {}, {}};
  const auto finding = std::ranges::find_if(
      validation.findings, [](const yang::config::ValidationFinding& value) {
        return value.state == yang::config::FindingState::kInvalid;
      });
  if (finding == validation.findings.end())
    return {false, {}, "fragment validation is incomplete"};
  return {false, finding->instance_path, finding->message};
}

std::vector<CompleteOperationalNode> CompleteNodesForFragment(
    const yang::config::RuntimeSchema& schema, pugi::xml_node root,
    bool data_wrapper) {
  pugi::xml_document wrapped;
  pugi::xml_node data = wrapped.append_child("data");
  data.append_attribute("xmlns") =
      "urn:ietf:params:xml:ns:netconf:base:1.0";
  if (data_wrapper) {
    for (const pugi::xml_node child : root.children())
      if (child.type() == pugi::node_element) data.append_copy(child);
  } else {
    data.append_copy(root);
  }
  std::ostringstream xml;
  wrapped.print(xml, "", pugi::format_raw);
  auto parsed = yang::config::ParseDatastoreXml(
      schema, xml.str(), {.coverage = yang::config::Coverage::kSelected,
                          .allow_origin_metadata = true});
  std::vector<CompleteOperationalNode> result;
  if (!parsed.document) return result;
  for (yang::config::ConfigNodeId id = 0; id < parsed.document->size(); ++id)
    result.push_back(
        {yang::config::ConfigNodeInstancePath(schema, *parsed.document, id)});
  return result;
}

std::string SeedNacm(std::string configuration, std::string_view nacm) {
  pugi::xml_document config_document;
  pugi::xml_document nacm_document;
  if (!yang::ParseUntrustedXml(configuration, &config_document).ok ||
      !yang::ParseUntrustedXml(nacm, &nacm_document).ok) return configuration;
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
  if (!yang::ParseUntrustedXml(xml, &document).ok) return {};
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
    entry.append_child("name").text() = module->name.c_str();
    if (module->revision)
      entry.append_child("revision").text() = module->revision->c_str();
    else if (!is_implemented)
      entry.append_child("revision").text() = "";
    entry.append_child("namespace").text() = module->namespace_uri.c_str();
    const auto source = std::ranges::find_if(
        plugin_sources, [&](const PluginYangSource& candidate) {
          return candidate.module_name == module->name &&
                 candidate.revision == module->revision;
        });
    if (source != plugin_sources.end() && !source->source_uri.empty())
      entry.append_child("location").text() = source->source_uri.c_str();
    if (source != plugin_sources.end() && is_implemented) {
      for (const std::string& feature : source->enabled_features)
        entry.append_child("feature").text() = feature.c_str();
    }
    if (is_implemented && module->name == "ietf-netconf") {
      for (const char* feature : {"writable-running", "candidate",
                                  "confirmed-commit", "rollback-on-error",
                                  "validate", "startup", "xpath"})
        entry.append_child("feature").text() = feature;
    }
    if (is_implemented && module->name == "ietf-netconf-nmda")
      entry.append_child("feature").text() = "origin";
    if (is_implemented && module->name == "ietf-keystore") {
      entry.append_child("feature").text() = "central-keystore-supported";
      entry.append_child("feature").text() = "symmetric-keys";
    }
    if (is_implemented) {
      for (const std::string& deviation : deviations[module->name])
        entry.append_child("deviation").text() = deviation.c_str();
    }
    for (const auto& submodule : module->includes) {
      pugi::xml_node child = entry.append_child("submodule");
      child.append_child("name").text() = submodule->name.c_str();
      if (submodule->revision)
        child.append_child("revision").text() = submodule->revision->c_str();
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
  const std::string content_id = Sha256(without_id.str());
  library.append_child("content-id").text() = content_id.c_str();
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
    const yang::netconf::NacmPolicy* nacm, const PluginRuntime* plugins,
    const yang::config::RuntimeSchema* runtime_schema)
    : yang_library_xml_(std::move(yang_library_xml)),
      model_sources_(std::move(model_sources)), nacm_(nacm),
      plugins_(plugins), schema_(runtime_schema) {
  pugi::xml_document library;
  pugi::xml_document legacy;
  if (!yang::ParseUntrustedXml(yang_library_xml_, &library).ok)
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
    schema.append_child("identifier").text() = source.identifier.c_str();
    schema.append_child("version").text() = source.version.c_str();
    schema.append_child("format").text() = "yang";
    schema.append_child("namespace").text() = source.namespace_uri.c_str();
    schema.append_child("location").text() = "NETCONF";
  }
  std::ostringstream monitoring_output;
  monitoring_state.print(monitoring_output, "  ", pugi::format_raw);
  monitoring_xml_ = monitoring_output.str();
}

DangdOperationalData::DataResult DangdOperationalData::AugmentDataXml(
    std::string_view configuration_data_xml) const {
  pugi::xml_document document;
  if (!yang::ParseUntrustedXml(configuration_data_xml, &document).ok)
    return {};
  if (applied_configuration_provider_) {
    const std::string applied_xml = applied_configuration_provider_();
    pugi::xml_document applied;
    if (!yang::ParseUntrustedXml(applied_xml, &applied).ok ||
        std::string_view(LocalName(applied.document_element().name())) !=
            "data") {
      return {};
    }
    document.reset();
    document.append_copy(applied.document_element());
  }
  pugi::xml_node data = document.document_element();
  std::vector<yang::config::ValidationFinding> operational_findings;
  std::optional<yang::config::ConfigDocument> applied_context;
  std::optional<FragmentValidation> applied_context_error;
  if (plugins_ && schema_) {
    pugi::xml_document wrapped_context;
    pugi::xml_node context_data = wrapped_context.append_child("data");
    context_data.append_attribute("xmlns") =
        "urn:ietf:params:xml:ns:netconf:base:1.0";
    for (const pugi::xml_node child : data.children())
      if (child.type() == pugi::node_element)
        context_data.append_copy(child);
    std::ostringstream applied_xml;
    wrapped_context.print(applied_xml, "", pugi::format_raw);
    auto parsed_context = yang::config::ParseDatastoreXml(
        *schema_, applied_xml.str(),
        {.coverage = yang::config::Coverage::kComplete,
         .allow_origin_metadata = true});
    if (parsed_context.document) {
      applied_context = std::move(*parsed_context.document);
    } else if (parsed_context.findings.empty()) {
      applied_context_error =
          FragmentValidation{false, {}, "applied context cannot be parsed"};
    } else {
      applied_context_error =
          FragmentValidation{false,
                             parsed_context.findings.front().instance_path,
                             parsed_context.findings.front().message};
    }
  }
  pugi::xml_document library;
  if (yang::ParseUntrustedXml(yang_library_xml_, &library).ok)
    data.append_copy(library.document_element());
  pugi::xml_document modules_state;
  if (yang::ParseUntrustedXml(modules_state_xml_, &modules_state).ok)
    data.append_copy(modules_state.document_element());
  pugi::xml_document monitoring;
  if (yang::ParseUntrustedXml(monitoring_xml_, &monitoring).ok)
    data.append_copy(monitoring.document_element());
  if (plugins_) {
    std::vector<OperationalProviderFailure> provider_failures;
    pugi::xml_document accepted_provider_data;
    pugi::xml_node accepted = accepted_provider_data.append_child("data");
    if (library.document_element())
      accepted.append_copy(library.document_element());
    if (modules_state.document_element())
      accepted.append_copy(modules_state.document_element());
    if (monitoring.document_element())
      accepted.append_copy(monitoring.document_element());
    const std::size_t core_children =
        static_cast<std::size_t>(std::distance(accepted.begin(), accepted.end()));
    std::vector<CompleteOperationalNode> complete_provider_nodes;
    for (const PluginOperationalFragment& fragment :
         plugins_->OperationalData()) {
      if (fragment.error) {
        provider_failures.push_back(
            {fragment.provider, "callback", fragment.error_path,
             *fragment.error});
        continue;
      }
      pugi::xml_document plugin_data;
      const auto parsed =
          yang::ParseUntrustedXml(fragment.data_xml, &plugin_data);
      if (!parsed.ok) {
        provider_failures.push_back(
            {fragment.provider, "validation", {}, parsed.message});
        continue;
      }
      if (!schema_) continue;
      const pugi::xml_node root = plugin_data.document_element();
      std::vector<CompleteOperationalNode> fragment_complete_nodes;
      if (fragment.complete)
        fragment_complete_nodes = CompleteNodesForFragment(
            *schema_, root,
            std::string_view(LocalName(root.name())) == "data");
      pugi::xml_document candidate_data;
      pugi::xml_node candidate = candidate_data.append_child("data");
      for (const pugi::xml_node child : accepted.children())
        if (child.type() == pugi::node_element) candidate.append_copy(child);
      if (std::string_view(LocalName(root.name())) == "data") {
        const FragmentValidation validation =
            ValidateFragmentInstance(*schema_, root, true, fragment.complete);
        if (!validation.valid) {
          provider_failures.push_back({fragment.provider, "validation",
                                       validation.instance_path,
                                       validation.reason});
          continue;
        }
        for (const pugi::xml_node child : root.children())
          if (child.type() == pugi::node_element) candidate.append_copy(child);
      } else {
        const FragmentValidation validation =
            ValidateFragmentInstance(*schema_, root, false, fragment.complete);
        if (validation.valid) {
          candidate.append_copy(root);
        } else {
          provider_failures.push_back({fragment.provider, "validation",
                                       validation.instance_path,
                                       validation.reason});
          continue;
        }
      }
      std::vector<CompleteOperationalNode> candidate_complete_nodes =
          complete_provider_nodes;
      candidate_complete_nodes.insert(candidate_complete_nodes.end(),
                                      fragment_complete_nodes.begin(),
                                      fragment_complete_nodes.end());
      FragmentValidation merged = ValidateFragmentInstance(
          *schema_, candidate, true, false, nullptr, candidate_complete_nodes);
      if (merged.valid && applied_context_error) {
        merged = *applied_context_error;
      } else if (merged.valid && applied_context) {
        merged = ValidateFragmentInstance(*schema_, candidate, true, false,
                                          &*applied_context,
                                          candidate_complete_nodes);
      }
      if (!merged.valid) {
        provider_failures.push_back({fragment.provider, "merge",
                                     merged.instance_path, merged.reason});
        continue;
      }
      accepted.remove_children();
      for (const pugi::xml_node child : candidate.children())
        if (child.type() == pugi::node_element) accepted.append_copy(child);
      complete_provider_nodes = std::move(candidate_complete_nodes);
    }
    std::size_t child_index = 0;
    for (const pugi::xml_node child : accepted.children())
      if (child.type() == pugi::node_element && child_index++ >= core_children)
        data.append_copy(child);
    pugi::xml_document reconciliation;
    if (yang::ParseUntrustedXml(plugins_->ReconciliationData(provider_failures),
                                &reconciliation).ok)
      data.append_copy(reconciliation.document_element());
    for (const OperationalProviderFailure& failure : provider_failures) {
      yang::config::ValidationFinding finding;
      finding.code = yang::config::ValidationCode::kInvalidValue;
      finding.state = yang::config::FindingState::kInvalid;
      finding.message = "operational provider " + failure.provider +
                        " failed during " + failure.stage + ": " +
                        failure.reason;
      finding.instance_path = failure.instance_path;
      finding.netconf_error_path = failure.instance_path;
      finding.netconf_error_tag = "operation-failed";
      finding.netconf_error_app_tag = "operational-provider-failure";
      operational_findings.push_back(std::move(finding));
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
  return {output.str(), std::move(operational_findings)};
}

void DangdOperationalData::SetAppliedConfigurationProvider(
    std::function<std::string()> provider) {
  applied_configuration_provider_ = std::move(provider);
}

std::vector<std::string> DangdOperationalData::Capabilities() const {
  pugi::xml_document document;
  if (!yang::ParseUntrustedXml(yang_library_xml_, &document).ok)
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
  if (!yang::ParseUntrustedXml(yang_library_xml_, &document).ok)
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
                         std::shared_ptr<StateFileLock> state_file_lock,
                         yang::netconf::SnapshotSaveCheckpoint
                             snapshot_save_checkpoint,
                         yang::netconf::NacmPolicy nacm, bool managed_nacm,
                         std::unique_ptr<PluginRuntime> plugins,
                         std::vector<PeerRecoveryTarget> peer_targets,
                         std::vector<std::string> peer_controller_users,
                         std::string yang_library_xml,
                         std::vector<DangdOperationalData::ModelSource>
                             model_sources)
    : state_file_lock_(std::move(state_file_lock)),
      schema_(std::move(schema)),
      plugins_(std::move(plugins)),
      nacm_(std::move(nacm)),
      notifications_(&nacm_, 1024, 16 * 1024 * 1024, &schema_),
      operational_(std::move(yang_library_xml), std::move(model_sources),
                   &nacm_, plugins_.get(), &schema_),
      backend_(configuration, plugins_.get(), &nacm_, managed_nacm,
               std::move(peer_targets)),
      datastores_(schema_, std::move(configuration), std::nullopt, &backend_),
      server_(datastores_, &nacm_, nullptr, &notifications_, std::nullopt,
              &operational_, plugins_.get()),
      state_file_(std::move(state_file)),
      snapshot_save_checkpoint_(std::move(snapshot_save_checkpoint)) {
  server_.SetExternallyCoordinatedUsers(std::move(peer_controller_users));
  operational_.SetAppliedConfigurationProvider([this] {
    pugi::xml_document document;
    if (!yang::ParseUntrustedXml(backend_.WorkingXml(), &document).ok)
      return std::string("<data/>");
    std::ostringstream output;
    for (const pugi::xml_node child : document.document_element().children())
      child.print(output, "", pugi::format_raw);
    return "<data>" + output.str() + "</data>";
  });
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
    return operational_.AugmentDataXml(data).xml;
  });
  (void)notifications_.AddStream({});
}

Application::~Application() = default;

std::vector<std::string> Application::DrainRecoveryAuditRecords() {
  std::lock_guard lock(recovery_audit_mutex_);
  std::vector<std::string> records;
  records.swap(recovery_audit_records_);
  return records;
}

std::vector<std::string> Application::PollPluginNotifications() {
  std::vector<std::string> errors;
  if (!plugins_) return errors;
  for (PluginNotification& event : plugins_->Notifications()) {
    if (event.error) {
      errors.push_back("plugin " + event.provider + ": " + *event.error);
      continue;
    }
    if (!notifications_.Publish(
            event.stream_name, event.module_name, event.notification_name,
            event.content_xml, std::chrono::system_clock::now(),
            event.default_deny_all, event.instance_path))
      errors.push_back("plugin " + event.provider +
                       ": notification was rejected by the host");
  }
  return errors;
}

bool Application::PublishYangLibraryUpdate(std::string_view content_id) {
  if (content_id == operational_.content_id()) return true;
  const auto notification = [content_id](std::string_view root,
                                         std::string_view leaf) {
    const std::string value(content_id);
    if (value.find('\0') != std::string::npos) return std::string{};
    pugi::xml_document document;
    pugi::xml_node event = document.append_child(root.data());
    event.append_attribute("xmlns") =
        "urn:ietf:params:xml:ns:yang:ietf-yang-library";
    event.append_child(leaf.data()).text() = value.c_str();
    std::ostringstream output;
    document.print(output, "", pugi::format_raw);
    return output.str();
  };
  const std::string content =
      notification("yang-library-update", "content-id");
  const bool current = notifications_.Publish(
      "NETCONF", "ietf-yang-library", "yang-library-update", content);
  const std::string legacy =
      notification("yang-library-change", "module-set-id");
  const bool compatible = notifications_.Publish(
      "NETCONF", "ietf-yang-library", "yang-library-change", legacy);
  return current && compatible;
}

LoadResult Application::Load(const ApplicationOptions& options) {
  return LoadWithStateFileLock(options, nullptr);
}

LoadResult Application::LoadWithStateFileLock(
    const ApplicationOptions& options,
    std::shared_ptr<StateFileLock> inherited_state_file_lock) {
  LoadResult result;
  if (options.model.empty())
    result.errors.push_back("a root YANG model is required");
  if (options.configuration.empty())
    result.errors.push_back("an initial XML configuration is required");
  if (!result.errors.empty()) return result;
  std::vector<PeerRecoveryTarget> peer_targets;
  if (RecoverOrRejectPendingPeerTransaction(options, &result.errors,
                                            &peer_targets))
    return result;

  std::shared_ptr<StateFileLock> state_file_lock;
  if (options.state_file) {
    std::string lock_error;
    if (inherited_state_file_lock &&
        inherited_state_file_lock->Protects(*options.state_file, &lock_error)) {
      state_file_lock = std::move(inherited_state_file_lock);
    } else {
      if (!lock_error.empty()) {
        result.errors.push_back(std::move(lock_error));
        return result;
      }
      state_file_lock = StateFileLock::Acquire(*options.state_file, &lock_error);
      if (!state_file_lock) {
        result.errors.push_back(std::move(lock_error));
        return result;
      }
    }
  }

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

  std::unique_ptr<PluginRuntime> plugins;
  if (options.plugin_worker_executable) {
    plugins = PluginWorkerRuntime::Load(*options.plugin_worker_executable,
                                        options.plugins, &result.errors);
  } else {
    auto in_process = std::make_unique<PluginManager>();
    for (const auto& plugin : options.plugins)
      (void)in_process->Load(plugin, &result.errors);
    (void)in_process->ValidateDependencies(&result.errors);
    plugins = std::move(in_process);
  }
  if (!plugins || !result.errors.empty()) return result;

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
    add_import("dangd-reconciliation", std::string("2026-08-23"));
    // RFC 9644 publishes reusable groupings rather than top-level datastore
    // nodes.  Import all three modules so their complete dependency closure is
    // visible through YANG Library and get-schema as import-only modules.
    add_import("ietf-ssh-common", std::string("2024-10-10"));
    add_import("ietf-ssh-client", std::string("2024-10-10"));
    add_import("ietf-ssh-server", std::string("2024-10-10"));
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
        {"ietf-netconf-nmda", "origin"},
        // RFC 9642 phase one provides a central symmetric-key datastore.  The
        // asymmetric and encrypted/hidden representations remain disabled
        // until their backing cryptographic semantics are implemented.
        {"ietf-keystore", "central-keystore-supported"},
        {"ietf-keystore", "symmetric-keys"},
        {"ietf-crypto-types", "cleartext-symmetric-keys"}};
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
                                    "ietf-netconf", "ietf-netconf-nmda",
                                    "dangd-reconciliation", "ietf-keystore"};
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
  if (options.default_superuser &&
      !nacm.AddRecoveryUser(std::string(kDefaultSuperuser))) {
    result.errors.push_back("cannot install the default dangd super-user");
    return result;
  }
  for (const std::string& recovery_user : options.recovery_users) {
    if (!nacm.AddRecoveryUser(recovery_user)) {
      result.errors.push_back(
          "NACM recovery users must be unique canonical UTF-8 identities");
      return result;
    }
  }
  yang::netconf::NacmPolicy peer_controller_identities;
  for (const std::string& peer_controller_user :
       options.peer_controller_users) {
    if (!peer_controller_identities.AddRecoveryUser(peer_controller_user)) {
      result.errors.push_back(
          "peer controller users must be unique canonical UTF-8 identities");
      return result;
    }
    if (nacm.IsRecoveryUser(peer_controller_user)) {
      result.errors.push_back(
          "peer controller users must not also be NACM recovery users");
      return result;
    }
  }

  result.application = std::unique_ptr<Application>(new Application(
      std::move(schema), std::move(*parsed.document), options.state_file,
      std::move(state_file_lock),
      options.snapshot_save_checkpoint, std::move(nacm), managed_nacm,
      std::move(plugins), std::move(peer_targets),
      options.peer_controller_users, yang_library_xml,
      std::move(model_sources)));
  bool restored_snapshot = false;
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
          *options.state_file, result.application->datastores_,
          yang::netconf::DatastoreManager::RestoreBackend::kDefer);
      if (!loaded.ok) {
        result.errors.push_back("cannot restore state file: " +
                                loaded.error.value_or("unknown error"));
        result.application.reset();
      } else {
        restored_snapshot = true;
      }
    }
  }
  if (result.application) {
    const auto running = result.application->datastores_.Read(
        yang::netconf::Datastore::kRunning);
    const yang::netconf::BackendTransactionContext activation_context{
        .externally_coordinated =
            result.application->datastores_.ExportPersistentState()
                .rollback_externally_coordinated};
    if (auto error = result.application->backend_.Initialize(
            result.application->schema_, running, activation_context)) {
      result.errors.push_back("cannot activate startup configuration: " +
                              error->message);
      result.application.reset();
    }
  }
  if (result.application && options.state_file &&
      !options.configuration_override && !restored_snapshot) {
    if (const auto persistence_error = result.application->SaveState()) {
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
  LoadResult result =
      LoadWithStateFileLock(replacement, current.state_file_lock_);
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
    for (const std::string& error : application.PollPluginNotifications())
      errors << "dangd: plugin notification: " << error << '\n';
    if (response.close_transport) return response.error ? 1 : 0;
  }
  session.TransportClosed();
  return input.bad() ? 1 : 0;
}

}  // namespace dangd
