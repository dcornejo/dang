// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_CONFIG_VALIDATION_H_
#define YANG_CONFIG_VALIDATION_H_

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <pugixml.hpp>

#include "yang/compiler.h"

namespace yang::config {

using RuntimeSchemaNodeId = std::uint32_t;
using ConfigNodeId = std::uint32_t;
using EffectiveNodeId = std::uint32_t;
inline constexpr RuntimeSchemaNodeId kInvalidRuntimeSchemaNodeId =
    std::numeric_limits<RuntimeSchemaNodeId>::max();
inline constexpr ConfigNodeId kInvalidConfigNodeId =
    std::numeric_limits<ConfigNodeId>::max();
inline constexpr EffectiveNodeId kInvalidEffectiveNodeId =
    std::numeric_limits<EffectiveNodeId>::max();

/** Expanded XML name; prefixes are intentionally not semantic. */
struct QualifiedXmlName {
  std::string namespace_uri;
  std::string local_name;
  bool operator==(const QualifiedXmlName&) const = default;
};

/** One in-scope XML namespace binding retained for QName-valued scalars. */
struct XmlNamespaceBinding {
  std::string prefix;
  std::string namespace_uri;
};

/** Runtime behavior needed beyond an ordinary scalar value space. */
enum class RuntimeReferenceKind {
  kNone,
  kIdentityRef,
  kLeafRef,
  kInstanceIdentifier,
};

/** Runtime-resolved equality used to select a leafref list instance. */
struct RuntimeLeafrefPredicate {
  RuntimeSchemaNodeId list = kInvalidRuntimeSchemaNodeId;
  RuntimeSchemaNodeId key = kInvalidRuntimeSchemaNodeId;
  RuntimeSchemaNodeId source = kInvalidRuntimeSchemaNodeId;
};

/** Runtime XPath constraint with its prefix and diagnostic metadata. */
struct RuntimeXPathConstraint {
  semantic::XPathConstraintKind kind = semantic::XPathConstraintKind::kMust;
  RuntimeSchemaNodeId context_schema = kInvalidRuntimeSchemaNodeId;
  std::string expression;
  std::vector<XmlNamespaceBinding> namespaces;
  std::optional<std::string> error_message;
  std::optional<std::string> error_app_tag;
};

/** Reference-valued alternative nested inside an effective union type. */
struct RuntimeReferenceAlternative {
  RuntimeReferenceKind kind = RuntimeReferenceKind::kNone;
  bool require_instance = true;
  std::optional<RuntimeSchemaNodeId> leafref_target;
  std::shared_ptr<const semantic::ResolvedType> scalar_type;
  std::vector<QualifiedXmlName> identity_values;
};

/** Immutable schema node optimized for configuration data binding. */
struct RuntimeSchemaNode {
  RuntimeSchemaNodeId id = kInvalidRuntimeSchemaNodeId;
  std::optional<RuntimeSchemaNodeId> parent;
  semantic::SchemaNodeKind kind = semantic::SchemaNodeKind::kContainer;
  QualifiedXmlName name;
  /** YANG module that defines this effective schema node. */
  std::string module_name;
  bool config = true;
  bool mandatory = false;
  bool presence_container = false;
  bool supported = true;
  /** Effective RFC 8341 default-deny-all annotation, including inheritance. */
  bool nacm_default_deny_all = false;
  /** Effective RFC 8341 default-deny-write annotation, including inheritance. */
  bool nacm_default_deny_write = false;
  std::optional<std::uint64_t> min_elements;
  std::optional<std::uint64_t> max_elements;
  bool max_elements_unbounded = false;
  bool ordered_by_user = false;
  std::shared_ptr<const semantic::ResolvedType> type;
  RuntimeReferenceKind reference_kind = RuntimeReferenceKind::kNone;
  bool require_instance = true;
  std::optional<RuntimeSchemaNodeId> leafref_target;
  std::vector<RuntimeLeafrefPredicate> leafref_predicates;
  std::vector<QualifiedXmlName> identity_values;
  std::vector<RuntimeReferenceAlternative> union_references;
  std::optional<std::string> default_value;
  std::vector<std::string> default_values;
  std::vector<RuntimeSchemaNodeId> keys;
  std::vector<std::vector<RuntimeSchemaNodeId>> unique;
  std::vector<RuntimeXPathConstraint> xpath_constraints;
  std::vector<RuntimeSchemaNodeId> children;
};

struct ConfigParseOptions;
struct ConfigParseResult;
class XmlBinder;
class ConfigValidator;
class ConfigEditor;

/** Immutable, dependency-closure-wide effective schema. */
class RuntimeSchema {
 public:
  [[nodiscard]] const RuntimeSchemaNode& Get(RuntimeSchemaNodeId id) const;
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  [[nodiscard]] const std::vector<RuntimeSchemaNodeId>& roots() const noexcept {
    return roots_;
  }
  [[nodiscard]] std::optional<RuntimeSchemaNodeId> FindRoot(
      const QualifiedXmlName& name) const;
  [[nodiscard]] std::optional<RuntimeSchemaNodeId> FindChild(
      RuntimeSchemaNodeId parent, const QualifiedXmlName& name) const;
  /** Finds a supported top-level RPC or notification by expanded name. */
  [[nodiscard]] std::optional<RuntimeSchemaNodeId> FindTopLevelOperation(
      const QualifiedXmlName& name, semantic::SchemaNodeKind kind) const;
  /** Finds a supported action or notification below a data node. */
  [[nodiscard]] std::optional<RuntimeSchemaNodeId> FindChildOperation(
      RuntimeSchemaNodeId parent, const QualifiedXmlName& name,
      semantic::SchemaNodeKind kind) const;
  /** Resolves an expanded-name instance path, ignoring instance predicates. */
  [[nodiscard]] std::vector<RuntimeSchemaNodeId> ResolveInstancePath(
      std::string_view path) const;
  /** Returns visible input or output data children for an RPC/action. */
  [[nodiscard]] std::vector<RuntimeSchemaNodeId> OperationDataChildren(
      RuntimeSchemaNodeId operation, semantic::SchemaNodeKind io_kind) const;
  /** Returns supported data children with choices and cases flattened. */
  [[nodiscard]] std::vector<RuntimeSchemaNodeId> DataChildren(
      RuntimeSchemaNodeId parent) const;
  /** Returns whether an identity equals or transitively derives from a base. */
  [[nodiscard]] bool IdentityIsDerivedFrom(
      const QualifiedXmlName& identity,
      const QualifiedXmlName& base) const;

