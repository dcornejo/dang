// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "yang/nacm.h"

namespace {

constexpr std::size_t kMaximumFuzzInput = 1024 * 1024;
constexpr std::string_view kPathMarker = "\n@@INSTANCE-PATH@@\n";
constexpr std::string_view kDefaultPath =
    "/{urn:fuzz}system/{urn:fuzz}name";
constexpr std::string_view kFilterData = R"xml(
  <data xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
    <system xmlns="urn:fuzz"><name>router</name></system>
  </data>)xml";

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (size == 0 || size > kMaximumFuzzInput) return 0;
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  std::size_t separator = input.find('\0');
  std::size_t separator_length = 1;
  if (separator == std::string_view::npos) {
    separator = input.find(kPathMarker);
    separator_length = kPathMarker.size();
  }
  const std::string_view policy_xml =
      separator == std::string_view::npos ? input : input.substr(0, separator);
  const std::string_view instance_path =
      separator == std::string_view::npos
          ? kDefaultPath
          : input.substr(separator + separator_length);

  auto loaded = yang::netconf::LoadNacmPolicy(policy_xml);
  if (!loaded.policy) return 0;
  for (const yang::netconf::AccessOperation operation :
       {yang::netconf::AccessOperation::kRead,
        yang::netconf::AccessOperation::kCreate,
        yang::netconf::AccessOperation::kUpdate,
        yang::netconf::AccessOperation::kDelete}) {
    (void)loaded.policy->AuthorizeData("fuzz-user", "fuzz", operation,
                                      instance_path);
  }
  (void)loaded.policy->AuthorizeRpc("fuzz-user", "fuzz", instance_path);
  (void)loaded.policy->AuthorizeAction("fuzz-user", "fuzz", instance_path,
                                      instance_path, {});
  (void)loaded.policy->AuthorizeNotification("fuzz-user", "fuzz",
                                             instance_path, instance_path, {});
  (void)loaded.policy->FilterReadableData("fuzz-user", kFilterData);
  return 0;
}
