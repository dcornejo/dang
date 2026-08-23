// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "dangd/plugins/ip_management/platform_backend.h"

#include <memory>

namespace dangd::ip_management {
namespace {

class LoggingBackend final : public PlatformBackend {
 public:
  bool Reconcile(std::string_view, std::string_view, std::string*) override {
    return true;
  }
};

}  // namespace

std::unique_ptr<PlatformBackend> MakePlatformBackend() {
  return std::make_unique<LoggingBackend>();
}

}  // namespace dangd::ip_management
