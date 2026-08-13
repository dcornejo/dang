// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/source_file.h"

#include <fmt/format.h>

#include "yang/utf8.h"

namespace yang {
std::shared_ptr<const SourceFile> SourceFile::Create(
    std::string name, std::string contents, DiagnosticSink& diagnostics,
    const ResourceLimits& limits) {
  if (contents.size() > limits.maximum_source_bytes) {
    diagnostics.Report({DiagnosticCode::kResourceLimitExceeded,
                        DiagnosticSeverity::kError,
                        "YANG source exceeds the byte limit", {}});
    return nullptr;
  }
  SourceLocation location;
  while (location.byte_offset < contents.size()) {
    const auto decoded = utf8::Decode(std::string_view(contents).substr(location.byte_offset));
    if (!decoded) {
      diagnostics.Report({DiagnosticCode::kInvalidUtf8, DiagnosticSeverity::kError,
          fmt::format("invalid UTF-8 sequence beginning with byte 0x{:02X}",
                      static_cast<unsigned char>(contents[location.byte_offset])),
          {location, {location.byte_offset + 1, location.line, location.column + 1}}});
      return nullptr;
    }
    const SourceLocation begin = location;
    location.byte_offset += decoded->byte_count;
    if (decoded->code_point == U'\n') { ++location.line; location.column = 1; }
    else { ++location.column; }
    if (!utf8::IsYangCharacter(decoded->code_point)) {
      diagnostics.Report({DiagnosticCode::kIllegalUnicodeCharacter, DiagnosticSeverity::kError,
          fmt::format("Unicode character U+{:04X} is not permitted in YANG source",
                      static_cast<std::uint32_t>(decoded->code_point)), {begin, location}});
      return nullptr;
    }
  }
  return std::shared_ptr<const SourceFile>(new SourceFile(std::move(name), std::move(contents)));
}
}  // namespace yang
