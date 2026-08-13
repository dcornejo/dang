// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_TYPE_SYSTEM_H_
#define YANG_TYPE_SYSTEM_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "yang/semantic_model.h"
#include "yang/xml_schema_regex.h"

namespace yang::semantic {

enum class BuiltinType {
  kBinary, kBits, kBoolean, kDecimal64, kEmpty, kEnumeration, kIdentityRef,
  kInstanceIdentifier, kInt8, kInt16, kInt32, kInt64, kLeafRef, kString,
  kUint8, kUint16, kUint32, kUint64, kUnion,
};

struct Interval {
  std::string lower;
  std::string upper;
};

/** Identity name with its defining module namespace resolved. */
struct ResolvedIdentityName {
  std::string module;
  std::string name;
  bool operator==(const ResolvedIdentityName&) const = default;
};

/** Effective type information after following a typedef chain. */
struct ResolvedType {
  BuiltinType builtin = BuiltinType::kString;
  std::vector<std::string> typedef_chain;
  std::vector<Interval> ranges;
  std::vector<Interval> lengths;
  std::vector<std::string> patterns;
  std::vector<bool> pattern_inverted;
  std::vector<std::shared_ptr<const XmlSchemaRegex>> compiled_patterns;
  std::optional<std::uint8_t> fraction_digits;
  std::unordered_map<std::string, std::int32_t> enum_values;
  std::unordered_map<std::string, std::uint32_t> bit_positions;
  std::vector<std::shared_ptr<const ResolvedType>> union_members;
  std::vector<std::string> identity_bases;
  std::vector<ResolvedIdentityName> resolved_identity_bases;
  std::optional<std::string> leafref_path;
  bool require_instance = true;
};

/** Returns whether a lexical value belongs to a resolved type's value space. */
[[nodiscard]] bool ValueMatchesType(const ResolvedType& type,
                                    std::string_view value);

struct TypeKey {
  const ResolvedModule* source = nullptr;
  StatementId statement = kInvalidStatementId;
  bool operator==(const TypeKey&) const = default;
};

struct TypeKeyHash {
  std::size_t operator()(const TypeKey& key) const noexcept;
};

/** Results for type-bearing statements in a semantic dependency closure. */
class TypeContext {
 public:
  [[nodiscard]] std::shared_ptr<const ResolvedType> Find(
      const ResolvedModule& source, StatementId statement) const;

 private:
  friend class TypeResolver;
  std::unordered_map<TypeKey, std::shared_ptr<const ResolvedType>, TypeKeyHash> types_;
};

/** Resolves effective types and validates type restrictions and defaults. */
class TypeResolver {
 public:
  explicit TypeResolver(DiagnosticSink& diagnostics) : diagnostics_(diagnostics) {}
  [[nodiscard]] std::optional<TypeContext> Resolve(const SemanticContext& semantics);

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_TYPE_SYSTEM_H_
