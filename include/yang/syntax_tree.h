// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SYNTAX_TREE_H_
#define YANG_SYNTAX_TREE_H_

#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "yang/source_file.h"

namespace yang {
using StatementId = std::uint32_t;
inline constexpr StatementId kInvalidStatementId = std::numeric_limits<StatementId>::max();

/** One parsed YANG statement. */
struct Statement {
  StatementId id = kInvalidStatementId;
  std::string keyword;
  std::optional<std::string> argument;
  SourceRange range;
  std::vector<StatementId> children;
};

/** Arena-like syntax tree. IDs and Statement addresses remain stable as nodes are added. */
class SyntaxTree {
 public:
  explicit SyntaxTree(std::shared_ptr<const SourceFile> source) : source_(std::move(source)) {}
  [[nodiscard]] StatementId Add(Statement statement);
  [[nodiscard]] Statement& Get(StatementId id);
  [[nodiscard]] const Statement& Get(StatementId id) const;
  [[nodiscard]] const std::vector<StatementId>& roots() const noexcept { return roots_; }
  void AddRoot(StatementId id) { roots_.push_back(id); }
  [[nodiscard]] std::size_t size() const noexcept { return statements_.size(); }
  [[nodiscard]] const std::shared_ptr<const SourceFile>& source() const noexcept { return source_; }

 private:
  std::shared_ptr<const SourceFile> source_;
  std::deque<Statement> statements_;
  std::vector<StatementId> roots_;
};
}  // namespace yang
#endif  // YANG_SYNTAX_TREE_H_
