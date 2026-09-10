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

Configured persistence atomically saves and schema-validates the conventional
datastores and confirmed-commit recovery state. POSIX snapshots are private
regular files owned by the effective service user; symlinks and group/other
access fail closed. Initial and restored running configuration is activated
through affected plugins before the application is made available to a
transport; activation failure makes startup fail closed.

### RFC 6242 — NETCONF over SSH and message framing

Status: **substantial with an embedded `dangd` SSH host**.

The incremental base 1.0 end-marker and base 1.1 chunked framers, hello
negotiation, bounds, malformed-frame shutdown, exact `netconf` subsystem check,
and session-loss cleanup are implemented. `dangd` uses libssh to own its listen
socket, host private key, public-key-only user authentication, session channel,
and subsystem establishment. Authenticated usernames use the common exact
mapper, while NACM external groups come only from the matching local authorized
key record. An automated independent OpenSSH matrix covers unauthorized-key and
wrong-subsystem rejection plus four concurrently initiated public-key,
subsystem, RPC-framing, and clean-close interactions. Independent session
workers are admission-bounded and a slow-reader interaction pipelines 512
replies beyond its unread SSH window while four other sessions complete. A
20-iteration sustained run passed 10,240 backpressured replies and 80
independent sessions without cross-session blocking.

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

Status: **implemented; project compliance evidence complete**.

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

The independent sysrepo 3.7.11 NACM decision comparison and final intentional-
deviation review are complete. The comparison covered positive and negative
group, default, data-read, data-update, and RPC-execution decisions using the
real sysrepo datastore boundary. No intentional RFC 8341 semantic variance was
identified. An external Python ncclient 0.6.17 session then proved that one
RFC 5277 subscriber receives the permitted current YANG Library event while
the paired legacy event is suppressed and counted once. SSH uses explicit
public-key authorization; TLS supports CN, DNS SAN, and URI SAN selection plus
exact local-account mapping.
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
complete. A focused Clang/libFuzzer ASan/UBSan campaign completed 79,611,946
NACM inputs without a finding. The reusable sysrepo comparison is in
`tests/interoperability`; broader client/version matrices remain release
evidence rather than implementation gaps.

Dangd includes the `dangd-superuser` recovery identity by default. It is only a
name in dangd's trusted NACM recovery set: no operating-system account,
password, private key, certificate, or authorization for another service is
created. An administrator must bind a transport credential to that exact name.
Every RPC attempt is recovery-audited, and `--no-default-superuser` removes the
identity on startup or reload after an alternate recovery route is established.
`DangdApplicationTest.ProvidesRemovableDangdOnlySuperuser` covers privilege,
audit, and removal; duplicate explicit registration still fails closed.

When a runtime schema is supplied, unmodeled data is pruned even for recovery
users or disabled NACM; only authorization decisions are bypassed. Keyed list
identity is derived only from declared keys, and incomplete or ambiguous entries
are removed before instance-specific matching. List or leaf-list identities
containing both XPath quote forms also fail closed as unrepresentable.
Live datastore mutations
now publish their durable snapshot before success is returned and compensate a
failed save by restoring the prior snapshot and live backend/NACM state.
The RFC 8341 section matrix, independent decision comparison, negative cases,
notification transport evidence, and intentional-deviation review are complete.

The section-indexed implementation and interoperability evidence is maintained
in the [RFC 8341 NACM compliance matrix](NACM_COMPLIANCE_MATRIX.md).

### RFC 8342 — NMDA and RFC 8526 — NETCONF NMDA operations

Status: **conformant for the advertised conventional-datastore feature set;
optional features listed below are not advertised**.

The server publishes conventional datastores through RFC 8525, exposes a
read-only `intended` view equal to `running`, builds an `operational` view from
applied configuration plus core/plugin state, and implements RFC 8526
`get-data` and `edit-data` with datastore selection, filters, `config-filter`,
`max-depth`, NACM, and atomic editing. The origin feature, `with-origin`, and
positive/negative origin selection are enabled for intended, standard
non-intended, and schema-derived origin identities supplied by applied-state
sources.

