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

#include "yang/config_validation.h"
#include "yang/netconf_datastore.h"
#include "yang/netconf_server.h"

namespace dangd {

struct ApplicationOptions {
  std::filesystem::path model;
  std::vector<std::filesystem::path> search_paths;
  std::filesystem::path configuration;
  std::optional<std::filesystem::path> state_file;
};

struct LoadResult;

/** Owns the compiled schema, NETCONF datastores, and protocol service. */
class Application {
 public:
  [[nodiscard]] static LoadResult Load(const ApplicationOptions& options);

  Application(const Application&) = delete;
  Application& operator=(const Application&) = delete;

  [[nodiscard]] yang::netconf::NetconfServer& server() noexcept {
    return server_;
  }
  [[nodiscard]] yang::netconf::DatastoreManager& datastores() noexcept {
    return datastores_;
  }
  [[nodiscard]] const yang::config::RuntimeSchema& schema() const noexcept {
    return schema_;
  }
  [[nodiscard]] bool has_state_file() const noexcept {
    return state_file_.has_value();
  }
  /** Saves all persistent datastore state when a state path was configured. */
  [[nodiscard]] std::optional<std::string> SaveState() const;

 private:
  Application(yang::config::RuntimeSchema schema,
              yang::config::ConfigDocument configuration,
              std::optional<std::filesystem::path> state_file);

  yang::config::RuntimeSchema schema_;
  yang::netconf::DatastoreManager datastores_;
  yang::netconf::NetconfServer server_;
  std::optional<std::filesystem::path> state_file_;
};

struct LoadResult {
  std::unique_ptr<Application> application;
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
