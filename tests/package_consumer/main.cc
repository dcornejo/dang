// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <yang/diagnostic.h>
#include <yang/parser.h>
#include <yang/source_file.h>

int main() {
  yang::VectorDiagnosticSink diagnostics;
  auto source = yang::SourceFile::Create(
      "consumer.yang",
      std::string("module consumer { namespace 'urn:consumer'; prefix c; }"),
      diagnostics);
  if (!source) return 1;
  yang::Parser parser(source, diagnostics);
  const yang::SyntaxTree tree = parser.Parse();
  return tree.roots().size() == 1 && diagnostics.diagnostics().empty() ? 0 : 2;
}