Both NMDA RPC inputs are validated against the enabled YANG operation schema
before execution, so unknown data nodes, duplicate singleton parameters,
invalid scalar values, and missing mandatory inputs fail without changing a
datastore. The NMDA `with-defaults` feature and the separate
`:with-operational-defaults` capability are not advertised; a `with-defaults`
parameter on `<get-data>` is therefore rejected with `invalid-value` as RFC
8526 requires.
`<edit-data>` implements `merge`, `replace`, and `none` default operations and
restores the complete pre-edit datastore on error. RFC 8526 datastore identity
leaves are accepted as `<lock>` and `<unlock>` targets for supported writable
datastores; competing sessions receive `lock-denied`, while `intended` and
`operational` targets receive `invalid-value`.
Combined retrieval coverage applies NACM read filtering, configuration-only
selection, intended-origin annotation, subtree selection, and maximum depth in
one operational request. It verifies that denied leaves and unrelated state do
not leak, permitted ancestors and leaves retain origin metadata, and children
beyond the requested depth are removed. XPath depth limiting is independently
applied from every selected node rather than from the reply wrapper, and keeps
the selected nodes' ancestor paths without counting those ancestors against
the limit. Nested subtree selections follow the same rule: containment and
content-match ancestors identify the terminal selection without consuming its
depth allowance. Conventional running, candidate, startup, and intended reads
exercise subtree and XPath selection, configuration/state filtering, and depth
limiting. Origin options on those non-operational datastores and unknown
datastore identities return `invalid-value`; unsupported operational-defaults
input has the same RFC-required error behavior.

Applied configuration now comes from dangd's backend working snapshot after
successful hardware application, rather than being copied from the committed
running tree. Each schema-known configuration node is explicitly annotated as
`intended` when requested, provider-supplied origin metadata is preserved, and
config-false state is not annotated. Failed hardware compensation is retained
as modeled per-action/path remnant state until a successful later hardware
transaction. ABI-v6 plugins report complete post-apply snapshots and unique
per-node applied, transformed, rejected, and delayed outcomes. Reports are
composed in dependency order and schema-validated after every plugin; invalid
or ambiguous reports fail closed and are compensated. The accepted snapshot,
rather than unapplied intent, is published with modeled reconciliation
telemetry. ABI-v6 applied XML retains validated origin metadata supplied by
actual backend sources, including standard and derived identities. Positive
and negated filters apply inheritance and identity derivation to both intended
and non-intended configuration while preserving config-false state and
required structure. Metadata is stripped unless `with-origin` is requested.
Operational plugin fragments now receive typed partial-instance
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
Constraints that remain indeterminate between selected state fragments retain
selected-data semantics: omission is not treated as absence. ABI v5 providers
assert complete child collections when absence must be decisive, allowing
omitted children of their returned nodes to be decisive; mandatory children
and unresolved sibling leafrefs then become deterministic validation failures.
Mandatory enforcement
is also covered when one ABI-v1 plugin owns the model and an independent ABI-v5
plugin publishes the complete instance, with failure attributed to the
publisher. Older providers remain selected-data sources; this is an explicit
plugin contract rather than a bypass of a deterministic validation failure.
Completeness of an ABI-v5 top-level subtree is retained across subsequent
provider merges by canonical instance path. A complete keyed list entry does
not close another provider's partial entry of the same list. A later
state-to-state leafref therefore resolves against an
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
Dynamic configuration datastores are not implemented or advertised. If one is
introduced, its schema, protocol operations, validation, persistence, YANG
Library entry, applied mapping, and derived origin identity are release-gated
requirements rather than behavior inferred from conventional datastores. The
internal operation, filter/default/origin/NACM interaction matrix is covered.

The deliberately unsupported optional surface is:

- dynamic configuration datastores;
- the RFC 8526 `with-defaults` feature and NETCONF
  `:with-operational-defaults` capability; and
