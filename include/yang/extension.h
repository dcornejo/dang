// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_EXTENSION_H_
#define YANG_EXTENSION_H_

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "yang/semantic_model.h"

namespace yang::semantic {

struct ExtensionName {
  std::string module;
  std::string name;
  bool operator==(const ExtensionName&) const = default;
};

/** Resolved declaration metadata for a YANG extension. */
struct ExtensionDefinition {
  ExtensionName name;
  std::shared_ptr<const ResolvedModule> source_module;
  StatementId statement = kInvalidStatementId;
  std::optional<std::string> argument_name;
  bool yin_element = false;
};

/** One extension use linked to its resolved declaration. */
struct ExtensionInstance {
  ExtensionDefinition definition;
  std::shared_ptr<const ResolvedModule> source_module;
  StatementId statement = kInvalidStatementId;
  std::optional<std::string> argument;
};

/** Callback used for application-specific extension semantics. */
using ExtensionHandler =
    std::function<bool(const ExtensionInstance&, DiagnosticSink&)>;

/** Resolved extension definitions and instances in a module closure. */
class ExtensionContext {
 public:
  [[nodiscard]] const std::vector<ExtensionDefinition>& definitions() const {
    return definitions_;
  }
  [[nodiscard]] const std::vector<ExtensionInstance>& instances() const {
    return instances_;
  }

 private:
  friend class ExtensionResolver;
  std::vector<ExtensionDefinition> definitions_;
  std::vector<ExtensionInstance> instances_;
};

/** Resolves and validates extension uses and invokes optional client hooks. */
class ExtensionResolver {
 public:
  explicit ExtensionResolver(DiagnosticSink& diagnostics)
      : diagnostics_(diagnostics) {}

  [[nodiscard]] std::optional<ExtensionContext> Resolve(
      const SemanticContext& semantics,
      const std::vector<ExtensionHandler>& handlers = {});

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_EXTENSION_H_
