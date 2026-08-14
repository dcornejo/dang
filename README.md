<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dang - YANG + NETCONF

Copyright 2026 David Cornejo. Licensed under the Apache License, Version 2.0.
See [LICENSE](LICENSE) for the complete terms. Bundled third-party and IETF
materials retain their own copyright and license notices.

Current implementation work and completion criteria are tracked in
[TODO.md](TODO.md).
Release compatibility, reproducibility, checksums, and signature verification
are documented in [docs/RELEASING.md](docs/RELEASING.md). Release history is
maintained in [CHANGELOG.md](CHANGELOG.md).

Start with the [users guide](docs/USER_GUIDE.md) for the conceptual architecture
and a concrete end-to-end example built around `dangd`.

The separately owned [`dangd`](dangd/README.md) directory contains the
host-application foundation for building a runnable NETCONF configuration
server while keeping transport and deployment policy out of this library.

A C++20 library for parsing and semantically analyzing YANG 1.0/1.1
(RFC 6020/RFC 7950), with a pugixml-backed in-memory YIN representation.

## Build on macOS

```sh
brew install cmake fmt libxml2 pugixml nlohmann-json googletest
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

CLion can open the repository directly; its bundled CMake will discover the
Homebrew packages on a standard Apple Silicon or Intel installation.

## Quick start with `yangc`

Validate a module and all imports/includes reachable through the search path:

```sh
./build/yangc --search ./models ./models/example.yang
```

A successful validation prints nothing and exits with status 0. Diagnostics
include a stable symbolic code, source location, source line, and caret. Output
the source-equivalent YIN, the library's lossless YIN-tree JSON, or an expanded
effective-schema YIN view with:

```sh
./build/yangc --yin examples/example.yang
./build/yangc --yin-json examples/example.yang
./build/yangc --effective-yin examples/example.yang
```

The JSON produced by `--yin-json` is a versioned serialization of the YIN XML
tree, not RFC 7951 instance-data JSON.

## Recommended library workflow

`Compiler` is the high-level API. It resolves dependencies and runs symbol,
type, feature/identity, extension, effective-schema, deviation, leafref,
schema-dependent default, and XPath validation in the required order:

```cpp
#include <fstream>
#include <iostream>
#include <iterator>

#include <yang/compiler.h>
#include <yang/yin_document.h>

std::ifstream input("models/example.yang", std::ios::binary);
std::string text((std::istreambuf_iterator<char>(input)),
                 std::istreambuf_iterator<char>());

yang::VectorDiagnosticSink diagnostics;
auto source = yang::SourceFile::Create("models/example.yang", text,
                                       diagnostics);
yang::FilesystemModuleRepository repository({"models"});
yang::Compiler compiler(repository, diagnostics);
auto result = source ? compiler.Compile(source) : std::nullopt;

if (!result) {
  for (const auto& diagnostic : diagnostics.diagnostics()) {
    std::cerr << yang::FormatDiagnostic(diagnostic, source.get()) << '\n';
  }
  return 1;
}

for (yang::semantic::SchemaNodeId id : result->schemas.root().roots()) {
  const auto& node = result->schemas.root().Get(id);
  std::cout << node.name.module << ':' << node.name.local_name << '\n';
}

