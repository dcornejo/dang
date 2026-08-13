// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/type_system.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <functional>
#include <limits>
#include <cstdlib>
#include <sstream>
#include <unordered_set>

#include <fmt/format.h>

namespace yang::semantic {
namespace {

using Scope = std::unordered_map<std::string, Symbol>;

const Statement* FindChild(const ResolvedModule& source, const Statement& parent,
                           std::string_view keyword) {
  for (const StatementId id : parent.children) {
    const Statement& child = source.syntax->Get(id);
    if (child.keyword == keyword) return &child;
  }
  return nullptr;
}

std::optional<BuiltinType> ParseBuiltin(std::string_view name) {
  static const std::unordered_map<std::string_view, BuiltinType> types{
      {"binary", BuiltinType::kBinary}, {"bits", BuiltinType::kBits},
      {"boolean", BuiltinType::kBoolean}, {"decimal64", BuiltinType::kDecimal64},
      {"empty", BuiltinType::kEmpty}, {"enumeration", BuiltinType::kEnumeration},
      {"identityref", BuiltinType::kIdentityRef},
      {"instance-identifier", BuiltinType::kInstanceIdentifier},
      {"int8", BuiltinType::kInt8}, {"int16", BuiltinType::kInt16},
      {"int32", BuiltinType::kInt32}, {"int64", BuiltinType::kInt64},
      {"leafref", BuiltinType::kLeafRef}, {"string", BuiltinType::kString},
      {"uint8", BuiltinType::kUint8}, {"uint16", BuiltinType::kUint16},
      {"uint32", BuiltinType::kUint32}, {"uint64", BuiltinType::kUint64},
      {"union", BuiltinType::kUnion}};
  const auto found = types.find(name);
  return found == types.end() ? std::nullopt : std::optional<BuiltinType>(found->second);
}

std::string_view Trim(std::string_view value) {
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
  return value;
}

std::optional<std::vector<Interval>> ParseIntervals(std::string_view expression) {
  std::vector<Interval> result;
  while (!expression.empty()) {
    const std::size_t separator = expression.find('|');
    std::string_view part = Trim(expression.substr(0, separator));
    if (part.empty()) return std::nullopt;
    const std::size_t dots = part.find("..");
    if (dots == std::string_view::npos) {
      result.push_back({std::string(part), std::string(part)});
    } else {
      if (part.find("..", dots + 2) != std::string_view::npos) return std::nullopt;
      const std::string_view lower = Trim(part.substr(0, dots));
      const std::string_view upper = Trim(part.substr(dots + 2));
      if (lower.empty() || upper.empty()) return std::nullopt;
      result.push_back({std::string(lower), std::string(upper)});
    }
    if (separator == std::string_view::npos) break;
    expression.remove_prefix(separator + 1);
  }
  return result;
}

template <typename Integer>
std::optional<Integer> ParseInteger(std::string_view value) {
  Integer result{};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return std::nullopt;
  return result;
}

bool IsSignedInteger(BuiltinType type) {
  return type == BuiltinType::kInt8 || type == BuiltinType::kInt16 ||
         type == BuiltinType::kInt32 || type == BuiltinType::kInt64;
}

bool IsUnsignedInteger(BuiltinType type) {
  return type == BuiltinType::kUint8 || type == BuiltinType::kUint16 ||
         type == BuiltinType::kUint32 || type == BuiltinType::kUint64;
}

bool IsNumeric(BuiltinType type) {
  return IsSignedInteger(type) || IsUnsignedInteger(type) || type == BuiltinType::kDecimal64;
}

std::pair<long double, long double> BuiltinBounds(BuiltinType type) {
  switch (type) {
    case BuiltinType::kInt8: return {INT8_MIN, INT8_MAX};
    case BuiltinType::kInt16: return {INT16_MIN, INT16_MAX};
    case BuiltinType::kInt32: return {INT32_MIN, INT32_MAX};
    case BuiltinType::kInt64:
      return {static_cast<long double>(INT64_MIN),
              static_cast<long double>(INT64_MAX)};
    case BuiltinType::kUint8: return {0, UINT8_MAX};
    case BuiltinType::kUint16: return {0, UINT16_MAX};
    case BuiltinType::kUint32: return {0, UINT32_MAX};
    case BuiltinType::kUint64:
      return {0, static_cast<long double>(UINT64_MAX)};
    case BuiltinType::kDecimal64:
      return {static_cast<long double>(INT64_MIN),
              static_cast<long double>(INT64_MAX)};
    default: return {0, 0};
  }
}

std::optional<long double> ParseBound(std::string_view text,
                                      long double minimum,
                                      long double maximum) {
  if (text == "min") return minimum;
  if (text == "max") return maximum;
  std::string owned(text);
  char* end = nullptr;
  const long double value = std::strtold(owned.c_str(), &end);
  if (end != owned.c_str() + owned.size() || value < minimum ||
      value > maximum) {
    return std::nullopt;
  }
  return value;
}

bool ValidIntervals(const std::vector<Interval>& intervals,
                    long double minimum, long double maximum,
                    const std::vector<Interval>& base) {
  long double previous_upper = minimum;
  bool first = true;
  for (const Interval& interval : intervals) {
    const auto lower = ParseBound(interval.lower, minimum, maximum);
    const auto upper = ParseBound(interval.upper, minimum, maximum);
    if (!lower || !upper || *lower > *upper ||
        (!first && *lower <= previous_upper)) {
      return false;
    }
    if (!base.empty()) {
      bool contained = false;
      for (const Interval& allowed : base) {
        const auto allowed_lower = ParseBound(allowed.lower, minimum, maximum);
        const auto allowed_upper = ParseBound(allowed.upper, minimum, maximum);
        contained |= allowed_lower && allowed_upper &&
                     *lower >= *allowed_lower && *upper <= *allowed_upper;
      }
      if (!contained) return false;
    }
    previous_upper = *upper;
    first = false;
  }
  return !intervals.empty();
}

bool ValueInIntervals(long double value, const std::vector<Interval>& intervals,
                      long double minimum, long double maximum) {
  if (intervals.empty()) return true;
  for (const Interval& interval : intervals) {
    const auto lower = ParseBound(interval.lower, minimum, maximum);
    const auto upper = ParseBound(interval.upper, minimum, maximum);
    if (lower && upper && value >= *lower && value <= *upper) return true;
  }
  return false;
}

bool FitsSigned(BuiltinType type, std::int64_t value) {
  switch (type) {
    case BuiltinType::kInt8: return value >= INT8_MIN && value <= INT8_MAX;
    case BuiltinType::kInt16: return value >= INT16_MIN && value <= INT16_MAX;
    case BuiltinType::kInt32: return value >= INT32_MIN && value <= INT32_MAX;
    case BuiltinType::kInt64: return true;
    default: return false;
  }
}

bool FitsUnsigned(BuiltinType type, std::uint64_t value) {
  switch (type) {
    case BuiltinType::kUint8: return value <= UINT8_MAX;
    case BuiltinType::kUint16: return value <= UINT16_MAX;
    case BuiltinType::kUint32: return value <= UINT32_MAX;
    case BuiltinType::kUint64: return true;
    default: return false;
  }
}

std::optional<std::int64_t> ParseYangSignedDefault(std::string_view value) {
  std::string owned(value);
  char* end = nullptr;
  errno = 0;
  const long long parsed = std::strtoll(owned.c_str(), &end, 0);
  if (errno == ERANGE || end != owned.c_str() + owned.size()) return std::nullopt;
  return static_cast<std::int64_t>(parsed);
}

std::optional<std::uint64_t> ParseYangUnsignedDefault(std::string_view value) {
  if (!value.empty() && value.front() == '-') return std::nullopt;
  std::string owned(value);
  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(owned.c_str(), &end, 0);
  if (errno == ERANGE || end != owned.c_str() + owned.size()) return std::nullopt;
  return static_cast<std::uint64_t>(parsed);
}

bool ValidDecimal(std::string_view value, std::uint8_t fraction_digits) {
  if (value.empty()) return false;
  if (value.front() == '+' || value.front() == '-') value.remove_prefix(1);
  if (value.empty()) return false;
  const std::size_t dot = value.find('.');
  const std::string_view integer = value.substr(0, dot);
  const std::string_view fraction = dot == std::string_view::npos ? std::string_view{} : value.substr(dot + 1);
  if (integer.empty() || fraction.size() > fraction_digits ||
      (dot != std::string_view::npos && fraction.empty())) return false;
  return std::ranges::all_of(integer, [](unsigned char c) { return std::isdigit(c); }) &&
         std::ranges::all_of(fraction, [](unsigned char c) { return std::isdigit(c); });
}

std::optional<std::size_t> Base64DecodedLength(std::string_view value) {
  std::size_t encoded = 0;
  std::size_t padding = 0;
  bool saw_padding = false;
  for (const char raw_character : value) {
    const auto character = static_cast<unsigned char>(raw_character);
    if (std::isspace(character)) continue;
    if (character == '=') {
      saw_padding = true;
      if (++padding > 2) return std::nullopt;
    } else {
      const bool alphabet = std::isalnum(character) || character == '+' ||
                            character == '/';
      if (!alphabet || saw_padding) return std::nullopt;
    }
    ++encoded;
  }
  if (encoded == 0 || encoded % 4 != 0 || padding > encoded) {
    return std::nullopt;
  }
  return encoded / 4 * 3 - padding;
}

bool LexicalValueMatches(const ResolvedType& type, std::string_view value) {
  if (type.builtin == BuiltinType::kString) {
    std::size_t characters = 0;
    for (const char character : value) {
      const auto byte = static_cast<unsigned char>(character);
      characters += (byte & 0xC0U) != 0x80U;
    }
    if (!ValueInIntervals(static_cast<long double>(characters), type.lengths,
                          0, static_cast<long double>(UINT64_MAX))) {
      return false;
    }
    for (std::size_t index = 0; index < type.patterns.size(); ++index) {
      const auto compiled = index < type.compiled_patterns.size()
                                ? type.compiled_patterns[index]
                                : XmlSchemaRegex::Compile(type.patterns[index]);
      if (!compiled) return false;
      const bool inverted = index < type.pattern_inverted.size() &&
                            type.pattern_inverted[index];
      if (compiled->Matches(value) == inverted) return false;
    }
    return true;
  }
  if (type.builtin == BuiltinType::kBinary) {
    const auto length = Base64DecodedLength(value);
    return length && ValueInIntervals(static_cast<long double>(*length),
                                      type.lengths, 0,
                                      static_cast<long double>(UINT64_MAX));
  }
  if (type.builtin == BuiltinType::kIdentityRef || type.builtin == BuiltinType::kLeafRef ||
      type.builtin == BuiltinType::kInstanceIdentifier) return !value.empty();
  if (type.builtin == BuiltinType::kBoolean) return value == "true" || value == "false";
  if (IsSignedInteger(type.builtin)) {
    const auto parsed = ParseYangSignedDefault(value);
    const auto [minimum, maximum] = BuiltinBounds(type.builtin);
    return parsed && FitsSigned(type.builtin, *parsed) &&
           ValueInIntervals(static_cast<long double>(*parsed), type.ranges,
                            minimum, maximum);
  }
  if (IsUnsignedInteger(type.builtin)) {
    const auto parsed = ParseYangUnsignedDefault(value);
    const auto [minimum, maximum] = BuiltinBounds(type.builtin);
    return parsed && FitsUnsigned(type.builtin, *parsed) &&
           ValueInIntervals(static_cast<long double>(*parsed), type.ranges,
                            minimum, maximum);
  }
  if (type.builtin == BuiltinType::kDecimal64) {
    if (!type.fraction_digits || !ValidDecimal(value, *type.fraction_digits)) {
      return false;
    }
    const std::string owned(value);
    char* end = nullptr;
    const long double parsed = std::strtold(owned.c_str(), &end);
    const auto [minimum, maximum] = BuiltinBounds(type.builtin);
    return end == owned.c_str() + owned.size() &&
           ValueInIntervals(parsed, type.ranges, minimum, maximum);
  }
  if (type.builtin == BuiltinType::kEnumeration) {
    return type.enum_values.contains(std::string(value));
  }
  if (type.builtin == BuiltinType::kEmpty) return false;
  if (type.builtin == BuiltinType::kBits) {
    std::istringstream input{std::string(value)};
    std::string bit;
    bool found = false;
    while (input >> bit) {
      found = true;
      if (!type.bit_positions.contains(bit)) return false;
    }
    return found;
  }
  if (type.builtin == BuiltinType::kUnion) {
    return std::ranges::any_of(type.union_members, [&](const auto& member) {
      return LexicalValueMatches(*member, value);
    });
  }
  return false;
}

}  // namespace

bool ValueMatchesType(const ResolvedType& type, std::string_view value) {
  return LexicalValueMatches(type, value);
}

std::size_t TypeKeyHash::operator()(const TypeKey& key) const noexcept {
  return std::hash<const void*>{}(key.source) ^
         (std::hash<StatementId>{}(key.statement) << 1U);
}

std::shared_ptr<const ResolvedType> TypeContext::Find(
    const ResolvedModule& source, StatementId statement) const {
  const auto found = types_.find({&source, statement});
  return found == types_.end() ? nullptr : found->second;
}

std::optional<TypeContext> TypeResolver::Resolve(const SemanticContext& semantics) {
  if (!semantics.root()) return std::nullopt;
  TypeContext context;
  bool valid = true;
  std::unordered_set<TypeKey, TypeKeyHash> resolving;

  std::function<std::shared_ptr<const ResolvedType>(
      const std::shared_ptr<const ModuleSymbols>&,
      const std::shared_ptr<const ResolvedModule>&, const Statement&,
      const std::vector<Scope>&)> resolve_type;

  resolve_type = [&](const std::shared_ptr<const ModuleSymbols>& owner,
                     const std::shared_ptr<const ResolvedModule>& source,
                     const Statement& type_statement,
                     const std::vector<Scope>& scopes) -> std::shared_ptr<const ResolvedType> {
    const TypeKey key{source.get(), type_statement.id};
    if (const auto cached = context.types_.find(key); cached != context.types_.end()) return cached->second;
    if (!resolving.insert(key).second) {
      diagnostics_.Report({DiagnosticCode::kTypeCycle, DiagnosticSeverity::kError,
                           "typedef cycle detected", type_statement.range});
      valid = false;
      return nullptr;
    }
    if (!type_statement.argument) { resolving.erase(key); return nullptr; }
    auto result = std::make_shared<ResolvedType>();
    if (const auto builtin = ParseBuiltin(*type_statement.argument)) {
      result->builtin = *builtin;
    } else {
      std::string_view name = *type_statement.argument;
      std::optional<std::string_view> prefix;
      const std::size_t colon = name.find(':');
      if (colon != std::string_view::npos) { prefix = name.substr(0, colon); name.remove_prefix(colon + 1); }
      std::optional<Symbol> declaration;
      std::shared_ptr<const ModuleSymbols> target_owner = owner;
      if (!prefix) {
        for (auto scope = scopes.rbegin(); scope != scopes.rend() && !declaration; ++scope) {
          const auto found = scope->find(std::string(name));
          if (found != scope->end()) declaration = found->second;
        }
      } else if (*prefix == source->prefix || *prefix == owner->module()->prefix) {
        declaration = owner->Find(SymbolKind::kTypedef, name);
      } else {
        const auto imported = source->imports.find(std::string(*prefix));
        if (imported != source->imports.end()) {
          target_owner = semantics.FindModule(*imported->second);
          if (target_owner) declaration = target_owner->Find(SymbolKind::kTypedef, name);
        }
      }
      if (!declaration || !declaration->source_module->syntax) {
        resolving.erase(key);
        return nullptr;  // SymbolResolver already issued the primary diagnostic.
      }
      const Statement& typedef_statement = declaration->source_module->syntax->Get(declaration->statement);
      const Statement* underlying = FindChild(*declaration->source_module, typedef_statement, "type");
      if (!underlying) { resolving.erase(key); return nullptr; }
      std::vector<Scope> target_scopes;
      Scope top_level;
      for (const auto& [symbol_name, symbol] : target_owner->symbols(SymbolKind::kTypedef)) {
        top_level.emplace(symbol_name, symbol);
      }
      target_scopes.push_back(std::move(top_level));
      auto base = resolve_type(target_owner, declaration->source_module, *underlying,
                               prefix ? target_scopes : scopes);
      if (!base) { resolving.erase(key); return nullptr; }
      *result = *base;
      result->typedef_chain.insert(result->typedef_chain.begin(), *type_statement.argument);
    }

    std::int64_t next_enum = 0;
    std::uint64_t next_bit = 0;
    for (const StatementId child_id : type_statement.children) {
      const Statement& child = source->syntax->Get(child_id);
      if (child.keyword == "range" && child.argument) {
        if (!IsNumeric(result->builtin)) {
          diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                               "range is permitted only on numeric types", child.range}); valid = false;
        } else if (auto ranges = ParseIntervals(*child.argument)) {
          const auto [minimum, maximum] = BuiltinBounds(result->builtin);
          if (!ValidIntervals(*ranges, minimum, maximum, result->ranges)) {
            diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction,
                                 DiagnosticSeverity::kError,
                                 "range must be ordered, in bounds, and a subset of its base",
                                 child.range});
            valid = false;
          } else {
            result->ranges = std::move(*ranges);
          }
        } else { diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                                      "invalid range expression", child.range}); valid = false; }
      } else if (child.keyword == "length" && child.argument) {
        if (result->builtin != BuiltinType::kString && result->builtin != BuiltinType::kBinary) {
          diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                               "length is permitted only on string and binary types", child.range}); valid = false;
        } else if (auto lengths = ParseIntervals(*child.argument)) {
          if (!ValidIntervals(*lengths, 0,
                              static_cast<long double>(UINT64_MAX),
                              result->lengths)) {
            diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction,
                                 DiagnosticSeverity::kError,
                                 "length must be ordered, nonnegative, and a subset of its base",
                                 child.range});
            valid = false;
          } else {
            result->lengths = std::move(*lengths);
          }
        } else { diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                                      "invalid length expression", child.range}); valid = false; }
      } else if (child.keyword == "pattern" && child.argument) {
        if (result->builtin != BuiltinType::kString) {
          diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                               "pattern is permitted only on string types", child.range}); valid = false;
        } else {
          if (auto compiled = XmlSchemaRegex::Compile(*child.argument)) {
            result->patterns.push_back(*child.argument);
            result->compiled_patterns.push_back(std::move(compiled));
            const Statement* modifier = FindChild(*source, child, "modifier");
            result->pattern_inverted.push_back(
                modifier && modifier->argument == "invert-match");
          } else {
            diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction,
                                 DiagnosticSeverity::kError,
                                 "pattern is not a valid XML Schema regular expression",
                                 child.range});
            valid = false;
          }
        }
      } else if (child.keyword == "fraction-digits" && child.argument) {
        const auto digits = ParseInteger<unsigned>(*child.argument);
        if (result->builtin != BuiltinType::kDecimal64 || !digits || *digits < 1 || *digits > 18) {
          diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                               "decimal64 fraction-digits must be between 1 and 18", child.range}); valid = false;
        } else result->fraction_digits = static_cast<std::uint8_t>(*digits);
      } else if (child.keyword == "enum" && child.argument) {
        const Statement* value_statement = FindChild(*source, child, "value");
        const std::int64_t value = value_statement && value_statement->argument
            ? ParseInteger<std::int64_t>(*value_statement->argument).value_or(INT64_MAX) : next_enum;
        if (result->builtin != BuiltinType::kEnumeration || value < INT32_MIN || value > INT32_MAX ||
            result->enum_values.contains(*child.argument)) {
          diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                               "invalid or duplicate enumeration member", child.range}); valid = false;
        } else {
          for (const auto& [enum_name, existing] : result->enum_values) {
            (void)enum_name; if (existing == value) { valid = false; diagnostics_.Report({
                DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                "duplicate enumeration numeric value", child.range}); break; }
          }
          result->enum_values[*child.argument] = static_cast<std::int32_t>(value); next_enum = value + 1;
        }
      } else if (child.keyword == "bit" && child.argument) {
        const Statement* position_statement = FindChild(*source, child, "position");
        const std::uint64_t position = position_statement && position_statement->argument
            ? ParseInteger<std::uint64_t>(*position_statement->argument).value_or(UINT64_MAX) : next_bit;
        bool duplicate_position = false;
        for (const auto& [bit_name, existing] : result->bit_positions) {
          (void)bit_name; duplicate_position |= existing == position;
        }
        if (result->builtin != BuiltinType::kBits || position > UINT32_MAX || duplicate_position ||
            result->bit_positions.contains(*child.argument)) {
          diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                               "invalid or duplicate bit position", child.range}); valid = false;
        } else {
          result->bit_positions[*child.argument] = static_cast<std::uint32_t>(position); next_bit = position + 1;
        }
      } else if (child.keyword == "type" && result->builtin == BuiltinType::kUnion) {
        if (auto member = resolve_type(owner, source, child, scopes)) result->union_members.push_back(std::move(member));
      } else if (child.keyword == "base" && child.argument && result->builtin == BuiltinType::kIdentityRef) {
        result->identity_bases.push_back(*child.argument);
        std::string_view identity_name = *child.argument;
        std::string identity_module = source->belongs_to.value_or(source->name);
        const std::size_t colon = identity_name.find(':');
        if (colon != std::string_view::npos) {
          const std::string prefix(identity_name.substr(0, colon));
          identity_name.remove_prefix(colon + 1);
          if (prefix != source->prefix) {
            const auto imported = source->imports.find(prefix);
            if (imported != source->imports.end()) {
              identity_module = imported->second->name;
            }
          }
        }
        result->resolved_identity_bases.push_back(
            {std::move(identity_module), std::string(identity_name)});
      } else if (child.keyword == "path" && child.argument && result->builtin == BuiltinType::kLeafRef) {
        result->leafref_path = *child.argument;
      } else if (child.keyword == "require-instance" && child.argument &&
                 (result->builtin == BuiltinType::kLeafRef || result->builtin == BuiltinType::kInstanceIdentifier)) {
        result->require_instance = *child.argument != "false";
      }
    }
    if (result->builtin == BuiltinType::kDecimal64 && !result->fraction_digits) {
      diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                           "decimal64 requires fraction-digits", type_statement.range}); valid = false;
    }
    if (result->builtin == BuiltinType::kUnion && result->union_members.empty()) {
      diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                           "union requires at least one member type", type_statement.range}); valid = false;
    }
    if (result->builtin == BuiltinType::kLeafRef && !result->leafref_path) {
      diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                           "leafref requires a path", type_statement.range}); valid = false;
    }
    if (result->builtin == BuiltinType::kIdentityRef && result->identity_bases.empty()) {
      diagnostics_.Report({DiagnosticCode::kInvalidTypeRestriction, DiagnosticSeverity::kError,
                           "identityref requires at least one base", type_statement.range}); valid = false;
    }
    resolving.erase(key);
    context.types_[key] = result;
    return result;
  };

  const auto validate_default = [&](const ResolvedType& type, const Statement& statement) {
    if (!statement.argument) return;
    const std::string_view value = *statement.argument;
    const bool ok = ValueMatchesType(type, value);
    if (!ok) {
      diagnostics_.Report({DiagnosticCode::kInvalidDefaultValue, DiagnosticSeverity::kError,
                           fmt::format("'{}' is not a valid default for this type", value), statement.range});
      valid = false;
    }
  };

  std::unordered_set<const ResolvedModule*> visited;
  std::function<void(const std::shared_ptr<const ModuleSymbols>&)> analyze_module;
  analyze_module = [&](const std::shared_ptr<const ModuleSymbols>& owner) {
    if (!owner || !visited.insert(owner->module().get()).second) return;
    const auto analyze_unit = [&](const std::shared_ptr<const ResolvedModule>& source) {
      if (!source->syntax || source->syntax->roots().empty()) return;
      std::vector<Scope> scopes;
      Scope root_scope;
      for (const auto& [name, symbol] : owner->symbols(SymbolKind::kTypedef)) root_scope.emplace(name, symbol);
      scopes.push_back(std::move(root_scope));
      std::function<void(const Statement&, bool)> visit;
      visit = [&](const Statement& statement, bool root_statement) {
        Scope local;
        if (!root_statement) {
          for (const StatementId child_id : statement.children) {
            const Statement& child = source->syntax->Get(child_id);
            if (child.keyword == "typedef" && child.argument) {
              local.emplace(*child.argument, Symbol{SymbolKind::kTypedef, *child.argument, source, child_id});
            }
          }
        }
        const bool pushed = !local.empty();
        if (pushed) scopes.push_back(std::move(local));
        if (const Statement* type = FindChild(*source, statement, "type")) {
          auto resolved = resolve_type(owner, source, *type, scopes);
          if (resolved) {
            context.types_[{source.get(), statement.id}] = resolved;
            for (const StatementId child_id : statement.children) {
              const Statement& child = source->syntax->Get(child_id);
              if (child.keyword == "default") validate_default(*resolved, child);
            }
          }
        }
        for (const StatementId child_id : statement.children) visit(source->syntax->Get(child_id), false);
        if (pushed) scopes.pop_back();
      };
      visit(source->syntax->Get(source->syntax->roots().front()), true);
    };
    analyze_unit(owner->module());
    for (const auto& included : owner->module()->includes) analyze_unit(included);
    for (const auto& [prefix, imported] : owner->module()->imports) {
      (void)prefix; analyze_module(semantics.FindModule(*imported));
    }
  };
  analyze_module(semantics.root());
  if (!valid) return std::nullopt;
  return context;
}

}  // namespace yang::semantic
