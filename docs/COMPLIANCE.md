<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Standards compliance ledger

## How to read this document

This is a gap ledger, not a certification statement. It records the standards
that the source tree implements, the implemented subset, and every currently
known omission or deliberate variance. A row marked **substantial** means that
the named behavior exists and is tested; it does not mean that every normative
requirement has passed an independent conformance suite. **Partial** means that
important required behavior remains. **Integration boundary** means that the
library supplies protocol-neutral machinery but the embedding application must
provide part of the standard. **Reference only** means that an imported YANG
typedef or explanatory comment cites the RFC; no implementation claim is made.

The authoritative unfinished work is [TODO.md](../TODO.md). When a compliance
item is completed, its tests, documentation, changelog, and status here must be
updated in the same commit, and the completed item must be removed from
`TODO.md`. [STANDARDS.md](STANDARDS.md) explains resolved interpretation and
implementation decisions in more depth.

## Language, schema, and encodings

### RFC 6020 — YANG 1

Status: **substantial, not independently certified**.

YANG sources without `yang-version`, and sources declaring version 1, are
compiled with the RFC 6020 language gates. Parsing, module resolution, types,
groupings, augments, deviations, features, identities, schema paths, XPath,
defaults, and YIN conversion are covered by automated tests.

Remaining evidence: build a clause-indexed RFC 6020 conformance matrix and run
larger independent valid/invalid module corpora. Features introduced only by
YANG 1.1 remain correctly rejected for version 1 rather than treated as a
variance.

### RFC 7950 — YANG 1.1 and YIN

Status: **substantial, not independently certified**.

The compiler implements the core syntax and semantic pipeline, effective
schema construction, configuration validation, YANG XPath functions, XML
Schema regular-expression semantics, YIN mapping, and YANG 1.1 actions and
data-associated notifications.

Known boundaries: the stable syntax tree does not preserve comments or exact
source whitespace, so it is not a formatting round trip. Effective-YIN output
is a project inspection format rather than a normative source reconstruction.
The direct YIN runtime adapter rejects arbitrary extension instances because
their argument shape requires declaration resolution. A clause-indexed test
matrix, broader external corpora, and interoperability testing remain before a
complete compliance claim.

### RFC 7951 — JSON encoding of YANG data

Status: **not implemented**.

The project JSON format serializes a YIN syntax tree as
`yang-cpp-yin-tree-v1`; it is not RFC 7951 instance-data JSON. Adding an RFC
7951 instance-data adapter remains separate future work and no RFC 7951
capability or claim is made.

### RFC 7952 — metadata annotations

Status: **partial**.

The normative metadata module is compiled and RFC 8342 origin attributes can
be serialized for NMDA retrieval. Generic annotation preservation and
validation across arbitrary modeled annotations is not a complete public
instance-data facility. Per-node origin behavior is limited as described under
RFC 8342 and RFC 8526 below.

### XML 1.0, XML Namespaces, XPath 1.0, and XML Schema regular expressions

Status: **implemented for the YANG/NETCONF subsets used here**.

Pugixml supplies XML parsing and serialization. The project resolves expanded
XML names, implements the XPath subset and YANG functions needed for schema and
NETCONF evaluation, and uses libxml2's XML Schema regular-expression engine.
This is not a general-purpose validating XML Schema or complete standalone
XPath implementation. Resource limits and malformed-input tests cover the
exposed protocol paths.

## NETCONF protocol and capabilities

### RFC 6241 — NETCONF 1.1

Status: **substantial with working SSH and TLS hosts**.

Implemented behavior includes hello capabilities, XML RPC dispatch, running,
candidate and startup datastores, locks, edit operations and options,
commit/discard, confirmed commit and rollback, copy/delete configuration,
session cleanup, `close-session`, `kill-session`, subtree and XPath filters,
schema-aware errors, and host-supplied URL access.

Known boundaries and optional omissions:

- The reusable core remains transport-neutral. `dangd` embeds libssh for an
  explicit public-key-only SSH host and has a mutual-TLS host.
- The `:url` capability is advertised only for explicitly configured schemes;
  `dangd` does not currently enable it for RFC 8526 `edit-data`.
- Complete live interoperability matrices over SSH and TLS remain release work.
- The server implements only capabilities it advertises; unconfigured optional
  facilities are omissions, not protocol variances.

### RFC 6242 — NETCONF over SSH and message framing

Status: **substantial with an embedded `dangd` SSH host**.