yang::YinConverter converter(diagnostics);
auto yin = converter.ConvertEffective(result->schemas, *result->module);
if (yin) std::cout << yin->ToString();
```

A compilable version is provided in `examples/library_example.cc`, with its
input model in `examples/example.yang`.

## Validate NETCONF XML configuration

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

The parser accepts a bare data root or NETCONF `config`/`data` wrapper and
matches namespace URIs rather than prefix spellings. Full-tree checks and
standalone partial-tree tri-state results are implemented. See
[configuration validation details](docs/CONFIG_VALIDATION.md).

Core RFC 6241 edits are available through `ParseEditXml` and `ConfigEditor`.
They apply `merge`, `replace`, `create`, `delete`, and `remove` atomically and
return a validated candidate plus a change set. See the
[configuration editing guide](docs/CONFIG_EDIT.md).

The thread-safe `DatastoreManager` adds running/candidate/startup transactions,
locks, test and error options, commit/discard, confirmed-commit rollback, and
copy/delete operations. Ordered-by-user list and leaf-list edits support the
RFC 7950 `insert`, `key`, and `value` XML attributes. See the
[NETCONF datastore guide](docs/NETCONF_DATASTORE.md).

`NetconfServer` provides capability hello generation and XML RPC dispatch over
that datastore layer. The embedding application remains responsible for
transport framing and authenticated sessions. See the
[NETCONF server guide](docs/NETCONF_SERVER.md).

`NetconfSession` adds RFC 6242 hello negotiation and incremental base 1.0/1.1
message framing, unique active-session registration, and asynchronous
`kill-session` shutdown signaling while leaving SSH, TLS, and socket I/O to the host. See the
[NETCONF framing guide](docs/NETCONF_FRAMING.md) and
[notification guide](docs/NETCONF_NOTIFICATIONS.md). Secure-stream adapters
and Call Home hooks are covered by the
[transport integration guide](docs/NETCONF_TRANSPORT.md).

Subtree and XPath retrieval filters, model-driven NACM policy enforcement, and
atomic JSON datastore snapshots are also available. See the
[filtering, NACM, and persistence guide](docs/NETCONF_FILTER_NACM_PERSISTENCE.md).
RFC 6243 default reporting is opt-in and supports `explicit`, `trim`,
`report-all`, and `report-all-tagged` without materializing defaults in stored
configuration. See the [with-defaults guide](docs/NETCONF_WITH_DEFAULTS.md).

## Parse, validate, and create YIN

The lower-level APIs remain available when an application needs to stop after
syntax parsing or control individual semantic passes.

```cpp
yang::VectorDiagnosticSink diagnostics;
auto source = yang::SourceFile::Create("example.yang", contents, diagnostics);
if (source != nullptr) {
  yang::Parser parser(source, diagnostics);
  const yang::SyntaxTree tree = parser.Parse();

  yang::YinConverter converter(diagnostics);
  if (auto yin = converter.Convert(tree)) {
    std::cout << yin->ToString();
  }
}
```

`YinConverter` performs structural validation before producing XML. Imported
extension namespaces are available when conversion receives a resolved module;
the syntax-tree overload supports self-contained modules and local extensions.

## Resolve module dependencies

```cpp
yang::FilesystemModuleRepository repository({"models", "/opt/yang/modules"});
yang::ModuleResolver resolver(repository, diagnostics);

