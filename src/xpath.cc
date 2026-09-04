// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/xpath.h"

#include "yang/resource_limits.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

namespace yang::semantic {
namespace {

std::optional<std::string> ChildArgument(const ResolvedModule& source,
                                         const Statement& parent,
                                         std::string_view keyword) {
  for (StatementId child_id : parent.children) {
    const Statement& child = source.syntax->Get(child_id);
    if (child.keyword == keyword) return child.argument;
  }
  return std::nullopt;
}

enum class TokenKind {
  kName, kString, kNumber, kSlash, kDoubleSlash, kDot, kDotDot, kAt,
  kLeftParen, kRightParen, kLeftBracket, kRightBracket, kComma, kPipe,
  kPlus, kMinus, kStar, kEqual, kNotEqual, kLess, kLessEqual, kGreater,
  kGreaterEqual, kEnd, kInvalid,
};

struct Token { TokenKind kind; std::string text; };

bool ValidQualifiedName(std::string_view name) {
  if (name.empty()) return false;
  const std::size_t colon = name.find(':');
  return colon == std::string_view::npos ||
         (colon > 0 && colon + 1 < name.size() &&
          name.find(':', colon + 1) == std::string_view::npos);
}

std::vector<Token> Lex(std::string_view input) {
  std::vector<Token> tokens;
  while (!input.empty()) {
    if (std::isspace(static_cast<unsigned char>(input.front()))) { input.remove_prefix(1); continue; }
    const char c = input.front();
    if (c == '\'' || c == '"') {
      const std::size_t close = input.find(c, 1);
      if (close == std::string_view::npos) { tokens.push_back({TokenKind::kInvalid, {}}); break; }
      tokens.push_back({TokenKind::kString, std::string(input.substr(1, close - 1))});
      input.remove_prefix(close + 1); continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      std::size_t size = 1;
      while (size < input.size() && (std::isdigit(static_cast<unsigned char>(input[size])) || input[size] == '.')) ++size;
      tokens.push_back({TokenKind::kNumber, std::string(input.substr(0, size))}); input.remove_prefix(size); continue;
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      std::size_t size = 1;
      while (size < input.size()) {
        const unsigned char value = static_cast<unsigned char>(input[size]);
        if (std::isalnum(value) == 0 && value != '_' && value != '-' && value != '.' && value != ':') break;
        ++size;
      }
      tokens.push_back({TokenKind::kName, std::string(input.substr(0, size))}); input.remove_prefix(size); continue;
    }
    auto emit = [&](TokenKind kind, std::size_t size = 1) {
      tokens.push_back({kind, std::string(input.substr(0, size))}); input.remove_prefix(size);
    };
    if (input.starts_with("//")) emit(TokenKind::kDoubleSlash, 2);
    else if (input.starts_with("..")) emit(TokenKind::kDotDot, 2);
    else if (input.starts_with("!=")) emit(TokenKind::kNotEqual, 2);
    else if (input.starts_with("<=")) emit(TokenKind::kLessEqual, 2);
    else if (input.starts_with(">=")) emit(TokenKind::kGreaterEqual, 2);
    else {
      switch (c) {
        case '/': emit(TokenKind::kSlash); break; case '.': emit(TokenKind::kDot); break;
        case '@': emit(TokenKind::kAt); break; case '(': emit(TokenKind::kLeftParen); break;
        case ')': emit(TokenKind::kRightParen); break; case '[': emit(TokenKind::kLeftBracket); break;
        case ']': emit(TokenKind::kRightBracket); break; case ',': emit(TokenKind::kComma); break;
        case '|': emit(TokenKind::kPipe); break; case '+': emit(TokenKind::kPlus); break;
        case '-': emit(TokenKind::kMinus); break; case '*': emit(TokenKind::kStar); break;
        case '=': emit(TokenKind::kEqual); break; case '<': emit(TokenKind::kLess); break;
        case '>': emit(TokenKind::kGreater); break; default: emit(TokenKind::kInvalid); break;
      }
    }
  }
  tokens.push_back({TokenKind::kEnd, {}});
  return tokens;
}

struct ParsedPath {
  bool absolute = false;
  bool from_current = false;
  bool statically_resolvable = true;
  bool schema_resolvable = true;
  std::optional<std::size_t> predicate_context;
  std::size_t predicate_context_step = 0;
  std::vector<std::string> steps;
};

class Parser {
 public:
  explicit Parser(std::string_view input) : tokens_(Lex(input)) {}
  bool Parse() { return ParseOr() && Current().kind == TokenKind::kEnd; }
  const std::vector<ParsedPath>& paths() const { return paths_; }
  const std::optional<std::string>& unknown_function() const { return unknown_function_; }
  const std::optional<std::string>& invalid_arity() const {
    return invalid_arity_;
  }
  const std::optional<std::string>& invalid_type() const {
    return invalid_type_;
  }
  XPathValueType inferred_type() const { return last_type_; }

