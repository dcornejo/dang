// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SCHEMA_TREE_H_
#define YANG_SCHEMA_TREE_H_

#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

#include "yang/type_system.h"

namespace yang::semantic {

class FeatureSet;

using SchemaNodeId = std::uint32_t;
inline constexpr SchemaNodeId kInvalidSchemaNodeId =
    std::numeric_limits<SchemaNodeId>::max();

enum class SchemaNodeKind {
  kContainer, kList, kLeaf, kLeafList, kChoice, kCase, kAnydata, kAnyxml,
  kRpc, kAction, kInput, kOutput, kNotification,
};

enum class SchemaNodeOrigin { kDeclared, kUses, kAugment, kImplicitCase };

struct SchemaName {
  std::string module;
  std::string local_name;
  bool operator==(const SchemaName&) const = default;
};

/** A constraint statement retained with the module that defines its prefixes. */
struct SchemaConstraint {
  const ResolvedModule* source_module = nullptr;
  StatementId statement = kInvalidStatementId;
  std::optional<SchemaNodeId> context_node;
};

/** One node in the effective schema tree. */
struct SchemaNode {
  SchemaNodeId id = kInvalidSchemaNodeId;
  std::optional<SchemaNodeId> parent;
  SchemaNodeKind kind = SchemaNodeKind::kContainer;
  SchemaNodeOrigin origin = SchemaNodeOrigin::kDeclared;
  SchemaName name;
  std::shared_ptr<const ResolvedModule> source_module;
  StatementId declaration = kInvalidStatementId;
  StatementId instantiation = kInvalidStatementId;
  std::optional<bool> declared_config;
  bool effective_config = true;
  std::optional<bool> declared_mandatory;
  bool mandatory = false;
  bool presence_container = false;
  std::optional<std::string> default_value;
  std::vector<std::string> default_values;
  std::optional<std::string> units;
  std::optional<std::uint64_t> min_elements;
  std::optional<std::uint64_t> max_elements;
  bool max_elements_unbounded = false;
  bool ordered_by_user = false;
  bool supported = true;
  std::shared_ptr<const ResolvedType> type;
  std::vector<SchemaNodeId> key;
  std::vector<std::vector<SchemaNodeId>> unique;
  std::vector<SchemaConstraint> must_constraints;
  std::vector<SchemaConstraint> when_constraints;
  std::vector<SchemaNodeId> children;
};

/** Stable arena containing the effective schema for one module. */
class SchemaTree {
 public:
  [[nodiscard]] const std::vector<SchemaNodeId>& roots() const noexcept { return roots_; }
  [[nodiscard]] const SchemaNode& Get(SchemaNodeId id) const;
  [[nodiscard]] SchemaNode& Get(SchemaNodeId id);
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  [[nodiscard]] std::optional<SchemaNodeId> FindChild(
      std::optional<SchemaNodeId> parent, const SchemaName& name) const;

 private:
  friend class SchemaBuilder;
  friend class SchemaContextBuilder;
  friend class DeviationApplier;
  [[nodiscard]] SchemaNodeId Add(SchemaNode node);
  void DisableSubtree(SchemaNodeId id);
  std::deque<SchemaNode> nodes_;
  std::vector<SchemaNodeId> roots_;
};

/** Constructs an effective schema, expanding groupings and local refinements. */
class SchemaBuilder {
 public:
  explicit SchemaBuilder(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<SchemaTree> Build(
      const SemanticContext& semantics, const TypeContext& types,
      const FeatureSet* features = nullptr);
  [[nodiscard]] std::optional<SchemaTree> BuildFor(
      const SemanticContext& semantics, const TypeContext& types,
      std::shared_ptr<const ModuleSymbols> module,
      const FeatureSet* features = nullptr);

 private:
  DiagnosticSink& diagnostics_;
};

/** Effective trees for every module in a resolved dependency closure. */
class SchemaContext {
 public:
  [[nodiscard]] const SchemaTree* Find(const ResolvedModule& module) const;
  [[nodiscard]] SchemaTree* Find(const ResolvedModule& module);
  [[nodiscard]] const std::shared_ptr<const ResolvedModule>& root_module() const noexcept {
    return root_module_;
  }
  [[nodiscard]] const SchemaTree& root() const;
  [[nodiscard]] std::vector<const ResolvedModule*> modules() const;

 private:
  friend class SchemaContextBuilder;
  friend class DeviationApplier;
  std::shared_ptr<const ResolvedModule> root_module_;
  std::unordered_map<const ResolvedModule*, SchemaTree> trees_;
};

/** Builds all canonical trees and applies cross-module augments. */
class SchemaContextBuilder {
 public:
  explicit SchemaContextBuilder(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<SchemaContext> Build(
      const SemanticContext& semantics, const TypeContext& types,
      const FeatureSet* features = nullptr);

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_SCHEMA_TREE_H_
