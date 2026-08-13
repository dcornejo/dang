// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_NETCONF_WITH_DEFAULTS_H_
#define YANG_NETCONF_WITH_DEFAULTS_H_

#include <optional>
#include <string>
#include <string_view>

#include "yang/config_validation.h"

namespace yang::netconf {

/** RFC 6243 retrieval modes supported by the server. */
enum class WithDefaultsMode {
  kReportAll,
  kReportAllTagged,
  kTrim,
  kExplicit,
};

/** Opt-in RFC 6243 configuration; stored datastores use explicit basic mode. */
struct WithDefaultsConfig {};

/** Parses one RFC 6243 enumeration value. */
[[nodiscard]] std::optional<WithDefaultsMode> ParseWithDefaultsMode(
    std::string_view value);

/** Serializes a datastore as a NETCONF data element in the requested mode. */
[[nodiscard]] std::string SerializeWithDefaults(
    const config::RuntimeSchema& schema, const config::ConfigDocument& document,
    WithDefaultsMode mode);

}  // namespace yang::netconf

#endif  // YANG_NETCONF_WITH_DEFAULTS_H_
