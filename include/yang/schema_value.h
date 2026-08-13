// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_SCHEMA_VALUE_H_
#define YANG_SCHEMA_VALUE_H_

#include "yang/identity_feature.h"
#include "yang/leafref.h"

namespace yang::semantic {

/** Validates defaults whose value spaces depend on the effective schema. */
class SchemaValueValidator {
 public:
  explicit SchemaValueValidator(DiagnosticSink& diagnostics)
      : diagnostics_(diagnostics) {}

  /** Validates identityref and leafref defaults after schema resolution. */
  [[nodiscard]] bool Validate(const SchemaContext& schemas,
                              const IdentityGraph& identities,
                              const LeafrefContext& leafrefs);

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_SCHEMA_VALUE_H_
