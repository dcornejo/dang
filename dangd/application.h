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

#include "yang/config_validation.h"
#include "yang/nacm.h"
#include "yang/netconf_datastore.h"
#include "yang/netconf_server.h"

namespace dangd {

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
};

struct LoadResult;

/** Owns the compiled schema, NETCONF datastores, and protocol service. */
class Application {
 public:
  /** Compiles, validates, and constructs an application from filesystem inputs. */
  [[nodiscard]] static LoadResult Load(const ApplicationOptions& options);

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

 private:
  Application(yang::config::RuntimeSchema schema,
              yang::config::ConfigDocument configuration,
              std::optional<std::filesystem::path> state_file,
              std::optional<yang::netconf::NacmPolicy> nacm);

  yang::config::RuntimeSchema schema_;
  EnglishConfigurationBackend backend_;
  yang::netconf::DatastoreManager datastores_;
  std::optional<yang::netconf::NacmPolicy> nacm_;
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
