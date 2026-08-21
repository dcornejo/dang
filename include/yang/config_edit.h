// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_CONFIG_EDIT_H_
#define YANG_CONFIG_EDIT_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "yang/config_validation.h"

namespace yang::config {

/** Core RFC 6241 edit-config node operations. */
enum class EditOperation { kMerge, kReplace, kCreate, kDelete, kRemove, kNone };

/** RFC 7950 ordered-by user insertion position. */
enum class InsertPosition { kFirst, kLast, kBefore, kAfter };

/** Insertion metadata carried in the YANG XML namespace. */
struct InsertDirective {
  InsertPosition position = InsertPosition::kLast;
  /** Raw RFC 7950 key predicate for lists or value for leaf-lists. */
  std::optional<std::string> anchor;
  /** True for yang:key; false for yang:value. */
  bool anchor_is_key = false;
  /** Namespace bindings in scope for a list key selector. */
  std::vector<XmlNamespaceBinding> namespaces;
};

struct EditParseResult;

/** Schema-bound edit payload plus per-node explicit operation metadata. */
class EditDocument {
 public:
  [[nodiscard]] const ConfigDocument& data() const noexcept { return data_; }
  [[nodiscard]] std::optional<EditOperation> operation(ConfigNodeId id) const;
  [[nodiscard]] std::optional<InsertDirective> insertion(
      ConfigNodeId id) const;

 private:
  friend struct EditParseResult;
  friend EditParseResult ParseEditXml(const RuntimeSchema&, std::string_view);
  friend class ConfigEditor;
  ConfigDocument data_;
  std::vector<std::optional<EditOperation>> operations_;
  std::vector<std::optional<InsertDirective>> insertions_;
};

struct EditParseResult {
  std::optional<EditDocument> document;
  std::vector<ValidationFinding> findings;
};

/** Parses NETCONF edit XML and retains operation attributes as metadata. */
[[nodiscard]] EditParseResult ParseEditXml(const RuntimeSchema& schema,
                                           std::string_view xml);

enum class ChangeKind {
  kCreated,
  kDeleted,
  kValueChanged,
  kSubtreeReplaced,
  /** Existing ordered-by user list or leaf-list instance changed position. */
  kMoved
};

/** One deterministic successful candidate change. */
struct ChangeEvent {
  ChangeKind kind = ChangeKind::kCreated;
  std::string instance_path;
  /** Previous scalar value, or one-based "position N" for a move. */
  std::optional<std::string> before;
  /** Resulting scalar value, or one-based "position N" for a move. */
  std::optional<std::string> after;
  RuntimeSchemaNodeId schema = kInvalidRuntimeSchemaNodeId;
};

/** Computes deterministic per-instance changes between complete trees. */
[[nodiscard]] std::vector<ChangeEvent> DiffConfigDocuments(
    const RuntimeSchema& schema, const ConfigDocument& before,
    const ConfigDocument& after);

struct EditRequest {
  EditRequest(const RuntimeSchema& request_schema,
              const ConfigDocument& request_target,
              const EditDocument& request_edit,
              EditOperation operation = EditOperation::kMerge,
              const ConfigDocument* request_context = nullptr,
              bool request_validate_candidate = true)
      : schema(request_schema), target(request_target), edit(request_edit),
        default_operation(operation), context(request_context),
        validate_candidate(request_validate_candidate) {}
  const RuntimeSchema& schema;
  const ConfigDocument& target;
  const EditDocument& edit;
  EditOperation default_operation = EditOperation::kMerge;
  const ConfigDocument* context = nullptr;
  bool validate_candidate = true;
};

struct EditResult {
  std::optional<ConfigDocument> candidate;
  std::vector<ValidationFinding> errors;
  /** Explicit requested changes reported to callers. */
  std::vector<ChangeEvent> changes;
  /** Validation side effects that RFC 8341 must not authorize separately. */
  std::vector<ChangeEvent> implicit_changes;
};

/** Plans and atomically applies core RFC 6241 edits to an immutable target. */
class ConfigEditor {
 public:
  [[nodiscard]] EditResult Apply(const EditRequest& request) const;
};

}  // namespace yang::config

#endif  // YANG_CONFIG_EDIT_H_
