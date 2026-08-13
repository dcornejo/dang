// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/xml_schema_regex.h"

#include <mutex>
#include <string>
#include <unordered_map>

#include <libxml/xmlregexp.h>

namespace yang {

class XmlSchemaRegex::Impl {
 public:
  explicit Impl(xmlRegexpPtr expression) : expression_(expression) {}
  ~Impl() { xmlRegFreeRegexp(expression_); }

  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;

  [[nodiscard]] bool Matches(std::string_view value) const {
    const std::string terminated(value);
    std::lock_guard lock(mutex_);
    return xmlRegexpExec(expression_,
                         reinterpret_cast<const xmlChar*>(terminated.c_str())) == 1;
  }

 private:
  xmlRegexpPtr expression_;
  mutable std::mutex mutex_;
};

XmlSchemaRegex::XmlSchemaRegex(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

std::shared_ptr<const XmlSchemaRegex> XmlSchemaRegex::Compile(
    std::string_view expression) {
  static std::mutex cache_mutex;
  static std::unordered_map<std::string, std::weak_ptr<const XmlSchemaRegex>>
      cache;

  const std::string key(expression);
  std::lock_guard lock(cache_mutex);
  if (const auto found = cache.find(key); found != cache.end()) {
    if (auto compiled = found->second.lock()) return compiled;
  }

  xmlRegexpPtr raw = xmlRegexpCompile(
      reinterpret_cast<const xmlChar*>(key.c_str()));
  if (raw == nullptr) return nullptr;
  auto compiled = std::shared_ptr<const XmlSchemaRegex>(
      new XmlSchemaRegex(std::make_shared<Impl>(raw)));
  cache[key] = compiled;
  return compiled;
}

bool XmlSchemaRegex::Matches(std::string_view value) const {
  return impl_->Matches(value);
}

}  // namespace yang
