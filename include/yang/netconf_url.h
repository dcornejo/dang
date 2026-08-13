// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_URL_H_
#define YANG_NETCONF_URL_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace yang::netconf {

/** Result returned by a host-supplied URL datastore operation. */
struct UrlResult {
  std::optional<std::string> config_xml;
  std::optional<std::string> error;
  /** NETCONF error-tag for failures; defaults to operation-failed. */
  std::optional<std::string> error_tag;
};

/** Security boundary for RFC 6241 URL datastore access. */
class UrlDatastoreProvider {
 public:
  virtual ~UrlDatastoreProvider() = default;
  /** Returns lowercase URI schemes that the host permits. */
  [[nodiscard]] virtual std::vector<std::string> Schemes() const = 0;
  /** Reads a complete NETCONF config document. */
  [[nodiscard]] virtual UrlResult Read(std::string_view url) = 0;
  /** Atomically writes a complete NETCONF config document. */
  [[nodiscard]] virtual UrlResult Write(std::string_view url,
                                        std::string_view config_xml) = 0;
  /** Deletes the named external configuration. */
  [[nodiscard]] virtual UrlResult Delete(std::string_view url) = 0;
};

}  // namespace yang::netconf

#endif  // YANG_NETCONF_URL_H_
