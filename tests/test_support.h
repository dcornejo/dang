// Copyright 2026 David Cornejo
// SPDX-License-Identifier: Apache-2.0

#ifndef YANG_TESTS_TEST_SUPPORT_H_
#define YANG_TESTS_TEST_SUPPORT_H_
#include <memory>
#include <string>
#include "yang/source_file.h"
namespace yang::test {
inline std::shared_ptr<const SourceFile> Source(std::string text, VectorDiagnosticSink& sink) {
  return SourceFile::Create("test.yang", std::move(text), sink);
}
}  // namespace yang::test
#endif
