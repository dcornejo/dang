<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Configuration validation and manipulation libraries

Status: implemented foundation and roadmap. The validation library, effective
data view, context-assisted partial validation, and core atomic RFC 6241 edit
library are implemented.

Implementation update: effective defaults, uniqueness, runtime references,
runtime XPath, context overlays, mixed coverage, source-aware deterministic
findings, built-in YIN input, and the core atomic RFC 6241 manipulation library
are now present. The transaction layer now also provides running, candidate,
and startup management, locks, commit and confirmed commit, test/error options,
and ordered-by-user controls. Remaining items describe transport integration,
uncommon XPath dynamic-context features, extension-aware YIN bootstrap,
durable persistence, NACM, fuzzing, and large-tree benchmarking.

## Decisions

- Schemas may be supplied as YIN XML or as the existing `yang::Compilation` /
  effective schema. Both compile into one immutable runtime schema.
- Configuration encoding is NETCONF XML. The internal data model remains
  encoding-neutral enough to add RFC 7951 JSON later without changing schema,
  validation, or edit semantics.
- Full and partial trees are first-class inputs.
- Partial validation supports standalone and context-assisted modes.
- Manipulation begins with core RFC 6241 node operations: `merge`, `replace`,
  `create`, `delete`, and `remove`.
- Datastore transactions are independent from candidate/running/startup,
  locking, commit, and confirmed-commit workflows.
- Stored trees contain explicit nodes only. Defaults are exposed through an
  effective view and never silently inserted into stored XML.

## Product boundaries

The work is split into three packages with one-way dependencies:

```text
yang-schema-runtime
  immutable effective schema, namespace/name interning, type codecs,
  compiled XPath, schema paths, identity and leafref metadata
          |
          +--------------+
          v              v
yang-config          yang-config-validate
explicit XML tree    validation engine and reports
          |              ^
          +------+-------+
                 v
          yang-config-edit
          RFC 6241 edit planning and atomic application
```

`yang-config` is the shared data-tree package, not a third user-facing goal.
Validation never mutates. Editing depends on validation and exposes candidate
results atomically.

## Shared runtime schema

### Inputs

`RuntimeSchemaBuilder` has two adapters:

1. `FromCompilation(const yang::Compilation&)` reuses the existing effective
   schema directly.
2. `FromYin(const pugi::xml_document&, ModuleRepository&)` parses YIN into the
   same syntax/semantic pipeline and then lowers it to runtime form.

The YIN adapter must consume source-equivalent YIN, not the library-specific
effective-YIN inspection view. A format marker or separate function prevents
accidental misuse.

### Runtime representation

`RuntimeSchema` is immutable, reference-counted, and safe for concurrent reads.
Each `RuntimeSchemaNode` contains:

- stable schema ID and qualified XML name (`namespace URI`, local name);
- node kind, parent, children, choice/case membership;
- configuration/state classification and presence-container status;
- list keys, `unique`, ordering, min/max, mandatory, defaults;
- compiled scalar codec and restriction set;
- compiled `must`, `when`, leafref, and instance-identifier expressions;
- identityref value space and feature-pruned availability;
- source locations for diagnostics.

Lookup indexes cover top-level names, child names, list keys, and schema paths.
Prefix spelling is never semantic; namespace URIs are.

## Shared explicit configuration tree

`ConfigDocument` owns a stable arena of `ConfigNode` values and an XML namespace
table. It records only explicitly supplied data.

```cpp
struct ConfigNode {
  ConfigNodeId id;
  RuntimeSchemaNodeId schema;
  std::optional<ConfigNodeId> parent;
  QualifiedXmlName name;
  NodeOrigin origin;  // parsed, created, merged, replaced
  std::optional<TypedValue> value;
  std::vector<ConfigNodeId> children;
  SourceRange source_range;
};
```

Lists and leaf-lists preserve document order. System-ordered collections may be
compared canonically but are not arbitrarily reordered during parsing. Unknown
attributes are rejected unless explicitly allowed by an extension policy.
NETCONF protocol attributes are recognized only in edit documents.

Parsing has two entry points:

- `ParseDatastoreXml`: ordinary explicit configuration data; rejects operation
  attributes.
- `ParseEditXml`: configuration edit payload; retains RFC 6241 operation
  attributes as edit metadata rather than data nodes.

An encoding interface isolates XML parsing so a later JSON adapter can emit the
same typed `ConfigDocument`.

# Goal 1: configuration validation library

## Public API shape

```cpp
enum class ValidationScope { kComplete, kPartialStandalone, kPartialWithContext };
enum class FindingState { kInvalid, kIndeterminate };

struct ValidationRequest {
  const RuntimeSchema& schema;
  const ConfigDocument& document;
  ValidationScope scope;
  const ConfigDocument* context = nullptr;
  std::optional<InstancePath> attachment_point;
};

struct ValidationResult {
  bool valid;
  bool complete;
  std::vector<ValidationFinding> findings;
};

ValidationResult Validate(const ValidationRequest& request);
```

