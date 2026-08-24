<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# YANG library and command-line guide

This document covers the reusable C++20 library and `yangc`. Applications that
want a complete NETCONF server should begin with the [project README](../README.md)
and [user guide](USER_GUIDE.md).

## Validate and inspect models with yangc

Validate a module and its reachable imports and includes:

```sh
./build/yangc --search ./models ./models/example.yang
```

A successful validation prints nothing and exits with status 0. Diagnostics
contain a stable code, source location, source line, and caret. Inspection
formats include source-equivalent YIN, lossless ordered YIN-tree JSON, and an
expanded effective-schema YIN view:

```sh
./build/yangc --yin examples/example.yang
./build/yangc --yin-json examples/example.yang
./build/yangc --effective-yin examples/example.yang
```

The `--yin-json` output is a versioned serialization of the YIN XML tree, not
RFC 7951 instance-data JSON.

## Compile a model in C++

`yang::Compiler` is the normal high-level entry point. It resolves module
dependencies and runs semantic processing in the required order:

```cpp
#include <yang/compiler.h>
#include <yang/source_file.h>

yang::VectorDiagnosticSink diagnostics;
auto source = yang::SourceFile::Create("models/example.yang", text,
                                       diagnostics);
yang::FilesystemModuleRepository repository({"models"});
yang::Compiler compiler(repository, diagnostics);
auto result = source ? compiler.Compile(source) : std::nullopt;
```

A compilable example is in `examples/library_example.cc`. A successful result
owns resolved modules, symbols, effective types and schemas, feature/identity
state, extensions, deviations, leafrefs, validated defaults, and XPath
constraints. Prefer this facade unless an application deliberately needs to
stop at or customize an individual pass.

## Lower-level processing

The lower-level APIs remain public for tooling and research use:

- `Parser` creates the syntax tree after UTF-8-aware lexical analysis.
- `ModuleResolver` selects revisions, resolves imports/includes, validates
  `belongs-to`, detects cycles, and caches dependencies.
- `SymbolResolver` builds module, submodule, typedef, grouping, feature,
  identity, and extension scopes.
- `TypeResolver` resolves typedef chains and restrictions for all built-in and
  derived YANG types, including unions, identityrefs, leafrefs, and patterns.
- `IdentityFeatureResolver` evaluates feature expressions and identity
  inheritance.
- `SchemaContextBuilder` expands groupings and augments, prunes disabled
  features, creates implicit cases, resolves keys/unique paths, and computes
  inherited configuration.
- `DeviationApplier`, `LeafrefResolver`, `SchemaValueValidator`, and
  `XPathValidator` complete the effective model.
- `YinConverter` produces source or deterministic effective YIN views.

Detailed behavioral decisions and supported language boundaries are recorded
in [STANDARDS.md](STANDARDS.md). Generated Doxygen output is the declaration-
level API reference.

## Validate configuration

Convert a successful compilation to the immutable runtime schema shared by
configuration parsing, edits, filtering, authorization, and NETCONF:

```cpp
#include <yang/config_validation.h>

auto runtime = yang::config::RuntimeSchemaBuilder::FromCompilation(*result);
auto parsed = yang::config::ParseDatastoreXml(runtime, config_xml);
if (parsed.document) {
  yang::config::ConfigValidator validator;
  auto checked = validator.Validate({runtime, *parsed.document});
  if (checked.valid && checked.complete) {
    // The complete explicit configuration is valid.
  }
}
```

The parser binds XML by namespace URI rather than prefix spelling. It supports
complete datastores and selected fragments with context-sensitive tri-state
validation. Continue with the focused guides for
[configuration validation](CONFIG_VALIDATION.md),
[configuration edits](CONFIG_EDIT.md), and
[NETCONF datastores](NETCONF_DATASTORE.md).

## NETCONF components

The library separates protocol logic from host authentication and I/O:

- `NetconfServer` dispatches complete XML RPC documents over the datastore.
- `NetconfSession` adds hello negotiation, base 1.0/1.1 framing, session
  registration, and close/kill handling.
- Filtering, NACM, persistence, notifications, and with-defaults are optional
  providers with focused documents in this directory.

`dangd` composes these pieces with SSH, TLS, persistence, YANG Library, NACM,
and supervised device plugins.

## Installed package

Downstream CMake projects can use the exported package:

```cmake
find_package(yang 0.1 CONFIG REQUIRED)
target_link_libraries(my_tool PRIVATE yang::yang)
target_compile_features(my_tool PRIVATE cxx_std_20)
```
