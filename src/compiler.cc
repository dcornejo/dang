// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/compiler.h"

#include "yang/deviation.h"

namespace yang {

std::optional<Compilation> Compiler::Compile(
    std::shared_ptr<const SourceFile> source,
    const std::vector<semantic::QualifiedSymbolName>& requested_features) {
  ModuleResolver modules(repository_, diagnostics_);
  auto module = modules.Resolve(std::move(source));
  if (!module) return std::nullopt;

  semantic::SymbolResolver symbols(diagnostics_);
  auto semantics = symbols.Resolve(module);
  if (!semantics) return std::nullopt;
  semantic::TypeResolver type_resolver(diagnostics_);
  auto types = type_resolver.Resolve(*semantics);
  if (!types) return std::nullopt;
  semantic::IdentityFeatureResolver identity_resolver(diagnostics_);
  auto identities = identity_resolver.Resolve(*semantics, requested_features);
  if (!identities) return std::nullopt;
  semantic::ExtensionResolver extension_resolver(diagnostics_);
  auto extensions = extension_resolver.Resolve(*semantics);
  if (!extensions) return std::nullopt;
  semantic::SchemaContextBuilder schema_builder(diagnostics_);
  auto schemas = schema_builder.Build(*semantics, *types,
                                      &identities->features);
  if (!schemas) return std::nullopt;
  semantic::DeviationApplier deviations(diagnostics_);
  if (!deviations.Apply(*schemas, *types)) return std::nullopt;
  semantic::LeafrefResolver leafref_resolver(diagnostics_);
  auto leafrefs = leafref_resolver.Resolve(*schemas);
  if (!leafrefs) return std::nullopt;
  semantic::SchemaValueValidator values(diagnostics_);
  if (!values.Validate(*schemas, identities->identities, *leafrefs)) {
    return std::nullopt;
  }
  semantic::XPathValidator xpath_validator(diagnostics_);
  auto xpath = xpath_validator.Validate(*schemas);
  if (!xpath) return std::nullopt;

  return Compilation{.module = std::move(module),
                     .semantics = std::move(*semantics),
                     .types = std::move(*types),
                     .identities = std::move(*identities),
                     .extensions = std::move(*extensions),
                     .schemas = std::move(*schemas),
                     .leafrefs = std::move(*leafrefs),
                     .xpath = std::move(*xpath)};
}

}  // namespace yang
