// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_DEVIATION_H_
#define YANG_DEVIATION_H_

#include "yang/schema_tree.h"

namespace yang::semantic {

/** Applies RFC 7950 deviation statements to an effective schema context. */
class DeviationApplier {
 public:
  explicit DeviationApplier(DiagnosticSink& diagnostics)
      : diagnostics_(diagnostics) {}

  /** Applies all deviations in the resolved dependency closure in place. */
  [[nodiscard]] bool Apply(SchemaContext& schemas, const TypeContext& types);

 private:
  DiagnosticSink& diagnostics_;
};

}  // namespace yang::semantic
#endif  // YANG_DEVIATION_H_