 private:
  friend class RuntimeSchemaBuilder;
  std::vector<RuntimeSchemaNode> nodes_;
  std::vector<RuntimeSchemaNodeId> roots_;
  std::vector<std::pair<QualifiedXmlName, QualifiedXmlName>>
      identity_derivations_;
};

/** Lowers existing semantic compilation results to a runtime schema. */
class RuntimeSchemaBuilder {
 public:
  [[nodiscard]] static RuntimeSchema FromCompilation(
      const Compilation& compilation);
  /** Compiles source-equivalent RFC 7950 YIN into the same runtime schema. */
  [[nodiscard]] static std::optional<RuntimeSchema> FromYin(
      const pugi::xml_document& yin, ModuleSourceRepository& repository,
      DiagnosticSink& diagnostics);
};

/** Whether omitted siblings and entries are known to be absent. */
enum class Coverage { kComplete, kSelected };

/** One explicitly present XML configuration node. */
struct ConfigNode {
  ConfigNodeId id = kInvalidConfigNodeId;
  RuntimeSchemaNodeId schema = kInvalidRuntimeSchemaNodeId;
  std::optional<ConfigNodeId> parent;
  QualifiedXmlName name;
  std::optional<std::string> value;
  std::vector<XmlNamespaceBinding> value_namespaces;
  Coverage child_coverage = Coverage::kComplete;
  std::vector<ConfigNodeId> children;
  std::optional<SourceRange> source_range;
};

/** Stable explicit configuration tree; defaults are never materialized. */
class ConfigDocument {
 public:
  [[nodiscard]] const ConfigNode& Get(ConfigNodeId id) const;
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  [[nodiscard]] const std::vector<ConfigNodeId>& roots() const noexcept {
    return roots_;
  }
  [[nodiscard]] std::string_view source() const noexcept { return source_; }
  /** Serializes explicit nodes as deterministic NETCONF XML configuration. */
  [[nodiscard]] std::string ToXml(bool netconf_wrapper = true) const;
  /** Returns a copy with completeness changed for one node's child set. */
  [[nodiscard]] ConfigDocument WithChildCoverage(ConfigNodeId node,
                                                 Coverage coverage) const;
  /** Returns a copy with completeness changed for one child collection. */
  [[nodiscard]] ConfigDocument WithCollectionCoverage(
      std::optional<ConfigNodeId> parent, RuntimeSchemaNodeId child_schema,
      Coverage coverage) const;
  [[nodiscard]] std::optional<Coverage> CollectionCoverage(
      std::optional<ConfigNodeId> parent,
      RuntimeSchemaNodeId child_schema) const;

