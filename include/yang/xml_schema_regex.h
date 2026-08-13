// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_XML_SCHEMA_REGEX_H_
#define YANG_XML_SCHEMA_REGEX_H_

#include <memory>
#include <string_view>

namespace yang {

/** A cached, compiled XML Schema regular expression. */
class XmlSchemaRegex {
 public:
  /** Compiles an expression, returning null when its syntax is invalid. */
  [[nodiscard]] static std::shared_ptr<const XmlSchemaRegex> Compile(
      std::string_view expression);

  /** Returns whether the complete UTF-8 value matches the expression. */
  [[nodiscard]] bool Matches(std::string_view value) const;

 private:
  class Impl;
  explicit XmlSchemaRegex(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

}  // namespace yang

#endif  // YANG_XML_SCHEMA_REGEX_H_
