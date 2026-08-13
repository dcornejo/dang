// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_LEAFREF_H_
#define YANG_LEAFREF_H_

#include <optional>
#include <unordered_map>

#include "yang/schema_tree.h"

namespace yang::semantic {

struct SchemaNodeRef {
  const ResolvedModule* tree_module = nullptr;
  SchemaNodeId node = kInvalidSchemaNodeId;
  bool operator==(const SchemaNodeRef&) const = default;
};

struct SchemaNodeRefHash {
  std::size_t operator()(const SchemaNodeRef& value) const noexcept;
};

/** One compiler-resolved list-key equality in a leafref path predicate. */
struct LeafrefPredicate {
  SchemaNodeRef list;
  SchemaNodeRef key;
  SchemaNodeRef source;
};

/** Static leafref targets and their inherited effective types. */
class LeafrefContext {
 public:
  [[nodiscard]] std::optional<SchemaNodeRef> Target(
      const SchemaNodeRef& leafref) const;
  [[nodiscard]] std::shared_ptr<const ResolvedType> EffectiveType(
      const SchemaContext& schemas, const SchemaNodeRef& node) const;
  [[nodiscard]] const std::vector<LeafrefPredicate>& Predicates(
      const SchemaNodeRef& leafref) const;

 private:
  friend class LeafrefResolver;
  std::unordered_map<SchemaNodeRef, SchemaNodeRef, SchemaNodeRefHash> targets_;
  std::unordered_map<SchemaNodeRef, std::vector<LeafrefPredicate>,
                     SchemaNodeRefHash>
      predicates_;
};

/** Resolves RFC 7950 leafref paths against an effective schema context. */
class LeafrefResolver {
 public:
  explicit LeafrefResolver(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<LeafrefContext> Resolve(
      const SchemaContext& schemas);

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_LEAFREF_H_