- URL content for `<edit-data>`, because the imported NETCONF `url` feature is
  not enabled.

These are omissions from the advertised feature set, not variances in enabled
behavior. `intended` being identical to `running` is the RFC 8342 conventional
datastore model for a device without configuration transformations. No known
semantic variance remains in the advertised RFC 8342/RFC 8526 behavior.

Independent protocol evidence uses ncclient 0.6.17 over production mutual TLS.
On Ubuntu 26.04 LTS it passed running/subtree `<get-data>`, locked candidate
`<edit-data>` and commit, intended visibility, operational intended-origin
filtering and metadata, and the required `invalid-value` response for
`with-origin` on running. The reproducible driver and transcript are in
`tests/interoperability/README.md`.

Operational callback XML is length-bounded before host string construction and
then passes the common XML byte/node/depth parser limits and schema validation.
Malformed or oversized provider output fails the retrieval atomically with
provider/stage attribution. Timeout and crash recovery use a supervised
out-of-process callback boundary. Bounded length-prefixed IPC uses a
monotonic deadline and classifies timeout, peer exit, truncation, oversized
frames, and system errors. The standalone worker exclusively owns plugin loading
and returns only copied values. Parent supervision contains and reaps callback
hangs and crashes under independent deadlines. A failed request is never
replayed; before a later independent request, a replacement worker is accepted
only if its manifest and YANG sources exactly match startup discovery.
The worker now implements distinct prepare and validate requests and an abort
request. Opaque preparation survives between those phases only inside the
worker, so dependency-wide prepare-before-validate semantics can be retained at
cutover. Workers now also copy hardware action descriptions and accept named
apply/rollback requests; legacy plugins receive a synthetic transaction action.
The parent worker coordinator now creates one deterministic hardware plan across
all participating workers, including module dependency edges and reverse
compensation. ABI-v6 reconciliation callbacks now run inside workers and return
bounded copies. The parent now validates schema, ownership, provider identity,
and outcome uniqueness before committing its retained plan, and compensates a
bad report in reverse. RPC/action callbacks use bounded worker requests. The
application layers depend on a common plugin runtime contract, and daemon
startup selects its worker-owned implementation without an in-process fallback.
The worker-owned implementation of that contract is complete and directly
tested for discovery, dependency rejection, operational publication, operation
routing, model compilation, and rejected commit behavior. Daemon startup now
selects it. Crash/timeout restart for subsequent independent requests is tested.
An end-to-end deterministic stress case issues 200 operational retrievals from
eight sessions, verifies every response, and proves that all eight provider
callbacks may execute concurrently. The ASan/UBSan release soak completed at
1,000 requests per worker: 8,000 validated retrievals over 315.7 seconds with
no finding. External interoperability remains release evidence.

### RFC 8525 — YANG Library

Status: **substantial, external interoperability pending**.

The live library publishes implemented and import-only modules, features,
submodules, deviations, datastore schemas, a content identifier, update
notifications, source retrieval locations, and the deprecated RFC 7895
`/modules-state` compatibility view. Plugin load/reload updates the inventory.

All five conventional/NMDA datastores reference the same rebuilt compiled
schema: configuration datastores use its config-true subset and operational may
use the full schema, making it the permitted superset. Atomic reload tests cover
module revision/content changes, deviation and plugin inventory replacement,
content identifiers, current and legacy update notifications, compiled plugin
nodes, and source retrieval. Feature enablement and datastore membership are
fixed inputs to each rebuilt application; dynamic datastores are not
advertised. External interoperability testing remains.

## Example management models

### FRRouting native models

Status: **partial external-provider implementation**.