`valid` means no proven violation. `complete` means every applicable constraint
was decidable. A standalone partial tree can therefore be valid but incomplete.
Callers that require a definitive answer check both fields.

## Partial-tree contract

### Complete

The document represents the complete configuration datastore. All applicable
constraints are decidable and enforced.

### Partial standalone

The document is attached at a declared schema node but has no surrounding
datastore. Local structure and values are fully checked. Any constraint that
could depend on omitted ancestors, siblings, other list entries, or remote
leafref targets produces an `indeterminate` finding, not a false error.

Absence is not treated as missing unless completeness is known for the relevant
parent. Each parsed node therefore carries a coverage marker:

- `complete-children`: all children of this instance are present;
- `selected-children`: only the supplied selection is known;
- `complete-collection`: all entries of a list/leaf-list are present;
- `selected-collection`: collection membership is incomplete.

The root coverage defaults from `ValidationScope`; callers may refine it per
subtree when filters produce mixed coverage.

### Partial with context

The fragment is overlaid read-only at `attachment_point` on a complete or
partially covered context. XPath, uniqueness, leafrefs, and ancestor-dependent
rules evaluate against the composed view. Context nodes are never copied or
mutated.

## Validation phases

1. **XML envelope** — well-formed XML, namespace correctness, one schema match
   per element, legal text/attribute shape.
2. **Tree shape** — parent/child legality, singleton multiplicity, list-key
   presence and order independence, leaf/leaf-list scalar content.
3. **Value typing** — canonical typed values, range, length, pattern, enum,
   bits, binary, decimal64, identityref and instance-identifier syntax.
4. **Local structural constraints** — mandatory, min/max, choice exclusivity,
   presence-container rules and configuration-only nodes.
5. **Cross-node constraints** — list keys, `unique`, leafref targets,
   require-instance and instance-identifier targets.
6. **XPath constraints** — `when` accessibility and `must` expressions over an
   immutable effective data view containing explicit plus virtual defaults.
7. **Final classification** — invalid versus indeterminate findings, ordered
   deterministically by instance path and constraint source.

Defaults remain virtual. They participate where RFC 7950 says defaults are in
use, including `unique` and XPath, but never acquire a `ConfigNodeId` unless an
explicit effective-view iterator is requested.

## Findings and NETCONF mapping

Every finding contains a stable library code, severity/state, instance path,
schema source, XML source range, message, and optional NETCONF mapping:

- invalid scalar → `invalid-value`;
- missing required node/key → `missing-element` or `data-missing`;
- duplicate singleton or choice conflict → `bad-element`;
- `unique` → `operation-failed` / `data-not-unique`;
- min/max → `operation-failed` / `too-few-elements` or `too-many-elements`;
- `must` → `operation-failed` / declared app-tag or `must-violation`;
- require-instance → `data-missing` / `instance-required`;
- mandatory choice → `data-missing` / `missing-choice`.

The standalone validator reports library findings. The edit library converts
them into ordered `rpc-error` records.

## Validation milestones

1. Runtime-schema adapters and XML-name matching.
2. Explicit tree parser, typed scalar codecs, stable instance paths.
3. Complete-tree structural and scalar validation.
4. Defaults, choices, list keys, uniqueness, and collection bounds.
5. Data-tree XPath evaluator and `when`/`must` dependency ordering.
6. Leafref and instance-identifier runtime resolution.
7. Coverage model and standalone partial tri-state validation.
8. Context overlays and context-assisted partial validation.
9. Diagnostics, NETCONF error mapping, performance and concurrency hardening.

Exit criteria: conformance fixtures cover every phase; identical input produces
stable findings; a complete tree never yields indeterminate; no validation API
mutates its inputs.

# Goal 2: configuration manipulation library

## Public API shape

```cpp
enum class EditOperation { kMerge, kReplace, kCreate, kDelete, kRemove };

struct EditRequest {
  const RuntimeSchema& schema;
  const ConfigDocument& target;
  const EditDocument& edit;
  EditOperation default_operation = EditOperation::kMerge;
  ValidationPolicy validation;
};

struct EditResult {
  std::optional<ConfigDocument> candidate;
  std::vector<NetconfError> errors;
  ChangeSet changes;
};
```

The core API is datastore-independent and atomic: target and edit are immutable;
success returns a new candidate plus a normalized change set, failure returns
errors and no candidate. Copy-on-write arenas allow unchanged subtrees to be
shared.

## Operation semantics

- **merge**: create missing nodes and recursively merge present content;
  unspecified children remain unchanged.
- **replace**: replace the identified node and its represented subtree with the
  edit content; at the edit root, default `replace` applies at that level.
- **create**: create only when the addressed instance does not exist; otherwise
  return `data-exists`.
- **delete**: delete only when it exists; otherwise return `data-missing`.
- **remove**: delete when present and otherwise succeed without change.

