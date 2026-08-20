// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_APPLICATION_H_
#define DANGD_APPLICATION_H_

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "dangd/configuration_backend.h"
#include "dangd/plugin_manager.h"

#include "yang/config_validation.h"
#include "yang/nacm.h"
#include "yang/netconf_datastore.h"
#include "yang/netconf_notifications.h"
#include "yang/netconf_server.h"

namespace dangd {

/** Immutable RFC 8525 operational data supplied to NETCONF get. */
class DangdOperationalData final
    : public yang::netconf::OperationalDataProvider {
 public:
  struct ModelSource {
    std::string identifier;
    std::string version;
    std::string namespace_uri;
    std::string content;
  };
  DangdOperationalData(std::string yang_library_xml,
                       std::vector<ModelSource> model_sources,
                       const yang::netconf::NacmPolicy* nacm);
  [[nodiscard]] std::string AugmentDataXml(
      std::string_view configuration_data_xml) const override;
  [[nodiscard]] std::vector<std::string> Capabilities() const override;
  [[nodiscard]] SchemaLookup GetSchema(
      std::string_view identifier, std::optional<std::string_view> version,
      std::string_view format) const override;
  /** Returns the current RFC 8525 content identifier. */
  [[nodiscard]] std::string content_id() const;
  /** Hashes the exact retrievable source registry for reload consistency. */
  [[nodiscard]] std::string source_digest() const;

 private:
  std::string yang_library_xml_;
  std::string modules_state_xml_;
  std::string monitoring_xml_;
  std::vector<ModelSource> model_sources_;
  const yang::netconf::NacmPolicy* nacm_ = nullptr;
};

/**
 * Filesystem inputs and optional persistence used to construct an Application.
 */
struct ApplicationOptions {
  /** Root YANG or YIN model to compile. */
  std::filesystem::path model;
  /** Additional directories searched for imported modules and submodules. */
  std::vector<std::filesystem::path> search_paths;
  /** Complete initial XML configuration used when no snapshot is restored. */
  std::filesystem::path configuration;
  /** Optional atomic datastore snapshot restored and updated by the host. */
  std::optional<std::filesystem::path> state_file;
  /** Optional RFC 8341 NACM XML configuration loaded at startup. */
  std::optional<std::filesystem::path> nacm_configuration;
  /** Host-authenticated users whose sessions bypass NACM for recovery. */
  std::vector<std::string> recovery_users;
  /** POSIX shared libraries implementing versioned dangd plugin ABI v1. */
  std::vector<std::filesystem::path> plugins;
  /** In-memory startup configuration used by an atomic runtime reload. */
  std::optional<std::string> configuration_override;
};

struct LoadResult;

/** Owns the compiled schema, NETCONF datastores, and protocol service. */
class Application {
 public:
  /** Compiles, validates, and constructs an application from filesystem inputs. */
  [[nodiscard]] static LoadResult Load(const ApplicationOptions& options);
  /** Builds a replacement using the current running configuration. */
  [[nodiscard]] static LoadResult Reload(const ApplicationOptions& options,
                                         const Application& current);

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  /** Returns the unframed NETCONF RPC service owned by this application. */
  [[nodiscard]] yang::netconf::NetconfServer& server() noexcept {
    return server_;
  }
  /** Returns the datastore manager owned by this application. */
  [[nodiscard]] yang::netconf::DatastoreManager& datastores() noexcept {
    return datastores_;
  }
  /** Returns the immutable runtime schema shared by all configuration layers. */
  [[nodiscard]] const yang::config::RuntimeSchema& schema() const noexcept {
    return schema_;
  }
  /** Returns whether persistent datastore storage was configured. */
  [[nodiscard]] bool has_state_file() const noexcept {
    return state_file_.has_value();
  }
  /** Saves all persistent datastore state when a state path was configured. */
  [[nodiscard]] std::optional<std::string> SaveState() const;
  /** Drains English descriptions emitted since the preceding drain. */
  [[nodiscard]] std::vector<std::string> DrainBackendDeltas() {
    return backend_.DrainDeltas();
  }
  /** Returns an immutable copy of the backend's current working configuration. */
  [[nodiscard]] yang::config::ConfigDocument working_configuration() const {
    return backend_.Working();
  }
  /** Returns the current RFC 8525 library content identifier. */
  [[nodiscard]] std::string yang_library_content_id() const {
    return operational_.content_id();
  }
  /** Publishes an RFC 8525 update to subscribed sessions. */
  [[nodiscard]] bool PublishYangLibraryUpdate(std::string_view content_id);

 private:
  Application(yang::config::RuntimeSchema schema,
              yang::config::ConfigDocument configuration,
              std::optional<std::filesystem::path> state_file,
              yang::netconf::NacmPolicy nacm, bool managed_nacm,
              std::unique_ptr<PluginManager> plugins,
              std::string yang_library_xml,
              std::vector<DangdOperationalData::ModelSource> model_sources);

  yang::config::RuntimeSchema schema_;
  std::unique_ptr<PluginManager> plugins_;
  yang::netconf::NacmPolicy nacm_;
  yang::netconf::NotificationManager notifications_;
  DangdOperationalData operational_;
  EnglishConfigurationBackend backend_;
  yang::netconf::DatastoreManager datastores_;
  yang::netconf::NetconfServer server_;
  std::optional<std::filesystem::path> state_file_;
};

/**
 * Result of loading an Application; exactly one of application or errors is
 * useful.
 */
struct LoadResult {
  /** Constructed application, or null when compilation or validation failed. */
  std::unique_ptr<Application> application;
  /** Human-readable startup diagnostics when application is null. */
  std::vector<std::string> errors;
};

/**
 * Runs one explicitly authenticated NETCONF session over a byte stream.
 *
 * This is intended for tests and supervised local integration. The streams do
 * not authenticate or encrypt the peer and must not be exposed as a network
 * transport.
 */
[[nodiscard]] int RunStreamSession(Application& application,
                                   std::istream& input, std::ostream& output,
                                   std::ostream& errors,
                                   std::uint32_t session_id,
                                   std::string authenticated_username);

}  // namespace dangd

#endif  // DANGD_APPLICATION_H_