The separately packaged `dang-frr` provider publishes the runtime-matched
schema closure for `frr-routing`, `frr-zebra`, and `frr-staticd`, plus live
`frr-interface` and `frr-vrf` parent modules, and claims the
exclusive `routing` resource domain, validates changes in disposable mgmtd
candidate sessions, commits them atomically, and restores a retained
before-image on rollback. It retrieves provider-owned top-level and augmented
protocol state from FRR's operational datastore through native `GET_DATA` and
publishes only complete, correlated XML `TREE_DATA` replies through dangd's
operational-provider ABI. When the live library implements the interface and
VRF parent modules, their complete roots are published; the zebra-only augment
filter remains for runtimes that do not implement a parent. Partial results,
unexpected formats, continuation replies, and protocol-correlation failures
fail closed. After apply, ABI-v6 reconciliation reads every managed root from
FRR's running datastore and replaces only those roots in the complete dangd
applied snapshot. This exposes FRR's accepted configuration, including deletion
or normalization, while preserving configuration owned by other providers.

Those reconciled roots are retained as expected state. Subsequent operational
retrievals compare fresh mgmtd running roots semantically, ignoring namespace
prefix spelling but not modeled names, namespaces, attributes, values, or child
order. A mismatch fails the provider at the affected root and is exposed by
dangd's modeled operational-provider failure telemetry. The external provider
also polls these roots read-only after reconciliation and emits one modeled
`configuration-drift` event per affected path until a successful commit resets
the baseline. ABI v8 transports it across worker isolation before trusted-core
schema validation, subscription filtering, and NACM enforcement.

The external provider also implements the strict wire codec for FRR's native
`NOTIFY_SELECT` and modeled XML `NOTIFY` messages. It rejects datastore
synchronization operations, non-XML formats, malformed XPath splits, embedded
NULs, and empty event bodies. This is protocol foundation rather than completed
event delivery. The transport/session layer sends a one-way selection and
receives session-attributed unsolicited events without treating an idle timeout
as a broken stream. A provider-owned reader now loads `frr-isisd` and
`frr-ripd` only when FRR's live module-set implements them, negotiates XML,
selects each modeled event by its exact schema XPath, bounds delivery to 1,024
queued events, and reconnects after mgmtd failure. Dangd retains final schema,
subscription, and
NACM enforcement. The provider includes every enabled standalone protocol root
in the complete transaction, reconciliation, drift, and operational paths;
augment-only modules remain within their managed parent root.

Because zebra, RIP, and IS-IS also augment keyed interface or VRF instances,
the provider publishes live `frr-interface` and `frr-vrf` modules as
implemented and treats their complete `lib` roots as part of the same atomic
boundary. This closes the earlier variance where an advertised protocol's
valid interface-level configuration was outside prepare, apply, rollback,
reconciliation, and drift detection.

The same live-library gate now recognizes BFD, EIGRP, OSPFv2, Pathd, PIM,
RIPng, and VRRP. Standalone daemon roots join the managed root set; augment-only
OSPFv2 and VRRP configuration remains inside the already-owned routing or
interface parent. Operational reads and native RPC dispatch are enabled only
for the corresponding live module. Portable discovery and ownership tests cover
this mechanism, but daemon-by-daemon Linux and FreeBSD interoperability remains
open and is not claimed here.

The external collection includes an opt-in, read-only Linux/FreeBSD schema inventory.
It starts each installed optional protocol daemon with mgmtd and zebra in a
unique disposable pathspace, reads the live RFC 8525 library, and distinguishes
an absent binary, an early daemon exit, a running daemon with no advertised
mgmtd schema, and an advertised schema. It creates no interface, address, or
route. This is
discovery evidence only: per-daemon transaction, rollback, operational, RPC,
and notification interactions remain required.
Independent FRR 10.7.1 runs on both Ubuntu 26.04.1 hosts found all nine daemon
binaries installed but only BFD, RIP, and RIPng registered with mgmtd. The
provider correctly excludes EIGRP, IS-IS, OSPFv2, Pathd, PIM, and VRRP on that
runtime despite their installed model sources and running processes.
Independent FreeBSD 16.0-CURRENT runs found the same three advertised modules
among six installed optional daemons. EIGRP, IS-IS, and OSPFv2 ran without
registering; Pathd, PIM, and VRRP binaries were absent from those packages.