The incremental base 1.0 end-marker and base 1.1 chunked framers, hello
negotiation, bounds, malformed-frame shutdown, exact `netconf` subsystem check,
and session-loss cleanup are implemented. `dangd` uses libssh to own its listen
socket, host private key, public-key-only user authentication, session channel,
and subsystem establishment. Authenticated usernames use the common exact
mapper, while NACM external groups come only from the matching local authorized
key record. An OpenSSH client smoke interaction covers public-key
authentication, subsystem negotiation, RPC framing, and clean close. A broader
independent matrix and sustained concurrent-session testing remain release
evidence.

### RFC 6243 — with-defaults

Status: **substantial for the advertised modes**.

The opt-in implementation advertises `explicit` as the basic mode and supports
`trim`, `report-all`, and `report-all-tagged`. Defaults are projected from the
effective schema before NACM and filtering and are not written into stored
configuration. Additional interoperability vectors, especially combinations
with RFC 8526 filters and origin metadata, remain.

### RFC 5277 — NETCONF event notifications

Status: **substantial for basic notifications**.

The implementation supports the mandatory `NETCONF` stream, named streams,
one subscription per session, replay, RFC 3339 times, subtree/XPath filters,
completion events, NACM, bounded queues, and interleaved RPC processing.

Known boundaries: applications provide event sources and stream operational
inventory. Live subscription overflow terminates the subscription and relies
on host telemetry. Independent interoperability and long-running backpressure
tests remain. Configured subscriptions and YANG Push (RFCs 8639–8641) are not
implemented or advertised.

### RFC 6022 — NETCONF monitoring

Status: **partial**.

`/netconf-state/schemas` and `get-schema` are implemented for built-in and
plugin sources. The server also exposes selected monitoring state. A complete
node-by-node audit of every RFC 6022 monitoring subtree, session statistic, and
counter remains before claiming full module implementation.

### RFC 7589 — NETCONF over TLS

Status: **integration boundary with a working dangd adapter**.

The transport abstraction carries a mutually authenticated, certificate-mapped
username into NETCONF and NACM, and `dangd` has an OpenSSL-based test listener
and simple interactive client. Production trust stores, revocation policy,
certificate-to-name configuration, algorithm policy, key protection, socket
service, and independent interoperability remain deployment responsibilities.
The generic library intentionally does not parse or trust certificates itself.

### RFC 8071 — NETCONF Call Home

Status: **partial integration hook**.

The host connector validates targets and supplies the standard SSH/TLS default
ports while preserving NETCONF client/server roles. Reconnect scheduling,
backoff and jitter, keepalives, DNS, socket creation, trust policy, and a full
Call Home state machine remain host work. No complete RFC 8071 compliance claim
is made.

## Access control and NMDA

### RFC 8341 — NACM

Status: **substantial, compliance closure incomplete**.

Implemented behavior includes datastore-managed `ietf-netconf-acm`, secure
seeding, enable/default controls, internal and external groups, ordered rule
lists, module/RPC/action/notification/data selectors, CRUDX decisions,
namespace-expanded instance paths and predicates, inherited
`default-deny-all` and `default-deny-write`, read filtering, atomic write
authorization, recovery users, notification authorization, and denial
counters. Managed policy input passes the compiled `ietf-netconf-acm` runtime
schema before policy construction and in-memory publication, and requires one
top-level NACM container with no ignored sibling roots. Schema-aware read
filtering omits elements absent from that runtime schema rather than evaluating
them without module identity or NACM annotations. Node-instance predicates
retain closing brackets inside quoted key and leaf-list values. Explicit edits
are authorized, while configuration removed implicitly by `choice` or `when`
evaluation is deliberately excluded from separate write authorization as RFC
8341 requires; the complete resulting delta still reaches transaction backends.

Actions and data-associated notifications bind complete keyed ancestor paths to
the current operational view. Notification authorization consequently applies
to the concrete publishing instance before replay or live delivery. The host's
claimed notification identity must also match the modeled XML event root and,
for associated events, the notification at the end of the instance path. Event
content is schema checked before NACM grants the event type and its complete
body as one authorization unit. Action
requests are authorized before their parent is resolved, so denied users cannot
use the existence error to probe datastore contents.

