// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/identity_feature.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <utility>

#include <fmt/format.h>

namespace yang::semantic {
namespace {

enum class ExpressionKind { kName, kNot, kAnd, kOr };

struct Expression {
  ExpressionKind kind = ExpressionKind::kName;
  std::string name;
  std::unique_ptr<Expression> left;
  std::unique_ptr<Expression> right;
};

struct Token {
  enum class Kind { kName, kNot, kAnd, kOr, kLeft, kRight, kEnd } kind;
  std::string_view text;
};

class ExpressionParser {
 public:
  explicit ExpressionParser(std::string_view input) : input_(input) { Advance(); }
  std::unique_ptr<Expression> Parse() {
    auto result = ParseOr();
    return result && current_.kind == Token::Kind::kEnd ? std::move(result) : nullptr;
  }

 private:
  void Advance() {
    while (!input_.empty() && std::isspace(static_cast<unsigned char>(input_.front()))) input_.remove_prefix(1);
    if (input_.empty()) { current_ = {Token::Kind::kEnd, {}}; return; }
    if (input_.front() == '(' || input_.front() == ')') {
      current_ = {input_.front() == '(' ? Token::Kind::kLeft : Token::Kind::kRight,
                  input_.substr(0, 1)};
      input_.remove_prefix(1); return;
    }
    std::size_t length = 0;
    while (length < input_.size() &&
           !std::isspace(static_cast<unsigned char>(input_[length])) &&
           input_[length] != '(' && input_[length] != ')') ++length;
    const std::string_view text = input_.substr(0, length);
    input_.remove_prefix(length);
    Token::Kind kind = Token::Kind::kName;
    if (text == "not") kind = Token::Kind::kNot;
    else if (text == "and") kind = Token::Kind::kAnd;
    else if (text == "or") kind = Token::Kind::kOr;
    current_ = {kind, text};
  }
  std::unique_ptr<Expression> ParseOr() {
    auto left = ParseAnd();
    while (left && current_.kind == Token::Kind::kOr) {
      Advance(); auto right = ParseAnd(); if (!right) return nullptr;
      left = std::make_unique<Expression>(ExpressionKind::kOr, "", std::move(left), std::move(right));
    }
    return left;
  }
  std::unique_ptr<Expression> ParseAnd() {
    auto left = ParseUnary();
    while (left && current_.kind == Token::Kind::kAnd) {
      Advance(); auto right = ParseUnary(); if (!right) return nullptr;
      left = std::make_unique<Expression>(ExpressionKind::kAnd, "", std::move(left), std::move(right));
    }
    return left;
  }
  std::unique_ptr<Expression> ParseUnary() {
    if (current_.kind == Token::Kind::kNot) {
      Advance(); auto operand = ParseUnary(); if (!operand) return nullptr;
      return std::make_unique<Expression>(ExpressionKind::kNot, "", std::move(operand), nullptr);
    }
    if (current_.kind == Token::Kind::kLeft) {
      Advance(); auto nested = ParseOr();
      if (!nested || current_.kind != Token::Kind::kRight) return nullptr;
      Advance(); return nested;
    }
    if (current_.kind != Token::Kind::kName || current_.text.empty()) return nullptr;
    auto result = std::make_unique<Expression>();
    result->name = std::string(current_.text); Advance(); return result;
  }
  std::string_view input_;
  Token current_{Token::Kind::kEnd, {}};
};

struct FeatureDeclaration {
  Symbol symbol;
  std::shared_ptr<const ModuleSymbols> owner;
  std::vector<std::unique_ptr<Expression>> conditions;
  std::vector<QualifiedSymbolName> references;
};

std::optional<QualifiedSymbolName> ResolveName(
    const SemanticContext& semantics, const std::shared_ptr<const ModuleSymbols>& owner,
    const std::shared_ptr<const ResolvedModule>& source, SymbolKind kind,
    std::string_view text) {
  std::string_view local = text;
  std::optional<std::string_view> prefix;
  const std::size_t colon = text.find(':');
  if (colon != std::string_view::npos) {
    if (colon == 0 || colon + 1 == text.size() || text.find(':', colon + 1) != std::string_view::npos) {
      return std::nullopt;
    }
    prefix = text.substr(0, colon); local = text.substr(colon + 1);
  }
  auto target = owner;
  if (prefix && *prefix != source->prefix && *prefix != owner->module()->prefix) {
    const auto imported = source->imports.find(std::string(*prefix));
    if (imported == source->imports.end()) return std::nullopt;
    target = semantics.FindModule(*imported->second);
  }
  if (!target || !target->Find(kind, local)) return std::nullopt;
  return QualifiedSymbolName{target->module()->name, std::string(local)};
}

}  // namespace

std::size_t QualifiedSymbolNameHash::operator()(
    const QualifiedSymbolName& value) const noexcept {
  return std::hash<std::string>{}(value.module) ^
         (std::hash<std::string>{}(value.name) << 1U);
}

bool FeatureSet::IsEnabled(std::string_view module,
                           std::string_view feature) const {
  const auto found = states_.find({std::string(module), std::string(feature)});
  return found != states_.end() && found->second;
}

std::optional<bool> FeatureSet::Evaluate(
    const ResolvedModule& source, std::string_view expression) const {
  ExpressionParser parser(expression);
  const auto parsed = parser.Parse();
  if (!parsed) return std::nullopt;
  std::function<std::optional<bool>(const Expression&)> evaluate;
  evaluate = [&](const Expression& node) -> std::optional<bool> {
    if (node.kind == ExpressionKind::kName) {
      std::string_view name = node.name;
      std::string module = source.belongs_to.value_or(source.name);
      const std::size_t colon = name.find(':');
      if (colon != std::string_view::npos) {
        if (colon == 0 || colon + 1 == name.size() ||
            name.find(':', colon + 1) != std::string_view::npos) {
          return std::nullopt;
        }
        const std::string prefix(name.substr(0, colon));
        name.remove_prefix(colon + 1);
        if (prefix != source.prefix) {
          const auto imported = source.imports.find(prefix);
          if (imported == source.imports.end()) return std::nullopt;
          module = imported->second->name;
        }
      }
      return IsEnabled(module, name);
    }
    const auto left = node.left ? evaluate(*node.left) : std::nullopt;
    if (!left) return std::nullopt;
    if (node.kind == ExpressionKind::kNot) return !*left;
    const auto right = node.right ? evaluate(*node.right) : std::nullopt;
    if (!right) return std::nullopt;
    return node.kind == ExpressionKind::kAnd ? *left && *right
                                             : *left || *right;
  };
  return evaluate(*parsed);
}

bool IdentityGraph::Contains(const QualifiedSymbolName& identity) const {
  return bases_.contains(identity);
}

bool IdentityGraph::IsDerivedFrom(const QualifiedSymbolName& identity,
                                  const QualifiedSymbolName& base) const {
  if (identity == base) return true;
  std::unordered_set<QualifiedSymbolName, QualifiedSymbolNameHash> visited;
  std::function<bool(const QualifiedSymbolName&)> visit;
  visit = [&](const QualifiedSymbolName& current) {
    if (!visited.insert(current).second) return false;
    const auto found = bases_.find(current);
    if (found == bases_.end()) return false;
    for (const auto& direct : found->second) {
      if (direct == base || visit(direct)) return true;
    }
    return false;
  };
  return visit(identity);
}

std::vector<QualifiedSymbolName> IdentityGraph::DerivedFrom(
    const QualifiedSymbolName& base) const {
  std::vector<QualifiedSymbolName> result;
  for (const auto& [identity, ignored] : bases_) {
    (void)ignored;
    if (identity != base && IsDerivedFrom(identity, base)) result.push_back(identity);
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return std::tie(left.module, left.name) < std::tie(right.module, right.name);
  });
  return result;
}

