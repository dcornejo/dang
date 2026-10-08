<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Configuration validation

This release turns a compiled YANG dependency closure into one immutable
runtime schema, parses NETCONF XML configuration into an explicit stable tree,
and validates that tree without changing it. XML prefixes are presentation:
elements bind by namespace URI and local name.

## Basic use

```cpp
yang::config::RuntimeSchema schema =
    yang::config::RuntimeSchemaBuilder::FromCompilation(*compilation);
auto parsed = yang::config::ParseDatastoreXml(schema, xml);
if (parsed.document) {
  yang::config::ConfigValidator validator;
  auto checked = validator.Validate({schema, *parsed.document});
  if (checked.valid && checked.complete) {
    // The complete explicit configuration is valid.
  }
}
```

A runnable version is `examples/config_validation_example.cc`.
`ParseDatastoreXml` accepts either a bare data element or an RFC 6241 `config`
or `data` wrapper. Stored nodes remain explicit; validation inserts no defaults.

## Full and partial trees

The default is a complete datastore. For a selection, parse with
`Coverage::kSelected` and validate with
`ValidationScope::kPartialStandalone`. Directly observable violations remain
invalid; requirements that omitted data might satisfy are `kIndeterminate`.
Thus `valid` can be true while `complete` is false.

`kPartialWithContext` requires an explicit context document. The fragment is
overlaid immutably on matching context nodes; omitted siblings remain visible
to structural, reference, uniqueness, and XPath checks. An explicit attachment
node resolves fragments whose schema parent has multiple context instances.

## Checks in this release

- XML well-formedness, expanded-name binding, and scalar/container shape;
- configuration versus state-data classification;
- singleton multiplicity, list key presence, and duplicate list keys;
- scalar lexical spaces and effective type restrictions;
- mandatory nodes/choices, choice exclusivity, and min/max bounds; a missing
  node guarded by `when` is evaluated in its hypothetical effective-data
  context and is required only when that condition applies;
- descendant-leaf `unique`, including virtual defaults under non-presence
  containers;
- identityref QName resolution and derived-identity membership;
- leafref target existence, effective target typing, `current()` list-key
  predicates, and `require-instance`;
- absolute instance-identifier lookup with list-key, leaf-list-value, and
  leaf-list-position predicates;
- expanded-name instance paths and basic NETCONF error tags.
- read-only effective data with virtual leaf/leaf-list defaults and synthesized
  non-presence containers;
- runtime `must` and `when` evaluation for paths, predicates, boolean and
  comparison operators, and commonly used XPath 1.0 functions;
- per-subtree and per-collection coverage overrides, deterministic finding
  ordering, and XML source locations;
- source-equivalent built-in YIN input through the same compiler pipeline.

Reference values use the namespace declarations in scope on their XML element.
An unprefixed identity or path step denotes the data node's module namespace;
cross-module names use an XML prefix. Missing required targets are invalid for
a complete tree and indeterminate for a partial tree. `require-instance false`
still requires valid lexical syntax and scalar typing but permits no target.

Reference alternatives nested inside unions are supported; union leafrefs with
list predicates remain deferred so they cannot be accepted without applying
their selection semantics. Runtime XPath covers the core XPath 1.0 operators,
relative and absolute paths, abbreviated self/parent/descendant navigation,
dynamic predicates, `position()`, `last()`, `current()`, namespace-aware node
tests, and commonly used standard/YANG functions. Node-set equality and
relational operations examine all candidate pairs as XPath 1.0 requires.
Expressions whose result depends on unavailable partial-tree data or an
unimplemented extension function are reported as indeterminate rather than
guessed. The YIN adapter currently rejects extension-instance elements because
their argument metadata is unavailable until extensions are resolved. See
`CONFIGURATION_LIBRARIES_PLAN.md` for the staged design.
