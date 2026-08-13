// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_IDENTITY_FEATURE_H_
#define YANG_IDENTITY_FEATURE_H_

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "yang/semantic_model.h"

namespace yang::semantic {

struct QualifiedSymbolName {
  std::string module;
  std::string name;
  bool operator==(const QualifiedSymbolName&) const = default;
};

struct QualifiedSymbolNameHash {
  std::size_t operator()(const QualifiedSymbolName& value) const noexcept;
};

/** Evaluated feature state for a dependency closure. */
class FeatureSet {
 public:
  [[nodiscard]] bool IsEnabled(std::string_view module,
                               std::string_view feature) const;
  /** Evaluates an if-feature expression in a module's prefix context. */
  [[nodiscard]] std::optional<bool> Evaluate(
      const ResolvedModule& source, std::string_view expression) const;

 private:
  friend class IdentityFeatureResolver;
  std::unordered_map<QualifiedSymbolName, bool, QualifiedSymbolNameHash> states_;
};

/** Resolved identity inheritance graph. */
class IdentityGraph {
 public:
  [[nodiscard]] bool Contains(const QualifiedSymbolName& identity) const;
  [[nodiscard]] bool IsDerivedFrom(const QualifiedSymbolName& identity,
                                   const QualifiedSymbolName& base) const;
  [[nodiscard]] std::vector<QualifiedSymbolName> DerivedFrom(
      const QualifiedSymbolName& base) const;

 private:
  friend class IdentityFeatureResolver;
  std::unordered_map<QualifiedSymbolName, std::vector<QualifiedSymbolName>,
                     QualifiedSymbolNameHash> bases_;
};

struct IdentityFeatureContext {
  FeatureSet features;
  IdentityGraph identities;
};

/** Resolves feature dependencies and identity inheritance. */
class IdentityFeatureResolver {
 public:
  explicit IdentityFeatureResolver(DiagnosticSink& diagnostics)
      : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<IdentityFeatureContext> Resolve(
      const SemanticContext& semantics,
      const std::vector<QualifiedSymbolName>& requested_features = {});

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_IDENTITY_FEATURE_H_