 private:
  const Token& Current() const { return tokens_[position_]; }
  const Token& Peek() const { return tokens_[std::min(position_ + 1, tokens_.size() - 1)]; }
  bool Consume(TokenKind kind) { if (Current().kind != kind) return false; ++position_; return true; }
  bool Keyword(std::string_view value) const { return Current().kind == TokenKind::kName && Current().text == value; }
  bool ParseOr() { if (!ParseAnd()) return false; bool used = false; while (Keyword("or")) { used = true; ++position_; if (!ParseAnd()) return false; } if (used) last_type_ = XPathValueType::kBoolean; return true; }
  bool ParseAnd() { if (!ParseEquality()) return false; bool used = false; while (Keyword("and")) { used = true; ++position_; if (!ParseEquality()) return false; } if (used) last_type_ = XPathValueType::kBoolean; return true; }
  bool ParseEquality() { if (!ParseRelational()) return false; bool used = false; while (Current().kind == TokenKind::kEqual || Current().kind == TokenKind::kNotEqual) { used = true; ++position_; if (!ParseRelational()) return false; } if (used) last_type_ = XPathValueType::kBoolean; return true; }
  bool ParseRelational() { if (!ParseAdditive()) return false; bool used = false; while (Current().kind == TokenKind::kLess || Current().kind == TokenKind::kLessEqual || Current().kind == TokenKind::kGreater || Current().kind == TokenKind::kGreaterEqual) { used = true; ++position_; if (!ParseAdditive()) return false; } if (used) last_type_ = XPathValueType::kBoolean; return true; }
  bool ParseAdditive() { if (!ParseMultiplicative()) return false; bool used = false; while (Current().kind == TokenKind::kPlus || Current().kind == TokenKind::kMinus) { used = true; ++position_; if (!ParseMultiplicative()) return false; } if (used) last_type_ = XPathValueType::kNumber; return true; }
  bool ParseMultiplicative() {
    if (!ParseUnary()) return false;
    while (Current().kind == TokenKind::kStar || Keyword("div") || Keyword("mod")) {
      ++position_; if (!ParseUnary()) return false; last_type_ = XPathValueType::kNumber;
    }
    return true;
  }
  bool ParseUnary() { if (Consume(TokenKind::kMinus)) { if (!ParseUnary()) return false; last_type_ = XPathValueType::kNumber; return true; } return ParseUnion(); }
  bool ParseUnion() { if (!ParsePrimary()) return false; while (Consume(TokenKind::kPipe)) { if (last_type_ != XPathValueType::kNodeSet) invalid_type_ = "union"; if (!ParsePrimary()) return false; if (last_type_ != XPathValueType::kNodeSet) invalid_type_ = "union"; last_type_ = XPathValueType::kNodeSet; } return true; }
  bool ParsePrimary() {
    if (Consume(TokenKind::kString)) { last_type_ = XPathValueType::kString; return true; }
    if (Consume(TokenKind::kNumber)) { last_type_ = XPathValueType::kNumber; return true; }
    if (Consume(TokenKind::kLeftParen)) { if (!ParseOr() || !Consume(TokenKind::kRightParen)) return false; return true; }
    if (Current().kind == TokenKind::kName && Peek().kind == TokenKind::kLeftParen) {
      const std::string function = Current().text;
      if (!ValidQualifiedName(function)) return false;
      position_ += 2;
      static const std::unordered_map<std::string_view,
                                      std::pair<std::size_t, std::size_t>>
          arities{{"last", {0, 0}}, {"position", {0, 0}}, {"count", {1, 1}},
                  {"local-name", {0, 1}}, {"namespace-uri", {0, 1}},
                  {"name", {0, 1}}, {"string", {0, 1}},
                  {"concat", {2, std::numeric_limits<std::size_t>::max()}},
                  {"starts-with", {2, 2}},
                  {"contains", {2, 2}}, {"substring-before", {2, 2}},
                  {"substring-after", {2, 2}}, {"substring", {2, 3}},
                  {"string-length", {0, 1}}, {"normalize-space", {0, 1}},
                  {"translate", {3, 3}}, {"boolean", {1, 1}},
                  {"not", {1, 1}}, {"true", {0, 0}}, {"false", {0, 0}},
                  {"lang", {1, 1}}, {"number", {0, 1}}, {"sum", {1, 1}},
                  {"floor", {1, 1}}, {"ceiling", {1, 1}}, {"round", {1, 1}},
                  {"current", {0, 0}}, {"re-match", {2, 2}},
                  {"derived-from", {2, 2}}, {"derived-from-or-self", {2, 2}},
                  {"enum-value", {1, 1}}, {"bit-is-set", {2, 2}},
                  {"deref", {1, 1}}};
      const auto arity = arities.find(function);
      if (arity == arities.end() && function.find(':') == std::string::npos) {
        unknown_function_ = function;
      }
      std::size_t argument_count = 0;
      std::vector<XPathValueType> argument_types;
      if (!Consume(TokenKind::kRightParen)) {
        do {
          if (!ParseOr()) return false;
          argument_types.push_back(last_type_);
          ++argument_count;
        } while (Consume(TokenKind::kComma));
        if (!Consume(TokenKind::kRightParen)) return false;
      }
      if (arity != arities.end() &&
          (argument_count < arity->second.first ||
           argument_count > arity->second.second)) {
        invalid_arity_ = function;
      }
      const auto require_nodes = [&](std::size_t argument) {
        if (argument < argument_types.size() &&
            argument_types[argument] != XPathValueType::kNodeSet) {
          invalid_type_ = function;
        }
      };
      if (function == "count" || function == "sum" || function == "deref" ||
          function == "enum-value") require_nodes(0);
      if (function == "derived-from" ||
          function == "derived-from-or-self" || function == "bit-is-set") {
        require_nodes(0);
      }
      static const std::unordered_map<std::string_view, XPathValueType> returns{
          {"last", XPathValueType::kNumber}, {"position", XPathValueType::kNumber},
          {"count", XPathValueType::kNumber}, {"string", XPathValueType::kString},
          {"concat", XPathValueType::kString}, {"starts-with", XPathValueType::kBoolean},
          {"contains", XPathValueType::kBoolean}, {"substring-before", XPathValueType::kString},
          {"substring-after", XPathValueType::kString}, {"substring", XPathValueType::kString},
          {"string-length", XPathValueType::kNumber}, {"normalize-space", XPathValueType::kString},
          {"translate", XPathValueType::kString}, {"boolean", XPathValueType::kBoolean},
          {"not", XPathValueType::kBoolean}, {"true", XPathValueType::kBoolean},
          {"false", XPathValueType::kBoolean}, {"lang", XPathValueType::kBoolean},
          {"number", XPathValueType::kNumber}, {"sum", XPathValueType::kNumber},
          {"floor", XPathValueType::kNumber}, {"ceiling", XPathValueType::kNumber},
          {"round", XPathValueType::kNumber}, {"current", XPathValueType::kNodeSet},
          {"re-match", XPathValueType::kBoolean}, {"derived-from", XPathValueType::kBoolean},
          {"derived-from-or-self", XPathValueType::kBoolean},
          {"enum-value", XPathValueType::kNumber}, {"bit-is-set", XPathValueType::kBoolean},
          {"deref", XPathValueType::kNodeSet}, {"local-name", XPathValueType::kString},
          {"namespace-uri", XPathValueType::kString}, {"name", XPathValueType::kString}};
      last_type_ = returns.contains(function) ? returns.at(function)
                                              : XPathValueType::kUnknown;
      if (function == "current" && (Current().kind == TokenKind::kSlash || Current().kind == TokenKind::kDoubleSlash)) {
        return ParsePath(false, true);
      }
      return true;
    }
    if (Current().kind == TokenKind::kSlash || Current().kind == TokenKind::kDoubleSlash) return ParsePath(true, false);
    if (Current().kind == TokenKind::kName || Current().kind == TokenKind::kDot ||
        Current().kind == TokenKind::kDotDot || Current().kind == TokenKind::kStar ||
        Current().kind == TokenKind::kAt) return ParsePath(false, false);
    return false;
  }
  bool ParsePath(bool absolute, bool from_current) {
    ParsedPath path{.absolute = absolute, .from_current = from_current};
    if (!absolute && !from_current && !predicate_contexts_.empty()) {
      path.predicate_context = predicate_contexts_.back().first;
      path.predicate_context_step = predicate_contexts_.back().second;
    }
    if (absolute || from_current) {
      if (Consume(TokenKind::kDoubleSlash)) {
        path.statically_resolvable = false;
        path.schema_resolvable = false;
      }
      else if (!Consume(TokenKind::kSlash)) return false;
    }
    const std::size_t path_index = paths_.size();
    paths_.push_back(std::move(path));
    for (;;) {
      if (Consume(TokenKind::kAt)) {
        paths_[path_index].statically_resolvable = false;
        paths_[path_index].schema_resolvable = false;
      }
      if (Current().kind == TokenKind::kName || Current().kind == TokenKind::kDot ||
          Current().kind == TokenKind::kDotDot || Current().kind == TokenKind::kStar) {
        if (Current().kind == TokenKind::kName &&
            !ValidQualifiedName(Current().text)) return false;
        paths_[path_index].steps.push_back(Current().text);
        if (Current().kind == TokenKind::kStar) {
          paths_[path_index].statically_resolvable = false;
          paths_[path_index].schema_resolvable = false;
        }
        ++position_;
      } else return false;
      while (Consume(TokenKind::kLeftBracket)) {
        paths_[path_index].statically_resolvable = false;
        predicate_contexts_.push_back(
            {path_index, paths_[path_index].steps.size() - 1});
        const bool parsed = ParseOr() && Consume(TokenKind::kRightBracket);
        predicate_contexts_.pop_back();
        if (!parsed) return false;
      }
      if (Consume(TokenKind::kDoubleSlash)) {
        paths_[path_index].statically_resolvable = false;
        paths_[path_index].schema_resolvable = false;
        continue;
      }
      if (!Consume(TokenKind::kSlash)) break;
    }
    last_type_ = XPathValueType::kNodeSet;
    return true;
  }
  std::vector<Token> tokens_;
  std::size_t position_ = 0;
  std::vector<ParsedPath> paths_;
  std::vector<std::pair<std::size_t, std::size_t>> predicate_contexts_;
  std::optional<std::string> unknown_function_;
  std::optional<std::string> invalid_arity_;
  std::optional<std::string> invalid_type_;
  XPathValueType last_type_ = XPathValueType::kUnknown;
};

std::optional<std::string> ModuleFor(const ResolvedModule& source,
                                     std::string_view step,
                                     std::string_view default_module) {
  const std::size_t colon = step.find(':');
  if (colon == std::string_view::npos) return std::string(default_module);
  const std::string prefix(step.substr(0, colon));
  if (prefix == source.prefix) return source.belongs_to.value_or(source.name);
  const auto imported = source.imports.find(prefix);
  return imported == source.imports.end() ? std::nullopt
                                          : std::optional<std::string>(imported->second->name);
}

std::string_view LocalName(std::string_view step) {
  const std::size_t colon = step.find(':');
  return colon == std::string_view::npos ? step : step.substr(colon + 1);
}

bool IsSchemaOnlyNode(const SchemaNode& node) {
  return node.kind == SchemaNodeKind::kChoice ||
         node.kind == SchemaNodeKind::kCase;
}

std::optional<SchemaNodeId> DataParent(const SchemaTree& tree,
                                       SchemaNodeId node) {
  std::optional<SchemaNodeId> parent = tree.Get(node).parent;
  while (parent && IsSchemaOnlyNode(tree.Get(*parent)))
    parent = tree.Get(*parent).parent;
  return parent;
}

std::optional<SchemaNodeId> FindStep(const SchemaTree& tree,
                                     std::optional<SchemaNodeId> parent,
                                     const SchemaName& name) {
  if (const auto direct = tree.FindChild(parent, name)) return direct;
  const auto& candidates = parent ? tree.Get(*parent).children : tree.roots();
  for (const SchemaNodeId id : candidates) {
    if (!IsSchemaOnlyNode(tree.Get(id))) continue;
    if (const auto nested = FindStep(tree, id, name)) return nested;
  }
  return std::nullopt;
}

const ResolvedModule* ModuleNamed(const SchemaContext& schemas,
                                  std::string_view name) {
  for (const auto* module : schemas.modules()) if (module->name == name) return module;
  return nullptr;
}

bool HasFeatureGatedChildNamed(const SchemaTree& tree, SchemaNodeId parent,
                              std::string_view local_name) {
  const SchemaNode& parent_node = tree.Get(parent);
  const ResolvedModule& source = *parent_node.source_module;
  if (!source.syntax || parent_node.declaration == kInvalidStatementId)
    return false;
  const Statement& parent_statement =
      source.syntax->Get(parent_node.declaration);
  for (const StatementId child_id : parent_statement.children) {
    const Statement& statement = source.syntax->Get(child_id);
    if (!statement.argument || *statement.argument != local_name) continue;
    if (statement.keyword != "container" && statement.keyword != "list" &&
        statement.keyword != "leaf" && statement.keyword != "leaf-list" &&
        statement.keyword != "choice" && statement.keyword != "case" &&
        statement.keyword != "anydata" && statement.keyword != "anyxml")
      continue;
    for (const StatementId property_id : statement.children)
      if (source.syntax->Get(property_id).keyword == "if-feature") return true;
  }
  return false;
}

}  // namespace

std::optional<XPathContext> XPathValidator::Validate(
    const SchemaContext& schemas) {
  XPathContext result;
  bool valid = true;
  for (const ResolvedModule* tree_module : schemas.modules()) {
    const SchemaTree* tree = schemas.Find(*tree_module);
    if (!tree) continue;
    for (std::size_t index = 0; index < tree->size(); ++index) {
      const SchemaNodeId id = static_cast<SchemaNodeId>(index);
      const SchemaNode& node = tree->Get(id);
      if (!node.supported ||
          node.origin == SchemaNodeOrigin::kImplicitCase || !node.source_module ||
          node.declaration == kInvalidStatementId) continue;
      std::vector<SchemaConstraint> constraints = node.must_constraints;
      constraints.insert(constraints.end(), node.when_constraints.begin(),
                         node.when_constraints.end());
      for (const SchemaConstraint& constraint_ref : constraints) {
        const ResolvedModule& constraint_source = *constraint_ref.source_module;
        const SchemaNodeId constraint_context =
            constraint_ref.context_node.value_or(id);
        const Statement& constraint =
            constraint_source.syntax->Get(constraint_ref.statement);
        if (!constraint.argument) continue;
        std::string resource_error;
        if (!XPathWithinResourceLimits(*constraint.argument,
                                       DefaultResourceLimits(),
                                       &resource_error)) {
          diagnostics_.Report({DiagnosticCode::kResourceLimitExceeded,
                               DiagnosticSeverity::kError, resource_error,
                               constraint.range});
          valid = false;
          continue;
        }
        Parser parser(*constraint.argument);
        if (!parser.Parse()) {
          diagnostics_.Report({DiagnosticCode::kInvalidXPath, DiagnosticSeverity::kError,
                               fmt::format("invalid XPath expression '{}'", *constraint.argument),
                               constraint.range});
          valid = false; continue;
        }
        if (parser.unknown_function()) {
          diagnostics_.Report({DiagnosticCode::kUnknownXPathFunction,
                               DiagnosticSeverity::kError,
                               fmt::format("unknown XPath function '{}'", *parser.unknown_function()),
                               constraint.range});
          valid = false; continue;
        }
        if (parser.invalid_arity()) {
          diagnostics_.Report({
              DiagnosticCode::kInvalidXPathFunctionArity,
              DiagnosticSeverity::kError,
              fmt::format("invalid argument count for XPath function '{}'",
                          *parser.invalid_arity()),
              constraint.range});
          valid = false;
          continue;
        }
        if (parser.invalid_type()) {
          diagnostics_.Report({
              DiagnosticCode::kInvalidXPathType,
              DiagnosticSeverity::kError,
              fmt::format("invalid static argument type for XPath operation '{}'",
                          *parser.invalid_type()),
              constraint.range});
          valid = false;
          continue;
        }
        ValidatedXPath validated{
            .kind = constraint.keyword == "must" ? XPathConstraintKind::kMust
                                                   : XPathConstraintKind::kWhen,
            .constrained_node = {tree_module, id},
            .context = {tree_module, constraint_context},
            .source_module = &constraint_source,
            .statement = constraint.id,
            .expression = *constraint.argument,
            .error_message = ChildArgument(constraint_source, constraint,
                                            "error-message"),
            .error_app_tag = ChildArgument(constraint_source, constraint,
                                           "error-app-tag"),
            .inferred_type = parser.inferred_type()};
        std::vector<std::optional<SchemaNodeRef>> path_contexts(
            parser.paths().size());
        std::vector<std::vector<SchemaNodeRef>> path_step_contexts(
            parser.paths().size());
        for (std::size_t path_index = 0;
             path_index < parser.paths().size(); ++path_index) {
          const ParsedPath& path = parser.paths()[path_index];
          if (!path.schema_resolvable || path.steps.empty()) continue;
          const SchemaTree* active_tree = tree;
          const ResolvedModule* active_module = tree_module;
          std::optional<SchemaNodeId> current = path.absolute ? std::nullopt
              : std::optional<SchemaNodeId>(constraint_context);
          if (path.predicate_context) {
            const auto& predicate_steps =
                path_step_contexts[*path.predicate_context];
            if (path.predicate_context_step >= predicate_steps.size()) continue;
            const SchemaNodeRef& predicate_base =
                predicate_steps[path.predicate_context_step];
            active_module = predicate_base.tree_module;
            active_tree = schemas.Find(*active_module);
            current = predicate_base.node;
          }
          if (path.absolute) {
            const auto module_name =
                ModuleFor(constraint_source, path.steps.front(),
                          active_module->name);
            active_module = module_name ? ModuleNamed(schemas, *module_name) : nullptr;
            active_tree = active_module ? schemas.Find(*active_module) : nullptr;
          }
          bool resolved = active_tree != nullptr;
          bool feature_pruned = false;
          for (const std::string& step : path.steps) {
            if (!resolved) break;
            if (step == ".") {
              if (current) {
                path_step_contexts[path_index].push_back(
                    {active_module, *current});
              }
              continue;
            }
            if (step == "..") {
              if (!current) { resolved = false; break; }
              current = DataParent(*active_tree, *current);
              if (!current) { resolved = false; break; }
              path_step_contexts[path_index].push_back(
                  {active_module, *current});
              continue;
            }
            const auto module_name =
                ModuleFor(constraint_source, step, active_module->name);
            if (!module_name) { resolved = false; break; }
            const std::optional<SchemaNodeId> parent = current;
            current = FindStep(*active_tree, parent,
                               {*module_name, std::string(LocalName(step))});
            if (!current) {
              feature_pruned = parent &&
                  HasFeatureGatedChildNamed(*active_tree, *parent,
                                            LocalName(step));
              resolved = false;
              break;
            }
            path_step_contexts[path_index].push_back(
                {active_module, *current});
          }
          if ((!resolved || !current) && !feature_pruned) {
            diagnostics_.Report({DiagnosticCode::kUnknownXPathNode,
                                 DiagnosticSeverity::kError,
                                 fmt::format("XPath path in '{}' cannot be resolved", *constraint.argument),
                                 constraint.range});
            valid = false;
          } else if (resolved && current) {
            path_contexts[path_index] = SchemaNodeRef{active_module, *current};
            if (path.statically_resolvable) {
              validated.statically_resolved_nodes.push_back(
                  {active_module, *current});
            }
          }
        }
        result.expressions_.push_back(std::move(validated));
      }
    }
  }
  if (!valid) return std::nullopt;
  return result;
}

}  // namespace yang::semantic
