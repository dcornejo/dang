// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_COMPILER_H_
#define YANG_COMPILER_H_

#include <memory>
#include <optional>
#include <vector>

#include "yang/extension.h"
#include "yang/identity_feature.h"
#include "yang/leafref.h"
#include "yang/schema_value.h"
#include "yang/xpath.h"

namespace yang {

/** Complete, validated semantic result for a root YANG module. */
struct Compilation {
  std::shared_ptr<const ResolvedModule> module;
  semantic::SemanticContext semantics;
  semantic::TypeContext types;
  semantic::IdentityFeatureContext identities;
  semantic::ExtensionContext extensions;
  semantic::SchemaContext schemas;
  semantic::LeafrefContext leafrefs;
  semantic::XPathContext xpath;
};

/** Runs the semantic passes in their required dependency order. */
class Compiler {
 public:
  Compiler(ModuleSourceRepository& repository, DiagnosticSink& diagnostics)
      : repository_(repository), diagnostics_(diagnostics) {}

  /** Resolves, validates, and builds the effective schema for source. */
  [[nodiscard]] std::optional<Compilation> Compile(
      std::shared_ptr<const SourceFile> source,
      const std::vector<semantic::QualifiedSymbolName>& requested_features = {});

 private:
  ModuleSourceRepository& repository_;
  DiagnosticSink& diagnostics_;
};

}  // namespace yang

#endif  // YANG_COMPILER_H_