Schema advertisement is not backend-registration evidence. A profile-only BFD
interaction on all four hosts found that FRR advertises `frr-bfdd` but `bfdd`
does not register with mgmtd; mgmtd acknowledges the candidate commit while its
running root remains empty. The provider now verifies every committed root in
a fresh post-unlock session and rejects this silent no-op at
`/frr-bfdd:bfdd`, retaining rollback eligibility. The native test skips only
after the before-image is successfully restored. BFD configuration is therefore
not claimed for these FRR runtimes.

RIP and RIPng provide real backend transaction evidence on all four hosts. In
separate disposable pathspaces, each daemon accepts an interface-free `default`
instance, exposes the committed XML through both the running and operational
datastores, and accepts the retained empty before-image as rollback. The final
root is verified absent. The stronger operational assertion also passes on the
current isolated Debian 13 and FreeBSD 16.0-CURRENT Proxmox guests. This
establishes configuration, basic instance operational visibility, and rollback
without creating an interface, address, neighbor, route, or packet.

A separate guarded two-peer interaction uses the sterile private LAN between
those current guests. Each disposable RIP pathspace advertises one temporary
loopback `/32`, discovers the other guest as a native RIP neighbor, and reports
the peer `/32` as a learned route with protocol `rip`, metric 2, and the correct
gateway through mgmtd operational `GET_DATA`. Both peers remove the temporary
address and daemons on exit. This supplies Linux/FreeBSD learned-route and
neighbor fidelity evidence for RIP. With one peer held as an active advertiser,
the other also invokes `clear-rip-route` without input, proves the learned entry
disappears, and waits for it to be learned again. Reversing roles passes, giving
successful native RPC backend evidence on both platforms rather than accepting
an acknowledged no-op.

The corresponding guarded RIPng interaction also passes in both directions.
Each endpoint receives a temporary ULA address only on the named sterile-LAN
interface and advertises a temporary ULA loopback `/128`. Native operational
data exposes the peer's link-local neighbor address and learned `/128` with
metric 2. The clear side invokes `clear-ripng-route` without input, requires the
entry to disappear, and then requires it to be learned again. Reversing roles
proves this behavior on both Debian 13 and FreeBSD 16.0-CURRENT; all temporary
addresses and isolated daemons are removed on exit. Other applicable protocol
state and RPC behavior, plus notification coverage, remain incomplete.

An isolated Linux RIP interaction confirmed backend registration, modeled
configuration apply, UDP activation, and event generation without a host LAN
interface. FRR 10.7.1 mgmtd rejects its own top-level notification as an
unexpected data element and aborts at `assure_notify_msg_cache()`. The guarded
native test recognizes only that exact upstream assertion as skipped. FreeBSD
strict-warning compilation passes; successful delivery on either platform
remains blocked until upstream mgmtd encodes the event.

At discovery, the provider reads FRR's RFC 8525 YANG Library through mgmtd and
copies daemon-enabled features into the corresponding YANG source descriptors.
It rejects required-module absence and installed-source versus running-daemon
revision or namespace skew. The closure follows `include` and `belongs-to` in
addition to imports, and validates every selected submodule against its nested
RFC 8525 owner and revision. This prevents dangd from compiling an incomplete
model or a feature set that differs from the backend it controls.

Opt-in native Linux and FreeBSD tests now create a disposable mgmtd pathspace,
validate and commit an empty staticd protocol instance, read the accepted
running tree, commit the exact before-image as rollback, and verify restoration.
The fixture starts no forwarding daemon and attaches no host or LAN interface.
This testing exposed and corrected the frontend requirement to lock both
candidate and running datastores for every configuration transaction.

