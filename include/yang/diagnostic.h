// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_DIAGNOSTIC_H_
#define YANG_DIAGNOSTIC_H_

#include <string>
#include <utility>
#include <vector>

#include "yang/source_location.h"

namespace yang {

class SourceFile;

/** Stable categories for source and syntax diagnostics. */
enum class DiagnosticCode {
  kInvalidUtf8,
  kIllegalUnicodeCharacter,
  kInvalidLineEnding,
  kUnterminatedBlockComment,
  kUnterminatedQuotedString,
  kInvalidEscapeSequence,
  kUnexpectedToken,
  kInvalidKeyword,
  kInvalidStringConcatenation,
  kMissingArgument,
  kMissingTerminator,
  kMissingRightBrace,
  kUnknownStatement,
  kInvalidRootStatement,
  kInvalidSubstatement,
  kUnexpectedArgument,
  kMissingRequiredStatement,
  kDuplicateStatement,
  kInvalidStatementOrder,
  kUnsupportedYangVersion,
  kStatementRequiresYang11,
  kUnknownPrefix,
  kModuleNotFound,
  kModuleNameMismatch,
  kRevisionMismatch,
  kBelongsToMismatch,
  kDuplicatePrefix,
  kDependencyCycle,
  kDuplicateSymbol,
  kUnknownSymbol,
  kInvalidQualifiedName,
  kTypeCycle,
  kInvalidTypeRestriction,
  kInvalidDefaultValue,
  kDuplicateSchemaNode,
  kGroupingCycle,
  kInvalidRefineTarget,
  kInvalidSchemaPath,
  kUnknownSchemaNode,
  kInvalidAugmentTarget,
  kInvalidKey,
  kInvalidUnique,
  kMandatoryAugmentRequiresWhen,
  kInvalidFeatureExpression,
  kFeatureCycle,
  kIdentityCycle,
  kInvalidLeafrefPath,
  kInvalidLeafrefTarget,
  kLeafrefCycle,
  kInvalidXPath,
  kUnknownXPathFunction,
  kInvalidXPathFunctionArity,
  kInvalidXPathType,
  kUnknownXPathNode,
  kInvalidDeviationTarget,
  kInvalidDeviate,
  kInvalidExtensionInstance,
  kInvalidChoiceDefault,
  kInvalidOperationPlacement,
  kInvalidElementBounds,
  kInvalidYinJson,
  kInvalidYinInput,
  kResourceLimitExceeded,
};

enum class DiagnosticSeverity { kError, kWarning };

/** A secondary source location explaining a diagnostic relationship. */
struct RelatedDiagnostic {
  std::string message;
  SourceRange range;
  std::string source_name;
};

/** One parser diagnostic with an exact source range. */
struct Diagnostic {
  Diagnostic(DiagnosticCode diagnostic_code,
             DiagnosticSeverity diagnostic_severity,
             std::string diagnostic_message, SourceRange diagnostic_range,
             std::string diagnostic_source_name = {},
             std::vector<RelatedDiagnostic> diagnostic_related = {})
      : code(diagnostic_code),
        severity(diagnostic_severity),
        message(std::move(diagnostic_message)),
        range(diagnostic_range),
        source_name(std::move(diagnostic_source_name)),
        related(std::move(diagnostic_related)) {}

  DiagnosticCode code;
  DiagnosticSeverity severity = DiagnosticSeverity::kError;
  std::string message;
  SourceRange range;
  std::string source_name;
  std::vector<RelatedDiagnostic> related;
};

/** Returns a stable symbolic name suitable for logs and machine tooling. */
[[nodiscard]] std::string_view DiagnosticCodeName(DiagnosticCode code) noexcept;

/** Formats a diagnostic, optionally including its source line and caret. */
[[nodiscard]] std::string FormatDiagnostic(const Diagnostic& diagnostic,
                                           const SourceFile* source = nullptr);

/** Destination for diagnostics. */
class DiagnosticSink {
 public:
  virtual ~DiagnosticSink() = default;
  virtual void Report(Diagnostic diagnostic) = 0;
};

/** Simple collecting sink useful to clients and tests. */
class VectorDiagnosticSink final : public DiagnosticSink {
 public:
  void Report(Diagnostic diagnostic) override;
  [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept;
  [[nodiscard]] bool has_errors() const noexcept;

 private:
  std::vector<Diagnostic> diagnostics_;
};

}  // namespace yang
#endif  // YANG_DIAGNOSTIC_H_
