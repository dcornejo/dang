// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef DANGD_PLUGINS_IP_MANAGEMENT_COMMAND_RUNNER_H_
#define DANGD_PLUGINS_IP_MANAGEMENT_COMMAND_RUNNER_H_

#include <string>
#include <vector>

namespace dangd::ip_management {

// Executes an absolute program path without a shell. Keeping arguments as a
// vector prevents interface names or addresses from becoming shell syntax.
bool RunCommand(const std::vector<std::string>& arguments, std::string* error);

}  // namespace dangd::ip_management

#endif