This is conformance to the installed FRR-native model set rather than an IETF
routing-model claim. All fourteen RPCs in the installed `frr-zebra` schema now
use a generic native mgmtd RPC request/reply path after dangd input validation
and NACM authorization; replies return through host output validation and read
filtering. Portable framing, correlation, format, and session behavior are
tested on Linux and FreeBSD. Live RPC interoperability remains unconfirmed
because Linux mgmtd reported no active zebra backend for the RPC path and the
FreeBSD socket refused connections. The RIP notification path reaches the
upstream mgmtd encoding assertion described above; successful delivery and
end-to-end validation of the other conditionally loaded protocols remain open.
RIP's `clear-rip-route` is the first successfully exercised live RPC: native
mgmtd dispatch removes a learned route on both Linux and FreeBSD, and the route
is subsequently relearned from the held peer.
FRR 10.7.1 installs the complete BGP source family, but its live library
omits `frr-bgp` and running bgpd registers no mgmtd backend; the provider does
not advertise that unavailable runtime capability.

The FRR-native and RFC 8431 RIB providers cannot be installed together through
the supported packages. Debian declares symmetric conflicts. FreeBSD packages
share an installed routing-domain ownership marker because CPack's FreeBSD
generator has no conflict-metadata setting, causing `pkg` to reject the file
collision. Dangd's ABI-v7 resource-domain check remains the authoritative
runtime safeguard for manual installations.

### RFC 8343 — interface management and RFC 8344 — IP management

Status: **schema support and partial Linux/FreeBSD device implementation**.

The pinned normative modules are shipped by `dang_plugins`, compile, validate
configuration, appear in YANG Library, and are owned by its ABI-v4
IP-management provider. The provider emits
fine-grained reversible actions to the common hardware planner, which applies
address work before activation and publishes running only after all actions
succeed. It prints English apply/rollback actions. Linux uses direct,
acknowledged rtnetlink operations for enabled state, IPv4/IPv6 addresses,
equal per-family link MTUs, and static neighbors; FreeBSD reconciles enabled
state, equal per-family MTUs, and addresses through native interface ioctls and
static neighbors through acknowledged route-netlink requests. Both backends
publish live RFC 8343 `/interfaces-state` and RFC 8344 IP operational data.
The shared platform parser now retains RFC 8344 IPv4/IPv6 MTUs and static
neighbors, enforces family ranges and keyed uniqueness, and fails closed on
incomplete modeled data. Linux snapshots live flags and MTU and retains the
reverse plan until transaction commit, including reverse-order compensation
after a partial rtnetlink failure.

It does not create/delete interfaces. Linux publishes live link status, MTU,
assigned prefixes, complete ARP/IPv6 neighbor entries, and native packet,
octet, error, and drop counters for all configured and system-controlled kernel
interfaces. Counter discontinuity uses kernel boot time. Address status comes
from rtnetlink flags, including preferred, deprecated, tentative, optimistic,
and duplicate DAD states.
Linux repairs missing or altered configured addresses and neighbors while
preserving unrelated kernel state. FreeBSD publishes live link status, MTU,
assigned prefixes, and complete IPv4/IPv6 neighbor entries for configured
and system-controlled interfaces, plus native packet, octet, error, drop,
multicast, and unknown-protocol counters with interface-epoch discontinuity
time. IPv6 address status comes from `SIOCGIFAFLAG_IN6`, including preferred,
deprecated, tentative, inaccessible, and duplicate DAD states. It repairs
missing or altered managed address and neighbor entries while preserving
unrelated kernel state. Its epair smoke covers address/MTU/IPv4 and IPv6 neighbor apply,
live-state publication, out-of-band removal repair, exact rollback, and forced
partial-failure compensation. It therefore does not claim operational
compliance with RFC 8343 or RFC 8344.

The Linux privileged suite induces and observes a real duplicate on a veth
pair. The FreeBSD privileged suite uses an epair endpoint moved into a temporary
VNET jail to supply the required independent network stack: duplicate IPv6 DAD
was observed on the host endpoint, and the backend published RFC 8344
`duplicate` status from the live `SIOCGIFAFLAG_IN6` result. The ordinary epair
suite also covers preferred-state retrieval, while deterministic tests cover
native flag precedence.