Explicit per-node `nc:operation` overrides the inherited/default operation.
Operation attributes are removed from the resulting data tree. Protocol
attributes never become configuration attributes.

List identity is the tuple of typed key values. Leaf-list identity is its typed
value. Containers and leaves use qualified name under their parent. All matches
use namespace URI plus local name, never input prefixes or lexical value alone.

## Full and partial edits

An edit payload is naturally partial. Omitted nodes mean “no instruction” for
merge/create/delete/remove. For replace, omission removes descendants only
inside the subtree whose root is actually replaced; it never implies replacing
unknown ancestors.

The target may also be partial. Safe operations are allowed only where coverage
proves the relevant existence/nonexistence and replacement boundary:

- create requires known absence;
- delete requires known presence;
- remove may succeed on known absence but is indeterminate on unknown absence;
- replace requires complete knowledge of the replaced node's current parent
  identity and produces a complete replacement subtree;
- merge can update known nodes, but creating a keyed instance requires complete
  key identity and must not assert collection-wide uniqueness without context.

An indeterminate edit returns `context-required` library errors and no candidate.
Supplying a context datastore converts the partial target to an overlay and
allows a definitive transaction.

## Transaction algorithm

1. Parse and schema-bind the edit XML.
2. Normalize inherited and explicit operations.
3. Resolve every target instance using typed list/leaf-list identity.
4. Build an edit plan without mutating the target.
5. Detect operation conflicts and RFC 6241 existence errors.
6. Apply the plan to a copy-on-write candidate.
7. Apply YANG automatic effects: selecting a choice case removes competing
   cases; nodes whose `when` becomes false are removed.
8. Validate the candidate using Goal 1.
9. On success, return candidate and change set; on failure, discard candidate
   and return mapped errors.

The node editor behaves like `test-then-set` with atomic rollback. The datastore
manager adds `test-only`, `set`, all three error options, and the protocol
workflow state transitions while continuing to reuse the same editor.

## Change set

`ChangeSet` records stable instance paths and typed before/after snapshots with
`created`, `deleted`, `value-changed`, and `subtree-replaced` events. It supports
auditing, subscriptions, dry-run display, and later datastore commit machinery.
It does not expose internal arena addresses.

## Manipulation milestones

1. Edit XML parser and operation inheritance.
2. Typed instance addressing for containers, lists, leaves, and leaf-lists.
3. Pure merge and replace planner/application.
4. Create/delete/remove existence semantics and NETCONF errors.
5. Choice-case and `when` automatic cleanup.
6. Candidate validation integration and atomic rollback.
7. Partial-target coverage rules and context overlays.
8. Deterministic change sets, performance, fuzzing, and concurrency hardening.
9. Candidate/running/startup, locks, commit, confirmed-commit, and test/error
   options. Durable persistence remains an embedding adapter.

Exit criteria: operations match RFC 6241 fixtures; input target and edit remain
unchanged; failed transactions expose no candidate; successful candidates pass
complete validation; partial uncertainty is never guessed.

## Testing strategy

- RFC examples and errata-driven fixtures for operation inheritance and errors.
- Matrix tests for five operations × existing/missing node × node kind × full/
  partial/contextual target.
- Namespace-prefix substitution tests proving URI-based identity.
- Property tests: merge idempotence, remove idempotence, failed-edit atomicity,
  and XML parse/serialize/parse equivalence.
- Mutation tests for choice cleanup, `when` cleanup, defaults, uniqueness,
  leafrefs, and instance identifiers.
- Differential tests against at least one established NETCONF/YANG validator
  for shared supported behavior.
- Fuzzers for XML binding, typed values, XPath evaluation, edit normalization,
  and change-set generation.
- Large-tree benchmarks separating schema compilation, parse, validation,
  edit planning, application, and final validation.

## Deferred extensions

- RFC 7951 JSON input/output via a new encoding adapter.
- RFC 6243 retrieval modes over the effective-default view.
- SSH/TLS establishment and session authentication (RFC 6242 base-version
  negotiation, framing, XML RPC dispatch, and server advertisement are
  implemented).
- Full `ietf-netconf-acm` configuration parsing, counters, module rules, and
  copy-config data authorization (the host policy API and CRUDX enforcement
  boundary are implemented).
- Notification/subscription integration.
- Operational/state data validation as a separate policy from configuration.

## Standards baseline

- RFC 7950: YANG 1.1 data-tree XML encoding, constraints, defaults, XPath,
  automatic choice and `when` behavior, and standard error app-tags.
- RFC 6020: behavior of schemas explicitly compiled as YANG version 1.
- RFC 6241: NETCONF `edit-config` node operations and error tags.
- RFC 6243: future effective-default retrieval views; explicit storage remains
  the selected internal policy.
- XML Namespaces: expanded names determine schema/data identity.

Open standards questions found during implementation must be recorded beside
their tests in a successor to `docs/STANDARDS.md`; verified errata take
precedence over uncorrected examples, and any behavior not normatively defined
must be exposed as an explicit policy rather than an implicit guess.
