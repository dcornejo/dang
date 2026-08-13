// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include "yang/diagnostic.h"

#include <algorithm>
#include <sstream>
#include <utility>

#include <fmt/format.h>

#include "yang/source_file.h"

namespace yang {

std::string_view DiagnosticCodeName(DiagnosticCode code) noexcept {
  switch (code) {
#define YANG_DIAGNOSTIC_NAME(name) case DiagnosticCode::name: return #name
    YANG_DIAGNOSTIC_NAME(kInvalidUtf8);
    YANG_DIAGNOSTIC_NAME(kIllegalUnicodeCharacter);
    YANG_DIAGNOSTIC_NAME(kInvalidLineEnding);
    YANG_DIAGNOSTIC_NAME(kUnterminatedBlockComment);
    YANG_DIAGNOSTIC_NAME(kUnterminatedQuotedString);
    YANG_DIAGNOSTIC_NAME(kInvalidEscapeSequence);
    YANG_DIAGNOSTIC_NAME(kUnexpectedToken);
    YANG_DIAGNOSTIC_NAME(kInvalidKeyword);
    YANG_DIAGNOSTIC_NAME(kInvalidStringConcatenation);
    YANG_DIAGNOSTIC_NAME(kMissingArgument);
    YANG_DIAGNOSTIC_NAME(kMissingTerminator);
    YANG_DIAGNOSTIC_NAME(kMissingRightBrace);
    YANG_DIAGNOSTIC_NAME(kUnknownStatement);
    YANG_DIAGNOSTIC_NAME(kInvalidRootStatement);
    YANG_DIAGNOSTIC_NAME(kInvalidSubstatement);
    YANG_DIAGNOSTIC_NAME(kUnexpectedArgument);
    YANG_DIAGNOSTIC_NAME(kMissingRequiredStatement);
    YANG_DIAGNOSTIC_NAME(kDuplicateStatement);
    YANG_DIAGNOSTIC_NAME(kInvalidStatementOrder);
    YANG_DIAGNOSTIC_NAME(kUnsupportedYangVersion);
    YANG_DIAGNOSTIC_NAME(kStatementRequiresYang11);
    YANG_DIAGNOSTIC_NAME(kUnknownPrefix);
    YANG_DIAGNOSTIC_NAME(kModuleNotFound);
    YANG_DIAGNOSTIC_NAME(kModuleNameMismatch);
    YANG_DIAGNOSTIC_NAME(kRevisionMismatch);
    YANG_DIAGNOSTIC_NAME(kBelongsToMismatch);
    YANG_DIAGNOSTIC_NAME(kDuplicatePrefix);
    YANG_DIAGNOSTIC_NAME(kDependencyCycle);
    YANG_DIAGNOSTIC_NAME(kDuplicateSymbol);
    YANG_DIAGNOSTIC_NAME(kUnknownSymbol);
    YANG_DIAGNOSTIC_NAME(kInvalidQualifiedName);
    YANG_DIAGNOSTIC_NAME(kTypeCycle);
    YANG_DIAGNOSTIC_NAME(kInvalidTypeRestriction);
    YANG_DIAGNOSTIC_NAME(kInvalidDefaultValue);
    YANG_DIAGNOSTIC_NAME(kDuplicateSchemaNode);
    YANG_DIAGNOSTIC_NAME(kGroupingCycle);
    YANG_DIAGNOSTIC_NAME(kInvalidRefineTarget);
    YANG_DIAGNOSTIC_NAME(kInvalidSchemaPath);
    YANG_DIAGNOSTIC_NAME(kUnknownSchemaNode);
    YANG_DIAGNOSTIC_NAME(kInvalidAugmentTarget);
    YANG_DIAGNOSTIC_NAME(kInvalidKey);
    YANG_DIAGNOSTIC_NAME(kInvalidUnique);
    YANG_DIAGNOSTIC_NAME(kMandatoryAugmentRequiresWhen);
    YANG_DIAGNOSTIC_NAME(kInvalidFeatureExpression);
    YANG_DIAGNOSTIC_NAME(kFeatureCycle);
    YANG_DIAGNOSTIC_NAME(kIdentityCycle);
    YANG_DIAGNOSTIC_NAME(kInvalidLeafrefPath);
    YANG_DIAGNOSTIC_NAME(kInvalidLeafrefTarget);
    YANG_DIAGNOSTIC_NAME(kLeafrefCycle);
    YANG_DIAGNOSTIC_NAME(kInvalidXPath);
    YANG_DIAGNOSTIC_NAME(kUnknownXPathFunction);
    YANG_DIAGNOSTIC_NAME(kInvalidXPathFunctionArity);
    YANG_DIAGNOSTIC_NAME(kInvalidXPathType);
    YANG_DIAGNOSTIC_NAME(kUnknownXPathNode);
    YANG_DIAGNOSTIC_NAME(kInvalidDeviationTarget);
    YANG_DIAGNOSTIC_NAME(kInvalidDeviate);
    YANG_DIAGNOSTIC_NAME(kInvalidExtensionInstance);
    YANG_DIAGNOSTIC_NAME(kInvalidChoiceDefault);
    YANG_DIAGNOSTIC_NAME(kInvalidOperationPlacement);
    YANG_DIAGNOSTIC_NAME(kInvalidElementBounds);
    YANG_DIAGNOSTIC_NAME(kInvalidYinJson);
    YANG_DIAGNOSTIC_NAME(kInvalidYinInput);
    YANG_DIAGNOSTIC_NAME(kResourceLimitExceeded);
#undef YANG_DIAGNOSTIC_NAME
  }
  return "kUnknownDiagnostic";
}

std::string FormatDiagnostic(const Diagnostic& diagnostic,
                             const SourceFile* source) {
  const std::string_view severity =
      diagnostic.severity == DiagnosticSeverity::kError ? "error" : "warning";
  const std::string_view source_name = !diagnostic.source_name.empty()
      ? std::string_view(diagnostic.source_name)
      : source != nullptr ? source->name() : std::string_view("<unknown>");
  std::ostringstream output;
  output << fmt::format("{}:{}:{}: {} {}: {}", source_name,
                        diagnostic.range.begin.line,
                        diagnostic.range.begin.column, severity,
                        DiagnosticCodeName(diagnostic.code),
                        diagnostic.message);
  if (source != nullptr && source->name() == source_name) {
    const std::string_view contents = source->contents();
    std::size_t begin = diagnostic.range.begin.byte_offset;
    begin = std::min(begin, contents.size());
    while (begin > 0 && contents[begin - 1] != '\n' &&
           contents[begin - 1] != '\r') {
      --begin;
    }
    std::size_t end = diagnostic.range.begin.byte_offset;
    end = std::min(end, contents.size());
    while (end < contents.size() && contents[end] != '\n' &&
           contents[end] != '\r') {
      ++end;
    }
    output << '\n' << contents.substr(begin, end - begin) << '\n';
    const std::size_t column = diagnostic.range.begin.column > 0
        ? diagnostic.range.begin.column - 1 : 0;
    const std::size_t width = std::max<std::size_t>(
        1, diagnostic.range.end.column > diagnostic.range.begin.column
               ? diagnostic.range.end.column - diagnostic.range.begin.column
               : 1);
    output << std::string(column, ' ') << std::string(width, '^');
  }
  for (const RelatedDiagnostic& related : diagnostic.related) {
    output << fmt::format("\nnote: {}:{}:{}: {}",
                          related.source_name.empty() ? source_name
                                                      : related.source_name,
                          related.range.begin.line,
                          related.range.begin.column, related.message);
  }
  return output.str();
}

void VectorDiagnosticSink::Report(Diagnostic diagnostic) { diagnostics_.push_back(std::move(diagnostic)); }
const std::vector<Diagnostic>& VectorDiagnosticSink::diagnostics() const noexcept { return diagnostics_; }
bool VectorDiagnosticSink::has_errors() const noexcept {
  for (const auto& diagnostic : diagnostics_) {
    if (diagnostic.severity == DiagnosticSeverity::kError) return true;
  }
  return false;
}
}  // namespace yang
