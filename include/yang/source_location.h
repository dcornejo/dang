// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SOURCE_LOCATION_H_
#define YANG_SOURCE_LOCATION_H_

#include <cstddef>

namespace yang {

/** Identifies a position in UTF-8 source. Lines and code-point columns are one-based. */
struct SourceLocation {
  std::size_t byte_offset = 0;
  std::size_t line = 1;
  std::size_t column = 1;
  bool operator==(const SourceLocation&) const = default;
};

/** A half-open source range. */
struct SourceRange {
  SourceLocation begin;
  SourceLocation end;
  bool operator==(const SourceRange&) const = default;
};

}  // namespace yang
#endif  // YANG_SOURCE_LOCATION_H_