Remaining gaps are tracked in `TODO.md`: sustained fuzzing and independent
interoperability testing. SSH uses explicit public-key authorization; TLS
supports CN, DNS SAN, and URI SAN selection plus exact local-account mapping.
Transport external
groups require explicit trusted provenance and bounded, unique values. Every
recovery-user RPC attempt emits a privacy-minimal host audit record, and only
unique canonical UTF-8 identities can receive recovery privilege. Recovery and
disabled-enforcement bypasses do not bypass XML document-shape, syntax, or
resource limits; readable-data filtering requires one `<data>` envelope.
NETCONF and NMDA namespaces and the internal unqualified form are accepted;
arbitrary model namespaces are not treated as envelopes.
The XML-injection audit now has a shared strict parser at the hello, RPC,
configuration/edit, NACM, filter, notification, URL-provider, plugin-output,
and snapshot-import boundaries. It rejects invalid UTF-8/XML characters,
embedded NULs, DTD/entity declarations, excess resources, and unexpected
multiple roots. Internal reparses, namespace rebinding, predicate quoting, and
output construction/escaping are inventoried in
[XML_SECURITY.md](XML_SECURITY.md); the code-level XML-injection audit is
complete. Sustained fuzzing and independent interoperability remain release
evidence rather than implementation gaps.
When a runtime schema is supplied, unmodeled data is pruned even for recovery
users or disabled NACM; only authorization decisions are bypassed. Keyed list
identity is derived only from declared keys, and incomplete or ambiguous entries
are removed before instance-specific matching. List or leaf-list identities
containing both XPath quote forms also fail closed as unrepresentable.
Live datastore mutations
now publish their durable snapshot before success is returned and compensate a
failed save by restoring the prior snapshot and live backend/NACM state.
Until the remaining work closes, the project must not describe NACM as fully
compliant.

The section-indexed implemented and open evidence is maintained in the
[RFC 8341 NACM compliance matrix](NACM_COMPLIANCE_MATRIX.md).

### RFC 8342 — NMDA and RFC 8526 — NETCONF NMDA operations

Status: **partial**.

The server publishes conventional datastores through RFC 8525, exposes a
read-only `intended` view equal to `running`, builds an `operational` view from
applied configuration plus core/plugin state, and implements RFC 8526
`get-data` and `edit-data` with datastore selection, filters, `config-filter`,
`max-depth`, NACM, and atomic editing. The origin feature, `with-origin`, and
positive/negative origin selection are enabled for the currently known
`ietf-origin:intended` configuration.

Both NMDA RPC inputs are validated against the enabled YANG operation schema
before execution, so unknown data nodes, duplicate singleton parameters,
invalid scalar values, and missing mandatory inputs fail without changing a
datastore. The NMDA `with-defaults` feature and the separate
`:with-operational-defaults` capability are not advertised; a `with-defaults`
parameter on `<get-data>` is therefore rejected with `invalid-value` as RFC
8526 requires.

Applied configuration now comes from dangd's backend working snapshot after
successful hardware application, rather than being copied from the committed
running tree. Each schema-known configuration node is explicitly annotated as
`intended` when requested, provider-supplied origin metadata is preserved, and
config-false state is not annotated. Failed hardware compensation is retained
as modeled per-action/path remnant state until a successful later hardware
transaction. Per-node transformed, rejected, and delayed states are not yet
represented. Origins other than `intended` are not derived from data sources,
so filtering cannot yet select such per-node data. Filtering of applied
`intended` data is namespace- and identity-derivation-aware, including repeated
and negated selections, while preserving config-false state and required
structure. Operational plugin fragments now receive typed partial-instance
validation, including schema binding, scalar shapes and types, list keys,
choices, references, and intra-fragment duplicates. Cross-provider validation
cumulatively rejects a fragment when it introduces a deterministic conflict
with daemon-owned operational data or an earlier provider. Core data is
authoritative and earlier plugin load order takes precedence. Constraints that
refer to complete applied configuration are evaluated with that backend
snapshot as context, including required state-to-configuration leafrefs.
Explicit `unique` violations across provider-published list entries reject the
later provider deterministically. Cross-provider `must` and `when` expressions
also reject the later fragment when their visible operands decide the result.
Constraints that remain indeterminate between selected state fragments are
still incomplete. ABI v5 providers may assert complete child collections for
their returned nodes; omitted mandatory children and unresolved sibling
leafrefs then become deterministic validation failures. Mandatory enforcement
is also covered when one ABI-v1 plugin owns the model and an independent ABI-v5
plugin publishes the complete instance, with failure attributed to the
publisher. Older providers remain selected-data sources for compatibility.
Completeness of an ABI-v5 top-level subtree is retained across subsequent
provider merges. A later state-to-state leafref therefore resolves against an
earlier target provider, or is rejected and attributed to the later provider
when the closed target list proves that the requested instance is absent.
Required instance-identifiers use the same cumulative semantics: paths can
resolve into an earlier provider's state, while a path into an ABI-v5-complete
top-level subtree is rejected when its selected instance is absent. New
instance-identifier fixtures use ABI v5 exclusively; compatibility with older
plugin ABIs is not a release constraint.
XPath paths now consult explicit collection coverage. Empty selections beneath
an earlier ABI-v5-complete subtree are known empty, so later `must` and `when`
constraints evaluate normally and reject false results; absent nodes in open
collections remain indeterminate.
Callback, validation, and merge failures omit the provider fragment and are
attributed by provider, stage, instance path, and reason in modeled operational
reconciliation telemetry. `<get>` and operational `<get-data>` additionally
fail atomically with `operation-failed`, the
`operational-provider-failure` application tag, the best available error path,
and a message naming the provider, stage, and reason. No otherwise accepted
partial operational payload accompanies that error.
Dynamic configuration datastores are not implemented. The complete operation,
filter/default/origin/NACM interaction matrix and external interoperability
remain. `TODO.md` is the normative work list.