std::optional<IdentityFeatureContext> IdentityFeatureResolver::Resolve(
    const SemanticContext& semantics,
    const std::vector<QualifiedSymbolName>& requested_features) {
  if (!semantics.root()) return std::nullopt;
  IdentityFeatureContext result;
  bool valid = true;
  std::vector<std::shared_ptr<const ModuleSymbols>> modules;
  std::unordered_set<const ResolvedModule*> visited_modules;
  std::function<void(std::shared_ptr<const ModuleSymbols>)> collect;
  collect = [&](std::shared_ptr<const ModuleSymbols> module) {
    if (!module || !visited_modules.insert(module->module().get()).second) return;
    modules.push_back(module);
    for (const auto& [prefix, imported] : module->module()->imports) {
      (void)prefix; collect(semantics.FindModule(*imported));
    }
    for (const auto& included : module->module()->includes) {
      for (const auto& [prefix, imported] : included->imports) {
        (void)prefix; collect(semantics.FindModule(*imported));
      }
    }
  };
  collect(semantics.root());

  std::unordered_map<QualifiedSymbolName, FeatureDeclaration,
                     QualifiedSymbolNameHash> features;
  for (const auto& owner : modules) {
    for (const auto& [name, symbol] : owner->symbols(SymbolKind::kFeature)) {
      const QualifiedSymbolName key{owner->module()->name, name};
      features.emplace(key, FeatureDeclaration{symbol, owner, {}, {}});
      result.features.states_[key] = false;
    }
    for (const auto& [name, symbol] : owner->symbols(SymbolKind::kIdentity)) {
      result.identities.bases_.try_emplace({owner->module()->name, name});
      const Statement& declaration = symbol.source_module->syntax->Get(symbol.statement);
      for (const StatementId child_id : declaration.children) {
        const Statement& child = symbol.source_module->syntax->Get(child_id);
        if (child.keyword != "base" || !child.argument) continue;
        const auto base = ResolveName(semantics, owner, symbol.source_module,
                                      SymbolKind::kIdentity, *child.argument);
        if (base) result.identities.bases_[{owner->module()->name, name}].push_back(*base);
      }
    }
  }

  std::function<void(Expression&, const FeatureDeclaration&, SourceRange)> resolve_expression;
  resolve_expression = [&](Expression& expression, const FeatureDeclaration& declaration,
                           SourceRange range) {
    if (expression.kind == ExpressionKind::kName) {
      const auto feature = ResolveName(semantics, declaration.owner,
                                       declaration.symbol.source_module,
                                       SymbolKind::kFeature, expression.name);
      if (!feature) {
        diagnostics_.Report({DiagnosticCode::kUnknownSymbol, DiagnosticSeverity::kError,
                             fmt::format("unknown feature '{}'", expression.name), range});
        valid = false;
      } else {
        expression.name = feature->module + ":" + feature->name;
      }
      return;
    }
    if (expression.left) resolve_expression(*expression.left, declaration, range);
    if (expression.right) resolve_expression(*expression.right, declaration, range);
  };

  for (auto& [key, declaration] : features) {
    (void)key;
    const Statement& statement = declaration.symbol.source_module->syntax->Get(
        declaration.symbol.statement);
    for (const StatementId child_id : statement.children) {
      const Statement& child = declaration.symbol.source_module->syntax->Get(child_id);
      if (child.keyword != "if-feature" || !child.argument) continue;
      ExpressionParser parser(*child.argument);
      auto expression = parser.Parse();
      if (!expression) {
        diagnostics_.Report({DiagnosticCode::kInvalidFeatureExpression,
                             DiagnosticSeverity::kError,
                             fmt::format("invalid if-feature expression '{}'", *child.argument),
                             child.range});
        valid = false;
        continue;
      }
      resolve_expression(*expression, declaration, child.range);
      declaration.conditions.push_back(std::move(expression));
    }
  }

  const auto expression_names = [](const Expression& expression) {
    std::vector<QualifiedSymbolName> names;
    std::function<void(const Expression&)> visit = [&](const Expression& node) {
      if (node.kind == ExpressionKind::kName) {
        const std::size_t colon = node.name.find(':');
        if (colon != std::string::npos) names.push_back({node.name.substr(0, colon), node.name.substr(colon + 1)});
      } else {
        if (node.left) visit(*node.left); if (node.right) visit(*node.right);
      }
    };
    visit(expression); return names;
  };
  for (auto& [key, declaration] : features) {
    (void)key;
    for (const auto& expression : declaration.conditions) {
      auto names = expression_names(*expression);
      declaration.references.insert(declaration.references.end(), names.begin(), names.end());
    }
  }

  enum class Mark { kVisiting, kDone };
  std::unordered_map<QualifiedSymbolName, Mark, QualifiedSymbolNameHash> feature_marks;
  std::function<void(const QualifiedSymbolName&)> check_feature_cycle;
  check_feature_cycle = [&](const QualifiedSymbolName& feature) {
    const auto existing = feature_marks.find(feature);
    if (existing != feature_marks.end() && existing->second == Mark::kDone) return;
    if (existing != feature_marks.end() && existing->second == Mark::kVisiting) {
      diagnostics_.Report({DiagnosticCode::kFeatureCycle, DiagnosticSeverity::kError,
                           fmt::format("feature dependency cycle at '{}:{}'", feature.module, feature.name), {}});
      valid = false; return;
    }
    feature_marks[feature] = Mark::kVisiting;
    const auto found = features.find(feature);
    if (found != features.end()) for (const auto& dependency : found->second.references) check_feature_cycle(dependency);
    feature_marks[feature] = Mark::kDone;
  };
  for (const auto& [feature, ignored] : features) { (void)ignored; check_feature_cycle(feature); }

  std::unordered_map<QualifiedSymbolName, Mark, QualifiedSymbolNameHash> identity_marks;
  std::function<void(const QualifiedSymbolName&)> check_identity_cycle;
  check_identity_cycle = [&](const QualifiedSymbolName& identity) {
    const auto existing = identity_marks.find(identity);
    if (existing != identity_marks.end() && existing->second == Mark::kDone) return;
    if (existing != identity_marks.end() && existing->second == Mark::kVisiting) {
      diagnostics_.Report({DiagnosticCode::kIdentityCycle, DiagnosticSeverity::kError,
                           fmt::format("identity inheritance cycle at '{}:{}'", identity.module, identity.name), {}});
      valid = false; return;
    }
    identity_marks[identity] = Mark::kVisiting;
    for (const auto& base : result.identities.bases_[identity]) check_identity_cycle(base);
    identity_marks[identity] = Mark::kDone;
  };
  for (const auto& [identity, ignored] : result.identities.bases_) { (void)ignored; check_identity_cycle(identity); }

  const std::unordered_set<QualifiedSymbolName, QualifiedSymbolNameHash> requested(
      requested_features.begin(), requested_features.end());
  std::unordered_map<QualifiedSymbolName, bool, QualifiedSymbolNameHash> evaluating;
  std::function<bool(const QualifiedSymbolName&)> enabled;
  std::function<bool(const Expression&)> evaluate_expression;
  evaluate_expression = [&](const Expression& expression) {
    if (expression.kind == ExpressionKind::kName) {
      const std::size_t colon = expression.name.find(':');
      return enabled({expression.name.substr(0, colon), expression.name.substr(colon + 1)});
    }
    if (expression.kind == ExpressionKind::kNot) return !evaluate_expression(*expression.left);
    if (expression.kind == ExpressionKind::kAnd) return evaluate_expression(*expression.left) && evaluate_expression(*expression.right);
    return evaluate_expression(*expression.left) || evaluate_expression(*expression.right);
  };
  enabled = [&](const QualifiedSymbolName& feature) {
    if (!requested.contains(feature)) return false;
    auto state = result.features.states_.find(feature);
    if (state == result.features.states_.end()) return false;
    if (state->second) return true;
    if (evaluating[feature]) return false;
    evaluating[feature] = true;
    const auto declaration = features.find(feature);
    bool value = declaration != features.end();
    if (value) for (const auto& condition : declaration->second.conditions) value = value && evaluate_expression(*condition);
    evaluating[feature] = false;
    state->second = value;
    return value;
  };
  for (const auto& feature : requested) {
    if (!features.contains(feature)) {
      diagnostics_.Report({DiagnosticCode::kUnknownSymbol, DiagnosticSeverity::kError,
                           fmt::format("requested feature '{}:{}' does not exist", feature.module, feature.name), {}});
      valid = false;
    } else {
      enabled(feature);
    }
  }
  if (!valid) return std::nullopt;
  return result;
}

}  // namespace yang::semantic