The transaction machinery is implementation safety behavior, not an RFC 8343
or RFC 8344 compliance claim. Dynamic capacity rejection, dependency cycles,
activation/deactivation ordering, partial failure, successful compensation, and
incomplete compensation with explicit `hardware-state-diverged` reporting are
covered by automated tests. A real device plugin must still reserve platform
resources during preflight and provide backend-specific dependency edges.

## RFC 9644 SSH client and server groupings

Status: **published as a complete import-only schema family**.

Dangd bundles the unmodified `ietf-ssh-common`, `ietf-ssh-client`, and
`ietf-ssh-server` revision 2024-10-10 modules and the pinned IANA algorithm and
normative dependency closure. The modules compile with both this project's
compiler and independent libyang `yanglint`, appear as import-only modules in
the RFC 8525 and legacy RFC 7895 inventories, and are available through RFC
6022 `get-schema`. No RFC 9644 feature is advertised.

This is schema/grouping conformance, not a claim that the grouping contents are
writable dangd configuration. RFC 9644 intentionally defines reusable
groupings and omits transport addresses and ports. The embedded libssh listener
continues to use explicit command-line settings. Dangd does not instantiate an
RFC 9644 consuming transport model, and its RFC 9641 truststore dependency
remains import-only. The RFC 9642 keystore is independently instantiated, but
transport host keys are not yet sourced from it.

## RFC 9642 keystore

Status: **partial implementation: central cleartext symmetric keys**.

Dangd implements the `ietf-keystore` revision 2024-10-10 top-level datastore
with `central-keystore-supported` and `symmetric-keys`. The effective
`ietf-crypto-types` schema enables `cleartext-symmetric-keys`. NETCONF clients
can edit, validate, commit, retrieve, and restart with named symmetric keys and
their key-format identities. The standard `default-deny-write` and
`default-deny-all` annotations are enforced: ordinary users cannot change the
keystore and cannot retrieve cleartext key values, while configured recovery
identities retain explicit break-glass access. The implemented module and its
two features are published in RFC 8525/RFC 7895 inventories and its source is
available through RFC 6022 `get-schema`.

Committed key data uses the same atomic, owner-only mode-0600 state snapshot as
the other datastores. This meets RFC 9642's fallback requirement that persisted
cleartext key storage be inaccessible to other users, but it is not encryption
at rest. Backups therefore require the same secret-handling controls.

The asymmetric-key, hidden-key, and encrypted-key features are not advertised
or accepted. Dangd does not yet verify asymmetric key pairs, generate CSRs,
emit certificate-expiration notifications, zeroize key buffers, expose
built-in operational keys, or connect SSH/TLS host credentials to this model.
The imported `ietf-crypto-types` feature used to shape the effective grouping
cannot be listed on an RFC 8525 import-only module entry. Independent schema
interoperability is covered through the bundled unmodified module family;
independent behavioral interoperability remains release-gated in `TODO.md`.

## External vendor models

### ISC Kea DHCPv4 and DHCPv6 3.2.0 models

Status: **implemented for the pinned revisions**.

The separately packaged `dang_plugins` provider implements the complete
configuration and state trees from `kea-dhcp4-server@2026-06-24` and
`kea-dhcp6-server@2026-06-24`. It validates and applies complete native Kea
configurations transactionally, compensates partial failure, and publishes
ABI-v5 complete operational leases, per-subnet statistics, and host
reservations including option data. Lease, host, and statistic collection has
independent page/query, item, byte, and duration bounds. Disposable native
interactions exercise the provider against packaged DHCPv4 and DHCPv6 daemons
on Linux and FreeBSD without exposing a LAN interface. These are ISC vendor
models rather than an IETF RFC compliance claim; support is pinned to the
embedded revisions and Kea 3.2.x control-command behavior.

## Roadmap-only standards and models