### RFC 8525 — YANG Library

Status: **substantial, final NMDA audit pending**.

The live library publishes implemented and import-only modules, features,
submodules, deviations, datastore schemas, a content identifier, update
notifications, source retrieval locations, and the deprecated RFC 7895
`/modules-state` compatibility view. Plugin load/reload updates the inventory.

Remaining work is to prove that every advertised datastore schema remains
accurate as operational providers, features, deviations, plugins, and future
dynamic datastores change, and to complete external interoperability testing.

## Example management models

### RFC 8343 — interface management and RFC 8344 — IP management

Status: **schema support and partial Linux/FreeBSD device implementation**.

The pinned normative modules compile, validate configuration, appear in YANG
Library, and are owned by the ABI-v4 IP-management example. The example emits
fine-grained reversible actions to the common hardware planner, which applies
address work before activation and publishes running only after all actions
succeed. It prints English apply/rollback actions and, on Linux or FreeBSD,
reconciles enabled state and IPv4/IPv6 addresses using the host administration
utility without invoking a shell. It derives a limited legacy
`/interfaces-state` tree from the last applied configuration.

It does not create/delete interfaces or inspect live addresses, neighbors, MTUs,
counters, duplicate-address detection, or link state. Kernel drift and changes
made by other agents are not reconciled, and command-level compensation is best
effort. It therefore does not claim operational compliance with RFC 8343 or RFC
8344.

The transaction machinery is implementation safety behavior, not an RFC 8343
or RFC 8344 compliance claim. Dynamic capacity rejection, dependency cycles,
activation/deactivation ordering, partial failure, successful compensation, and
incomplete compensation with explicit `hardware-state-diverged` reporting are
covered by automated tests. A real device plugin must still reserve platform
resources during preflight and provide backend-specific dependency edges.

## Imported typedef and reference RFCs

RFC 6991 (`ietf-inet-types` and `ietf-yang-types`) is directly used for schema
types. Its lexical value spaces are handled through the YANG type system, but
the project still needs a type-by-type conformance matrix against every typedef
and pattern in the imported modules.

The following RFCs occur only as normative/informative references inside the
imported IETF type and management modules or their revision histories:
RFC 1034, RFC 1123, RFC 1930, RFC 2119, RFC 2460, RFC 2474, RFC 2578, RFC 2579,
RFC 2780, RFC 2782, RFC 2856, RFC 3289, RFC 3305, RFC 3595, RFC 3986, RFC 4001,
RFC 4007, RFC 4122, RFC 4271, RFC 4291, RFC 4340, RFC 4502, RFC 4741, RFC 4960,
RFC 5017, RFC 5890, RFC 5952, RFC 6021, RFC 6536, RFC 6793, RFC 7895, and
RFC 8174. Except for the explicit RFC 7895 compatibility view noted above,
their appearance does not mean that `dang` or `dangd` implements the referenced
network protocol. They constrain particular typedef meanings, historical
revisions, or standards terminology only.

RFC 3339 is additionally used by the RFC 5277 notification implementation for
event, replay, start, and stop timestamps. Time-zone offsets and fractional
seconds are tested; broader calendar/time library conformance is outside the
project's protocol claim.

## Compliance release gate

A standard may be described as **compliant** only after all of the following
are true:

1. Every applicable normative requirement is mapped to code and at least one
   positive or negative automated test.
2. Optional features are either implemented and advertised accurately or
   explicitly listed here as unsupported.
3. Resource limits, malformed inputs, concurrency, restart, and failure paths
   have been exercised for the exposed boundary.
4. At least one independent interoperability run has passed where the standard
   defines a wire protocol.
5. `TODO.md`, this ledger, focused guides, model inventory, and changelogs agree
   with the tested implementation.
6. A clean build, full applicable test suite, install/export checks, and
   sanitizer/fuzzer release gates pass.

Until that gate is met, use the status labels in this document and describe the
exact supported subset rather than making an unqualified compliance claim.
