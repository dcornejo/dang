// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SOURCE_FILE_H_
#define YANG_SOURCE_FILE_H_

#include <memory>
#include <string>
#include <string_view>

#include "yang/diagnostic.h"
#include "yang/resource_limits.h"

namespace yang {

/** Immutable, validated UTF-8 YANG source. */
class SourceFile {
 public:
  [[nodiscard]] static std::shared_ptr<const SourceFile> Create(
      std::string name, std::string contents, DiagnosticSink& diagnostics,
      const ResourceLimits& limits = DefaultResourceLimits());
  [[nodiscard]] std::string_view name() const noexcept { return name_; }
  [[nodiscard]] std::string_view contents() const noexcept { return contents_; }

 private:
  SourceFile(std::string name, std::string contents)
      : name_(std::move(name)), contents_(std::move(contents)) {}
  std::string name_;
  std::string contents_;
};
}  // namespace yang
#endif  // YANG_SOURCE_FILE_H_