if (auto module = resolver.Resolve(source)) {
  // module->imports maps each local import prefix to its resolved module.
  // module->includes contains validated submodules.
  auto yin = converter.Convert(*module);
}
```

The resolver understands `name.yang` and `name@revision.yang`, selects the
latest available revision when none is requested, validates `revision-date`
and `belongs-to`, detects dependency cycles, and caches resolved dependencies.

## Resolve semantic symbols

```cpp
yang::semantic::SymbolResolver symbols(diagnostics);
if (auto semantics = symbols.Resolve(module)) {
  auto declaration = semantics->root()->Find(
      yang::semantic::SymbolKind::kTypedef, "interface-name");
}
```

Symbol resolution merges declarations from included submodules, retains nested
typedef and grouping scopes, and validates local and imported references for
typedefs, groupings, features, identities, and extensions.

## Resolve effective types

```cpp
yang::semantic::TypeResolver type_resolver(diagnostics);
if (auto types = type_resolver.Resolve(*semantics)) {
  auto leaf_type = types->Find(*module, leaf_statement_id);
}
```

The type layer follows local and imported typedef chains, detects cycles, and
models range, length, pattern, decimal64, enumeration, bits, union, identityref,
leafref, and instance-identifier metadata. Restriction narrowing, numeric and
decoded-binary lengths, pattern modifiers, and lexical defaults are checked;
patterns use XML Schema regular-expression semantics (including Unicode
categories, character-class subtraction, and implicit whole-value matching);
the effective-value pass completes schema-dependent leafref and identityref
default validation, including multiple YANG 1.1 leaf-list defaults.

## Build the effective schema

```cpp
yang::semantic::SchemaBuilder schema_builder(diagnostics);
if (auto schema = schema_builder.Build(*semantics, *types)) {
  for (yang::semantic::SchemaNodeId root : schema->roots()) {
    const yang::semantic::SchemaNode& node = schema->Get(root);
    // Names retain both their defining module and local identifier.
  }
}
```

The effective tree expands local and imported groupings, applies local
`refine` properties, creates implicit cases for choice shorthand, preserves
declaration and instantiation provenance, and computes inherited `config`.

Pass an evaluated `FeatureSet` to `SchemaContextBuilder::Build` to construct a
feature-specific effective schema. Disabled data nodes, `uses`, and augments
are pruned before paths, keys, leafrefs, and XPath constraints are resolved.

Schema-node identifiers are available through `SchemaPathParser` and
`SchemaPathResolver`. The resolver supports absolute and descendant forms,
module prefixes, and transparent traversal through implicit cases. Effective
schema construction applies same-module and `uses`-local augments and resolves
list `key` and `unique` leaf paths.

`SchemaContextBuilder` builds a canonical tree for every resolved module and
then applies cross-module augments to the target tree. Augmented nodes retain
the augmenting module's namespace, `uses` inside augments are expanded, and
unconditional mandatory configuration additions are rejected unless the
augment has a `when` condition.

## Features and identities

`IdentityFeatureResolver` parses YANG 1.1 feature expressions with `not`,
`and`, `or`, and parentheses, evaluates explicitly requested features through
their dependencies, and reports cycles. The same pass builds the multiple-base
identity inheritance graph and provides transitive `IsDerivedFrom` and
`DerivedFrom` queries for identityref validation.

## Resolve leafrefs

`LeafrefResolver` resolves absolute and parent-relative leafref paths across
the multi-module schema context. It validates list-key predicates using
`current()`, requires leaf or leaf-list targets, detects target cycles, and
exposes the non-leafref effective type inherited through the target chain.

## Apply deviations

```cpp
yang::semantic::DeviationApplier deviations(diagnostics);
if (!deviations.Apply(*schemas, *types)) {
  // Diagnostics identify an invalid target or property operation.
}
```

The deviation pass supports cross-module absolute targets,
`not-supported`, and `add`, `replace`, and `delete` for effective scalar
properties and types. It preserves stable node IDs, removes unsupported nodes
from traversal, recomputes inherited configuration, and rejects invalid bounds,
mandatory defaults, and removal of list keys. Effective `must` and `unique`
constraints can also be added or deleted; XPath validation consumes the
resulting constraint set with the deviating module's prefix context.

## Validate schema-dependent defaults

After identity and leafref resolution, `SchemaValueValidator` verifies that
identityref defaults belong to a declared base identity's derived value space
and that leafref defaults are valid lexical values for the final target type.
This pass should run after feature pruning and deviations so it observes the
same effective schema exposed to clients.

## Validate must and when expressions

```cpp
yang::semantic::XPathValidator xpath_validator(diagnostics);
if (auto xpath = xpath_validator.Validate(*schemas)) {
  for (const auto& constraint : xpath->expressions()) {
    // Each result retains its statement, expression, and context schema node.
  }
}
```

The XPath pass implements expression tokenization and recursive-descent parsing
with XPath 1.0 operator precedence, the RFC 7950 function set, YANG extension
functions, namespace-prefixed names, and `current()` paths. Deterministic
schema paths are resolved across module boundaries; predicates, wildcards,
and descendant selections are retained for runtime evaluation against the
effective data view.

Static XPath checks validate built-in and YANG function argument counts and
reject malformed qualified names. Paths inside predicates use the schema node
selected by their enclosing location step, while `current()` retains the
original `must` or `when` context as required by YANG.
At runtime, predicates receive their XPath position and size, numeric
predicates use positional selection, `//` traverses descendants, and node-set
comparisons follow XPath's existential pair semantics. Missing data in a
partial tree remains indeterminate rather than becoming a false constraint.

## Extensions and effective YIN

`ExtensionResolver` creates typed definitions and instances for local,
included, and imported extensions, enforces declared argument shape and
`yin-element`, and supports application-defined validation hooks.

`YinConverter::ConvertSubmodule` supplies the owning module namespace for an
included submodule. `YinConverter::ConvertEffective` emits a deterministic YIN
inspection view from a resolved `SchemaContext`, so expanded `uses`, augments,
feature pruning, deviations, effective configuration, and multiple defaults
are reflected in the XML tree.

## Standards notes and API documentation

[Standards conformance and implementation decisions](docs/STANDARDS.md)
documents issues found while applying RFC 7950/RFC 6020, how each was resolved,
and the deliberate boundaries around XSD regex and runtime XPath evaluation.

Generate Doxygen HTML and PDF API references with:

```sh
brew install doxygen
brew install --cask mactex-no-gui
# Open a new terminal after installing MacTeX so pdflatex is on PATH.
cmake -S . -B build-docs -DYANG_BUILD_DOCS=ON
cmake --build build-docs
```

The build creates `build-docs/docs/html/index.html` and
`build-docs/docs/latex/refman.pdf`. The `yang_docs` target can also be built
explicitly. Installing this documentation configuration places the HTML tree
under `share/doc/yang/html` and the PDF at
`share/doc/yang/yang-cpp-reference.pdf`.

After installation, downstream projects can use:

```cmake
find_package(yang 0.1 CONFIG REQUIRED)
target_link_libraries(my_tool PRIVATE yang::yang)
```