The remaining RFC 9642 keystore work, RFC 8431 completion, the RFC 9067
routing-policy model, and OpenConfig VLAN draft models are
planned but not implemented, advertised, or claimed. Exact module revisions,
feature/deviation choices, plugin ownership, platform effects, and
interoperability evidence must be established before their status moves into
an implemented section of this ledger. Their release-gated work is tracked in
`TODO.md`.

RFC 8431 partial runtime support lives in the separate `dang_plugins`
repository. Its ABI-v8 provider advertises the pinned schema, claims exclusive
`routing` ownership, and
pins and independently validates the unmodified schema family. Its runtime
foundation strictly parses destination-prefix IPv4/IPv6 routes with portable
base nexthops, computes delete-before-install replacements, and produces
shell-free Linux and FreeBSD command vectors. Its `posix_spawnp` executor stops
on failure and compensates completed changes in reverse order, retaining any
rollback failures for reconciliation. Opt-in native tests install, observe,
delete, and recheck a documentation-prefix route inside a Linux network
namespace and FreeBSD VNET jail without touching host routes. Unsupported match
and nexthop semantics fail with an attributed model path. Numeric-only RIB/FIB
names remain a temporary platform-mapping variance. FreeBSD interface-only
nexthops now resolve exactly one usable local address in the route family via
`getifaddrs(3)`; unnumbered and multihomed interfaces fail closed at the
modeled nexthop path. The provider publishes partial observed operational data
by reading Linux rtnetlink or FreeBSD `NET_RT_DUMP` directly. It emits IPv4/IPv6
unicast routes with active/installed status and deterministic synthetic indexes;
it does not yet represent every kernel route kind or every RFC 8431 attribute.
The provider implements `route-add` for its portable route subset with
per-member success/failure accounting and optional RFC-shaped failure detail.
`route-delete` resolves prefix requests against live kernel state and deletes
only an unambiguous observed match. Prefix-selected `route-update` replaces a
portable base nexthop or route attributes and restores the observed before-image
when installation fails. `rib-add` validates Linux logical tables or existing
FreeBSD FIBs and rejects unsupported RPF enforcement; `rib-delete` uses a
compensated plan to empty the observed RIB. `nh-add` and `nh-delete` allocate
and remove portable base nexthops in a thread-safe, per-RIB process registry.
The registry now restores its private atomic sidecar at startup and makes
`nh-add`/`nh-delete` durable before acknowledgement; corrupt recovery state and
write failures fail closed, with object mutation rolled back on write failure.
`rib-add` also persists the modeled RIB address family. Interface-only reusable
nexthops consume that explicit context and therefore appear in operational data
before any native route exists; an absent or conflicting family fails as a
modeled operation rather than being inferred from host interface state.
Imperative route additions, deletions, updates, and whole-RIB deletions also
make binding changes durable before acknowledgement. A failed sidecar write
runs the inverse native route plan, restores the registry checkpoint, and
reports any compensation failure. Its identifiers are represented in live
operational RIB state.
Configuration commits, `route-add`, and prefix-selected `route-update` resolve
identifiers through `nexthop-ref` with per-RIB isolation, while reference
lifetime is enforced across datastore prepare, apply, rollback, and release.
The ABI-v6 applied-configuration reconciliation callback rebuilds the exact
datastore-owned reference set, so restored snapshots do not depend on stale
pre-restart counters.
Imperative route add/update operations also retain bindings, and successful
route or RIB deletion releases them durably. The provider now uses ABI v8 to
publish bounded `route-change` events after successful durable imperative
operations and after datastore applied-state reconciliation; tentative or
compensated changes do not produce success events. Notification draining also
compares native route snapshots after a quiet initial baseline, reporting
external additions, removals, and property changes while managed operations
advance the baseline to avoid duplicates. `nexthop-resolution-status-change`,
complete state fidelity, and arbitrary RIB-name mapping remain before a
substantial RFC 8431 claim.

BaseX is likewise only a datastore architecture investigation. The current
implementation remains the in-process validated datastore manager with atomic
JSON snapshots; no BaseX runtime, query, packaging, or compliance dependency
exists today.

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
