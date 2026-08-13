// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/syntax_tree.h"

#include <stdexcept>
#include <utility>

namespace yang {
StatementId SyntaxTree::Add(Statement statement) {
  if (statements_.size() >= kInvalidStatementId) throw std::length_error("too many statements");
  const auto id = static_cast<StatementId>(statements_.size());
  statement.id = id; statements_.push_back(std::move(statement)); return id;
}
Statement& SyntaxTree::Get(StatementId id) { return statements_.at(id); }
const Statement& SyntaxTree::Get(StatementId id) const { return statements_.at(id); }
}  // namespace yang