 private:
  friend class XmlBinder;
  friend class ConfigValidator;
  friend class ConfigEditor;
  friend struct ConfigParseResult;
  friend ConfigParseResult ParseDatastoreXml(const RuntimeSchema&,
                                             std::string_view,
                                             const ConfigParseOptions&);
  std::string source_;
  std::vector<ConfigNode> nodes_;
  std::vector<ConfigNodeId> roots_;
  struct CollectionCoverageEntry {
    std::optional<ConfigNodeId> parent;
    RuntimeSchemaNodeId schema = kInvalidRuntimeSchemaNodeId;
    Coverage coverage = Coverage::kComplete;
  };
  std::vector<CollectionCoverageEntry> collection_coverage_;
};

/** Explicit or virtual node exposed by the immutable effective-data view. */
struct EffectiveNode {
  EffectiveNodeId id = kInvalidEffectiveNodeId;
  RuntimeSchemaNodeId schema = kInvalidRuntimeSchemaNodeId;
  std::optional<EffectiveNodeId> parent;
  std::optional<ConfigNodeId> explicit_node;
  std::optional<std::string> value;
  std::vector<EffectiveNodeId> children;
};

/** Read-only explicit-plus-default data view; it never mutates stored data. */
class EffectiveDataView {
 public:
  [[nodiscard]] static EffectiveDataView Build(const RuntimeSchema& schema,
                                               const ConfigDocument& document,
                                               bool allow_state_data = false);
  [[nodiscard]] const EffectiveNode& Get(EffectiveNodeId id) const;
  [[nodiscard]] const std::vector<EffectiveNodeId>& roots() const noexcept {
    return roots_;
  }
  [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
  /**
   * Returns a copy with one valueless node used to evaluate an absent `when`.
   *
   * @param schema Runtime node represented by the placeholder.
   * @param parent Effective parent, or no value for a top-level node.
   * @param placeholder Receives the new effective-node identity.
   */
  [[nodiscard]] EffectiveDataView WithPlaceholder(
      RuntimeSchemaNodeId schema, std::optional<EffectiveNodeId> parent,
      EffectiveNodeId* placeholder) const;

 private:
  std::vector<EffectiveNode> nodes_;
  std::vector<EffectiveNodeId> roots_;
};

/** Completeness contract applied while validating a configuration document. */
enum class ValidationScope { kComplete, kPartialStandalone, kPartialWithContext };
/** Whether a finding proves invalidity or reflects unavailable context. */
enum class FindingState { kInvalid, kIndeterminate };
/** Stable machine-readable categories emitted by parsing and validation. */
enum class ValidationCode {
  kMalformedXml,
  kUnknownDataNode,
  kInvalidNodeShape,
  kStateDataInConfiguration,
  kDuplicateNode,
  kInvalidValue,
  kUnresolvedReference,
  kWhenViolation,
  kMustViolation,
  kXPathIndeterminate,
  kMissingKey,
  kDuplicateListKey,
  kUniqueViolation,
  kMissingMandatoryNode,
  kElementCount,
  kChoiceConflict,
  kMissingMandatoryChoice,
  kContextRequired,
  kContextEvaluationUnsupported,
  kResourceLimitExceeded,
};

/** One deterministic configuration validation finding. */
struct ValidationFinding {
  ValidationCode code = ValidationCode::kMalformedXml;
  FindingState state = FindingState::kInvalid;
  std::string message;
  std::string instance_path;
  std::string netconf_error_tag;
  std::string netconf_error_app_tag;
  /** RFC 6241 XPath reported in error-path, when distinct from instance_path. */
  std::string netconf_error_path;
  /** Namespace URI bound to the `op` prefix in netconf_error_path. */
  std::string netconf_error_path_namespace;
  std::optional<SourceRange> source_range;
  std::optional<ConfigNodeId> config_node;
  /** YANG module defining the schema node responsible for this finding. */
  std::string module_name;
};

struct ConfigParseOptions {
  Coverage coverage = Coverage::kComplete;
  std::optional<RuntimeSchemaNodeId> attachment_parent;
  /** Accepts and validates RFC 8342 ietf-origin metadata on instance data. */
  bool allow_origin_metadata = false;
};

struct ConfigParseResult {
  std::optional<ConfigDocument> document;
  std::vector<ValidationFinding> findings;
};

/** Parses and schema-binds NETCONF XML configuration content. */
[[nodiscard]] ConfigParseResult ParseDatastoreXml(
    const RuntimeSchema& schema, std::string_view xml,
    const ConfigParseOptions& options = {});

struct ValidationRequest {
  ValidationRequest(const RuntimeSchema& request_schema,
                    const ConfigDocument& request_document,
                    ValidationScope request_scope = ValidationScope::kComplete,
                    const ConfigDocument* request_context = nullptr,
                    std::optional<ConfigNodeId> request_attachment = std::nullopt,
                    bool request_allow_state_data = false,
                    bool request_complete_config_context = false)
      : schema(request_schema),
        document(request_document),
        scope(request_scope),
        context(request_context),
        context_attachment(request_attachment),
        allow_state_data(request_allow_state_data),
        complete_config_context(request_complete_config_context) {}
  const RuntimeSchema& schema;
  const ConfigDocument& document;
  ValidationScope scope = ValidationScope::kComplete;
  const ConfigDocument* context = nullptr;
  std::optional<ConfigNodeId> context_attachment;
  /** Allows config-false nodes while retaining all other instance checks. */
  bool allow_state_data = false;
  /** Configuration nodes are complete context during partial state validation. */
  bool complete_config_context = false;
};

struct ValidationResult {
  bool valid = false;
  bool complete = false;
  std::vector<ValidationFinding> findings;
};

/** Pure structural and value validator for explicit config or instance trees. */
class ConfigValidator {
 public:
  [[nodiscard]] ValidationResult Validate(
      const ValidationRequest& request) const;
};

}  // namespace yang::config

#endif  // YANG_CONFIG_VALIDATION_H_
