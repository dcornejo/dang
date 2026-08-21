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

Status: **substantial core, transport integration required**.

Implemented behavior includes hello capabilities, XML RPC dispatch, running,
candidate and startup datastores, locks, edit operations and options,
commit/discard, confirmed commit and rollback, copy/delete configuration,
session cleanup, `close-session`, `kill-session`, subtree and XPath filters,
schema-aware errors, and host-supplied URL access.

Known boundaries and optional omissions:

- SSH authentication and socket/event-loop ownership are outside the core and
  must be supplied by the host through the authenticated transport boundary.
- The `:url` capability is advertised only for explicitly configured schemes;
  `dangd` does not currently enable it for RFC 8526 `edit-data`.
- Complete live interoperability matrices over SSH and TLS remain release work.
- The server implements only capabilities it advertises; unconfigured optional
  facilities are omissions, not protocol variances.

### RFC 6242 — NETCONF over SSH and message framing

Status: **framing substantial; SSH is an integration boundary**.

The incremental base 1.0 end-marker and base 1.1 chunked framers, hello
negotiation, bounds, malformed-frame shutdown, exact `netconf` subsystem check,
and session-loss cleanup are implemented. The host must provide the SSH stack,
host-key policy, authentication, channel/subsystem establishment, decrypted
byte stream, and event loop. End-to-end testing against independent SSH
implementations remains.

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
schema before policy construction and in-memory publication. Explicit edits are
authorized, while configuration removed implicitly by `choice` or `when`
evaluation is deliberately excluded from separate write authorization as RFC
8341 requires; the complete resulting delta still reaches transaction backends.

Actions and data-associated notifications bind complete keyed ancestor paths to
the current operational view. Notification authorization consequently applies
to the concrete publishing instance before replay or live delivery. Action
requests are authorized before their parent is resolved, so denied users cannot
use the existence error to probe datastore contents.

Remaining gaps are tracked in `TODO.md`: a clause-indexed decision matrix,
recoverable policy/datastore persistence on snapshot failure, production
identity/group hardening, fuzzing, and independent interoperability testing.
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

Known gaps: applied state is still approximated by the committed running tree;
there is no per-node accepted/transformed/rejected/remnant state. Origins other
than inherited `intended` are not populated, identity derivation is not used by
origin filtering, and operational plugin fragments receive structural name
binding rather than complete instance validation and collision arbitration.
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

Status: **schema support and example plugin; not a real device implementation**.

The pinned normative modules compile, validate configuration, appear in YANG
Library, and are owned by the ABI-v3 IP-management example. The example prints
English apply/rollback actions, assumes platform success, and derives a limited
legacy `/interfaces-state` tree from the last applied configuration.

It does not program or inspect host interfaces, addresses, neighbors, MTUs,
counters, duplicate-address detection, or link state. It therefore demonstrates
model and plugin integration but does not claim operational compliance with RFC
8343 or RFC 8344. The plugin source comments describe a practical rtnetlink
implementation path.

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
