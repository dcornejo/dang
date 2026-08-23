<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Standards conformance and implementation decisions

The primary language specification is [RFC 7950](https://www.rfc-editor.org/rfc/rfc7950.html)
(YANG 1.1). YANG 1 modules remain governed by
[RFC 6020](https://www.rfc-editor.org/rfc/rfc6020.html). YIN mapping follows
RFC 7950 section 13. This document records standards-sensitive issues found
during implementation, the resolution applied by the library, and deliberate
boundaries that callers should understand.

## Resolved issues

### YANG Library current and legacy views describe one schema inventory

RFC 8525 clients receive `/yang-library`, including implemented and
import-only modules, enabled features, submodules, and deviation relationships.
The same inventory generates the deprecated `/modules-state` compatibility
tree and its conformance types. Conventional running, candidate, startup, and
intended datastores reference the common schema. The initial read-only
`intended` view is identical to `running`, as RFC 8342 permits when there are
no configuration transformations. The read-only `operational` datastore
combines that applied intended configuration with the server's schema-bound
YANG Library, monitoring, and NACM state. The content identifier is
derived from the current tree, and reload emits current and legacy update
notifications only when that identifier changes. RFC 6022
`/netconf-state/schemas` identifies every compiled source with a `NETCONF`
location served by `get-schema`.

RFC 8526 `ietf-netconf-nmda` and its exact dependency sources are compiled and
retrievable. `<get-data>` and `<edit-data>` operate on the supported
conventional datastores, and ABI-v3 plugins may add schema-bound system state.
Every schema-known applied configuration node is explicitly tagged with
`ietf-origin:intended` metadata when requested, so the RFC 8526 `origin` feature,
`with-origin`, and origin filters are enabled. Additional origin identities
remain future work when dynamic, learned, system, or default configuration is
published.

### YANG version is semantic, not merely syntactic

A missing `yang-version` means version 1; it must not silently select 1.1.
The validator therefore determines the version before validating children and
gates `action`, `anydata`, `modifier`, boolean `if-feature`, multiple identity
bases, leaf-list defaults, and leafref `require-instance` accordingly.

### UTF-8, legal characters, and line endings are separate rules

RFC 7950 requires UTF-8, excludes surrogate values, Unicode noncharacters,
and most C0 controls, and permits only LF or CRLF line endings outside quoted
strings. `SourceFile` validates UTF-8 and legal scalar values before lexing.
The lexer handles the contextual rule: a standalone CR is diagnosed outside a
quoted string and preserved inside one. Source byte offsets remain byte-based,
while displayed columns count Unicode code points.

### Quoted values are normalized without preserving source presentation

Single-quoted strings preserve content; double-quoted strings apply indentation
and trailing-whitespace processing before the four legal escapes are decoded;
concatenation occurs last. The stable syntax tree retains source ranges and the
decoded argument value, but intentionally does not retain comments or exact
whitespace. This matches YIN's semantic-information requirement while honoring
the project decision not to provide a formatting round trip.

### Integer defaults have more lexical forms than instance XML

RFC 7950 permits decimal, hexadecimal, and octal forms for integer defaults,
including an optional sign. Type validation initially accepted decimal only.
It now recognizes all three default forms, still checks the exact builtin
width/range, and rejects malformed octal such as `09`.

### String and binary length count different things

String length counts Unicode characters, not UTF-8 bytes. Binary length counts
decoded octets, not base64 characters. The type checker now counts UTF-8 code
points for strings, validates base64 structure and padding, and applies binary
length restrictions to the decoded byte count.

### YANG patterns are XML Schema regular expressions

The C++ standard library's ECMAScript grammar differs materially from the
regular-expression language referenced by RFC 7950: it lacks XML Schema
Unicode category escapes and character-class subtraction, and its search API
does not provide YANG's implicit whole-value matching. The library now compiles
patterns with libxml2's XML Schema regular-expression engine, caches compiled
expressions by source text, and retains `invert-match` as a separate YANG
restriction. The same engine is used by the YANG XPath `re-match()` extension.
Invalid expressions fail type resolution instead of being reinterpreted under
a different regular-expression dialect.

### Derived restrictions must narrow their base

Range and length expressions must be ordered, disjoint, within builtin bounds,
and wholly contained by the inherited restriction. The resolver validates all
four properties before accepting a derived type and validates defaults against
the final effective restriction.

### Schema-node names carry module identity

Comparing local names alone makes cross-module augments ambiguous. Effective
nodes use `(module, local-name)` identities, while a node reference separately
records the module tree that contains it. This preserves the augmenting module's
namespace without confusing it with ownership of the augmented tree.

### Leafref has schema constraints beyond path syntax

Leafref paths use their restricted grammar and `current()` predicate context,
must end at a leaf or leaf-list, and cannot form cycles. In addition, a
configuration leafref cannot target state data. Resolution now checks each rule
against the effective, feature-pruned, deviation-adjusted schema.

### `when` context changes under `uses` and `augment`

The textual declaration is not always the evaluation context. Effective
constraints retain both their defining module (for prefixes) and their effective
context node. Constraints copied by `uses` or introduced by augment are then
statically validated from the RFC-defined context rather than their lexical
parent.

### YIN extension arguments use the declaration's namespace and shape

An extension's argument name and `yin-element` setting come from its resolved
declaration, which can be local, imported, or included. The converter resolves
that metadata before output, rejects missing or unexpected arguments, emits the
declaring module's namespace, and writes namespace declarations deterministically.

### JSON output is not RFC 7951 instance-data JSON

RFC 7951 encodes data modeled by YANG; it does not define a JSON form of a YIN
syntax tree. The library therefore uses an explicitly versioned private format,
`yang-cpp-yin-tree-v1`, containing ordered attributes and child nodes. It is a
lossless interchange form for the in-memory YIN tree and round-trips back to
identical compact XML. It must not be presented as RFC 7951 output.

## Deliberate boundaries and known limitations

### A `choice` permits multiple explicit `case` statements

Configuration choice tests exposed a registry error: `case` was legal inside
`choice` but was not classified as repeatable. RFC 7950 section 7.9 permits
zero or more `case` substatements. It is now repeatable, and tests reject data
that activates two cases simultaneously.

### Configuration validation is intentionally staged

The runtime checks complete trees, standalone partial trees, and fragments
composed with supplied context. Its effective data view supports runtime XPath,
leafref and instance-identifier targets, defaults, and `unique`. When partial
coverage leaves a reference or constraint undecidable, the result is explicitly
indeterminate rather than guessed. `unique` is enforced for leaf descendants
and accounts for defaults beneath non-presence containers; RFC 7950 permits
only leaf targets, so leaf-list targets are rejected during schema compilation.

### Reference values depend on XML namespace context

Identityref and instance-identifier strings contain qualified names whose XML
prefixes are scoped at the value element, not copied from the YANG module's
prefix statement. Configuration nodes therefore retain their in-scope XML
namespace bindings. Runtime identityref validation compares expanded names
against the compiler's derived-identity graph. Leafrefs reuse compiler-resolved
schema targets and predicate bindings; instance-identifiers resolve absolute
paths and legal list/leaf-list predicates against the explicit tree. Missing
`require-instance` targets are definite only for complete coverage and become
indeterminate for partial trees.

### Runtime XPath preserves dynamic context and partial-tree uncertainty

Static XPath acceptance and runtime truth are separate responsibilities. The
runtime evaluator supports node sets, virtual defaults, relative/absolute
paths, predicates, comparisons, arithmetic, boolean logic, core string and
numeric functions, abbreviated descendant navigation, `position()`, `last()`,
`current()`, `deref()`, identity, enum, and bits helpers. Predicate evaluation
retains the candidate position and node-set size, while `current()` continues
to reference the initial constrained node. Missing selections in partial trees
produce `kXPathIndeterminate`; they are never treated as false. This preserves
the validator's no-guessing contract.

### YIN input is normalized through the YANG compiler

Maintaining a second semantic implementation for YIN would allow YANG and YIN
to drift. The runtime adapter reconstructs normalized YANG statements from
built-in source-equivalent YIN and runs the normal compiler. Extension-instance
elements are rejected for now because their argument name and `yin-element`
shape depend on declarations that would need to be resolved before conversion.

### NETCONF transactions are separated from transport and capabilities

RFC 6241 describes datastore operations together with capabilities, RPC
framing, sessions, and access control hooks. The library implements the state
transitions independently: locks, edit test/error options, candidate commit and
discard, startup copy/delete, and confirmed-commit confirmation, cancellation,
timeout, persistence tokens, and session-loss rollback. Capability negotiation,
authentication, NACM, RPC framing, and durable storage remain in the embedding
server so an application does not accidentally advertise facilities it has not
configured. Confirmed-commit expiration is driven explicitly by
`ProcessTimeouts`, avoiding a hidden background thread and making time-based
behavior deterministic in tests.

The XML service advertises only operations backed by the in-memory manager and
keeps RFC 6242 framing out of the RPC parser. Unsupported retrieval filters are
rejected rather than silently treated as an unfiltered request. Persistent
confirmed-commit sequences retain the rollback snapshot from the first commit,
allow follow-up commits carrying the matching `persist-id`, and require the
same token for final confirmation or cancellation.

RFC 7950's `ordered-by user` edit attributes are metadata in the YANG XML
namespace, not configuration attributes. `ParseEditXml` removes them before
ordinary schema binding and retains typed insertion metadata beside each stable
configuration node. The editor then applies `first`, `last`, `before`, and
`after` only to user-ordered list and leaf-list collections. List anchor
prefixes are resolved from the edited element's in-scope XML namespace bindings
and matched to schema keys as expanded names.

Tree differences retain ordering as configuration state. A minimal set of
existing list or leaf-list instances whose relative positions changed is
reported as move events, with old and new one-based positions. This makes moves
visible to plugins and the English backend delta stream and maps them to NACM
`update` authorization instead of allowing a reorder to bypass write checks.
Leaf-list instance paths include their value as a self predicate for both
effective write deltas and schema-aware NACM read filtering. Predicate values
containing an apostrophe use a double-quoted literal, preserving
instance-specific authorization instead of falling back to collection-wide
matching.

### RFC 6242 framing is incremental and bounded

Hello documents always use the legacy end marker. After both capability sets
are known, base 1.1 selects chunked framing and base 1.0 falls back to the end
marker. Chunk sizes count octets, reject zero and leading zeroes, and are capped
at both RFC 6242's uint32 limit and the configured reassembled-message limit.
Malformed framing terminates the session instead of attempting resynchronization,
as required by RFC 6242. The implementation is transport-neutral: it does not
claim to provide SSH itself, but can be placed behind an authenticated SSH or
TLS byte stream.

### Filtering and access control fail closed at their boundaries

RFC 6241 subtree and XPath filters are evaluated after datastore retrieval and
before reply serialization. XPath expressions use the namespace bindings on
the filter, run at the conceptual datastore root, require a node-set, and
retain ancestor paths. NACM execution checks occur before RPC dispatch; data-change
checks occur before a transaction is published; denied reads are omitted. The
policy API follows RFC 8341 defaults and its special unconditional allowance
for `close-session`. It loads the `ietf-netconf-acm` XML container, preserves
rule-list order, evaluates module and rule-type selectors, and combines local
and optional transport groups. Policy is copied at RPC entry so one rule set
governs the complete request while sharing thread-safe denied-operation,
denied-data-write, and denied-notification counters. Ordered notification
authorization is applied by the RFC 5277 delivery subsystem before queueing.
Data-node rules structurally match namespace-expanded instance-identifier
segments and predicates; omitted key predicates match every list entry as
specified by RFC 8341. XML path names must carry explicit prefixes, following
RFC 7950 Section 9.13.2 and the RFC Editor's resolution of rejected RFC 8341
Erratum 6493. Effective `default-deny-all` and `default-deny-write` annotations
are preserved from YANG or YIN, inherited by descendants, and enforced after
explicit rules. The YIN adapter recognizes these two standard extension
elements by the imported NACM namespace while continuing to reject unknown
extension metadata. Schema-aware tree differences authorize every affected
edit, commit, and datastore or URL replacement atomically; URL targets fail
closed if their prior content cannot be inspected.

### RFC 6243 defaults remain a retrieval projection

The datastore's explicit-node model maps directly to RFC 6243's `explicit`
basic mode. `report-all` and `report-all-tagged` are produced from the
read-only effective view, while `trim` removes explicit scalar values equal to
their schema defaults. None of these modes mutates stored configuration. For
`report-all-tagged`, only synthesized scalar defaults receive the namespaced
`default="true"` attribute; an explicitly supplied default value remains
untagged under the explicit basic mode. Expansion precedes NACM and filtering,
so synthesized data cannot bypass access control and can be selected by either
retrieval filter. Capability and module URIs are advertised only when the host
opts into the completed behavior.

RFC 5277 notifications are enabled only when the host registers the mandatory
`NETCONF` stream. The server then advertises both `:notification` and
`:interleave`, accepts one `create-subscription` per session, validates replay
times and transient subtree/XPath filters, and delivers complete asynchronous
notification documents through the existing framing boundary. Replay and live
delivery apply stream, time, NACM, and filter selection in that order.
`replayComplete` and `notificationComplete` remain unfilterable. Bounded replay
logs and per-session count/byte ceilings resolve the RFC's implementation-
specific storage and resource policy: oversized replay is rejected, while a
live queue overflow terminates that subscription without blocking publishers.
Stream discovery data is supplied by the embedding application's operational
datastore rather than synthesized into configuration data by this library.

RFC 6242 SSH and RFC 7589 TLS are exposed through a common authenticated,
nonblocking secure-stream adapter. SSH connections require the exact
`netconf` subsystem and preserve the authenticated username without
transformation; TLS connections require the host to complete mutual X.509
validation and certificate-to-name mapping first. Transport groups flow into
NACM unchanged. Deterministic message, queue-count, queue-byte, and inactivity
limits close and clean up sessions on exhaustion, timeout, cancellation, EOF,
or write failure. The abstract write contract uses whole-buffer acceptance to
resolve partial-write ownership consistently across secure-transport stacks.

`dangd` implements the SSH host side with libssh. It permits only explicitly
configured username/public-key records, sources trusted NACM groups from those
records, applies the shared exact username mapper, and accepts only a session
channel requesting the exact `netconf` subsystem. Password and shell access are
outside this service.

RFC 8071 Call Home is represented by a host connector with default ports 4334
for SSH and 4335 for TLS. The connector reverses TCP initiation only; the
device remains the SSH/TLS and NETCONF server. Trust policy, cryptographic
implementation, reconnect scheduling, keepalives, DNS, and sockets remain
application responsibilities and are not falsely claimed by the core library.

Persistent snapshots include the original running tree and wall-clock expiry
for confirmed commits. This resolves the mismatch between the monotonic clock
needed during a process lifetime and the real-time deadline needed across a
restart: restore converts the persisted wall-clock remainder back to a new
monotonic deadline, or rolls back immediately if it has elapsed.

### Session identity separates protocol ownership from authorization

RFC 6241 session IDs identify locks, confirmed commits, and `kill-session`
targets, while NACM decisions identify authenticated users. The implementation
keeps these values separate so concurrent sessions for one username do not
share lock ownership. A successful `kill-session` releases locks and resources
and rolls back a non-persistent confirmed commit before acknowledging success;
transport closure is then signaled through the session event-loop API. Killing
the calling session and naming an inactive or malformed ID return
`invalid-value`.

### Complete inline copy sources use replacement semantics

RFC 6241 permits `copy-config` to take a complete inline `config` source. The
library parses and validates that source, computes replacement changes through
the normal transaction engine, applies NACM to explicit changes, and publishes
the target only on success. It rejects edit-operation metadata within the
source and rejects identical datastore source and target selections.

The optional RFC 6241 `:url` capability uses a host-supplied provider. Only
explicitly declared schemes are advertised and accepted; the core library
performs no ambient filesystem or network access. This preserves URL transport
flexibility while leaving credentials, redirects, atomic writes, and local-file
policy with the embedding application.

### Cross-module mandatory augments stop at optional instances

RFC 7950 requires a `when` expression when a cross-module augment adds a
mandatory configuration node. A mandatory descendant of an optional presence
container or a list whose effective `min-elements` is zero does not force that
instance to exist. The augment checker therefore stops at those optional
boundaries, while still treating a positive `min-elements` or an unshielded
mandatory leaf/choice as mandatory. Testing RFC 8344 exposed this distinction.

The same investigation found that augmented containers and lists were not
retaining `presence` and `ordered-by user`. Both properties are now copied into
the effective schema, matching their behavior on directly declared and
`uses`-expanded nodes.

### XPath has separate static and runtime phases

The library parses XPath 1.0/YANG functions, checks precedence, names, arity,
static value categories, prefix resolution, predicate contexts, and deterministic
schema paths. Wildcards, descendant selections, and the truth of `must`/`when`
depend on a runtime data tree, so the compiler retains them for the configuration
validator. The runtime phase evaluates them against the effective view, which
includes defaults, and preserves indeterminacy when partial coverage prevents a
definitive result.

### Effective YIN is an inspection view

`Convert` and `ConvertSubmodule` implement source YANG-to-YIN mapping.
`ConvertEffective` is a library extension for inspecting expanded schemas after
features, uses, augments, and deviations. It is deterministic, but it is not a
normative RFC 7950 source round trip because an effective schema is not itself
the original module syntax.

## Conformance evidence

The test suite covers malformed UTF-8, quoting and concatenation, stable source
ranges, every registered built-in statement's metadata, imports/includes and
revisions, type restrictions/defaults, features and identities, groupings,
augments, deviations, schema paths, keys/unique, leafrefs, XPath, extensions,
source and effective YIN, JSON round trips, diagnostics, and CMake consumption.
It builds an effective tree from an adapted RFC 7950 section 4.2.2.5 example,
compiles pinned and checksummed RFC 8343/RFC 8344 dependency closures, and
rejects every entry in a persistent malformed-YANG corpus.

The NETCONF interoperability corpus contains complete XML requests and
observable reply fragments derived from RFC 6241. It deliberately varies XML
prefixes and includes error paths so the vectors can be replayed against other
implementations without depending on serialization choices. Our automated
suite replays them against the in-memory server. This is portable evidence, not
a claim of certification against a particular vendor release; live SSH/TLS
server matrices remain release-environment work. Expected deviations are the
documented transport integration boundaries, host-provided URL I/O, effective
YIN's non-normative inspection form, and the intentional loss of source
formatting. No deviation is expected for the pinned modules or vector outcomes.

## Hardening findings

ASan found that partial-tree validation retained references into the composed
configuration node arena while recursively cloning fragment children. Arena
growth invalidated those references. The implementation now retains stable
`ConfigNodeId` values and reacquires nodes after every operation that can grow
the arena. This does not change RFC 7950 or NETCONF semantics; it makes the
existing full/partial-tree composition contract memory-safe.
