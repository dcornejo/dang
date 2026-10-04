<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Project TODO

This file contains only unfinished work. Add new tasks in dependency order and
remove each task after its implementation, tests, documentation, clean build,
and install/export checks pass. The commit that completes an item must remove
or narrow it here, update the corresponding status and variance in
[`docs/COMPLIANCE.md`](docs/COMPLIANCE.md), update focused documentation and
changelogs, and cite the verification performed. A partially completed item
stays in this file with its remaining work rewritten precisely.

## Active tasks

### Standards and models

- [ ] Complete RFC 8431 after the external ABI-v8 plugin wired its strict
  portable route parser, delta planner, and transaction-safe native execution
  into one reversible `routing` action and native Linux/FreeBSD route observation:
  interface-only nexthop families are now durably established by `rib-add`;
  managed and externally observed `route-change` delivery plus reusable
  nexthop resolution transitions are implemented through ABI v8; explicit
  family-aware modeled-name mappings now cover dual-stack Linux tables and
  FreeBSD FIBs without duplicate RFC list keys,
  including direct observation of every mapped FreeBSD FIB. Durable empty RIB
  registrations now remain visible in operational state before their first
  route or nexthop and after restart. Linux ECMP observations now preserve
  each native base-nexthop path without emitting invalid empty nexthops,
  including recursive expansion of simple and grouped kernel nexthop-object
  IDs through `RTM_GETNEXTHOP`; native weights remain absent until the optional
  load-balance feature is implemented end to end. Object-backed routes remain
  operational/read-only because the base model view cannot preserve the object
  ID and group topology required for exact rollback. Configured routes and
  imperative RPCs now reject `local-only=true` before mutation instead of
  misrepresenting an ordinary forwarding route as a kernel-owned receive path;
  real native local routes remain observable read-only state. Complete the
  remaining kernel route-kind and RFC attribute fidelity, then advertise and
  test the module end to end.
- [ ] Implement and document a separately packaged RFC 9249 NTP plugin using
  the pinned `ietf-ntp@2022-07-05` module. Cover configuration and NMDA
  operational state for NTPv4 and the model's NTPv3 compatibility, including
  supported association modes, authentication, access rules, interfaces, VRF
  binding, clock state, association state, and statistics. Prefer native
  programmatic daemon APIs, validate on Linux and FreeBSD, and define an
  explicit resource-ownership and migration policy against the RFC 7317
  `system` plugin so the two packages cannot concurrently manage the same NTP
  service or configuration files. Record daemon-specific unsupported features
  and standards variances in the compliance ledger.
- [ ] Complete RFC 9642 beyond the implemented central cleartext symmetric-key
  slice: add asymmetric key-pair verification, genuinely hidden and encrypted
  key representations, encryption and zeroization beyond the private snapshot
  boundary, built-in operational keys, CSR/certificate behavior, and
  independent behavioral interoperability evidence.
- [ ] Implement and document the RFC 9067 routing-policy model, with conformance
  and interoperability tests and any variance recorded in
  `docs/COMPLIANCE.md`.
- [ ] Add support for the OpenConfig VLAN draft models, pinning the exact model
  revisions and documenting deviations, platform behavior, and Linux/FreeBSD
  validation requirements.

### Datastore architecture

- [ ] Integrate the transport-neutral peer transaction coordinator into a
  production pair-wide commit path. The core state machine now prepares every
  peer before mutation, applies standbys before the primary, verifies all
  participants, records a durable commit decision before confirmation, rolls
  back pre-decision failures in reverse order, and resumes lost confirmations
  without contradicting that decision. A bounded programmatic mutual-TLS
  client now performs authenticated persistent-commit confirmation with
  hostname, capability, namespace, and reply-correlation checks. A private
  versioned `--peer-recovery` file now validates exact generic
  group-and-participant-to-endpoint and trust mappings at startup and reload,
  with unambiguous host-owned journal identities across multiple groups.
  Startup and reload now use those
  mappings to resume durable pending confirmations and proceed only after the
  journal is complete. The stateful mutual-TLS participant now maps prepare to
  candidate lock, complete replacement, and validation; maps apply to a
  persistent confirmed commit; supplies authenticated running and operational
  readback to a health callback; and implements confirmation, reconnecting
  idempotent cancellation, unlock, and close. Live two-peer commit and live
  rollback tests cover the complete adapter. The external Kea provider now
  supplies a strict verifier for authenticated running and operational
  replies, including full managed-image comparison, configured HA identity
  binding, stable states, exact scopes, reachability, interruption, and
  freshness. That provider now also supplies complete two-member hot-standby
  module images and routes authenticated readback through its opaque verifier
  context. ABI v9 carries those transport-neutral contributions through
  supervised workers, and the core composes several plugins' non-overlapping
  images with full-schema validation during normal backend preparation,
  aborting before mutation on any planning failure. Every composed participant
  must now resolve to its exact core-owned authenticated endpoint during the
  same preflight; missing mappings abort all plugin state before mutation.
  The generic production controller now converts one group into authenticated
  TLS participants, routes readback to the retained plugin verifiers, creates
  cryptographically random persistent tokens and the crash-safe journal, and
  invokes the tested state machine. A repeatable host-owned peer-controller
  identity now marks authenticated participant operations as already
  coordinated, suppresses only nested peer discovery, and durably preserves
  that context for confirmed-commit rollback. The generic backend lifecycle
  now separates successful replacement from post-persistence finalization and
  aborts retained work before persistence compensation. The version-2 peer
  journal now records PREPARED before network mutation and startup can safely
  resume either cancellation or COMMIT confirmation. The generic datastore
  contract now brackets backend finalization with a version-2 snapshot recovery
  marker containing an opaque kind, transaction identity, and proposal digest;
  it retains that marker if finalization or the durable clear fails and refuses
  automatic backend activation until the host resolves it. Startup now loads
  the validated snapshot first, requires exact marker/journal identity and
  digest agreement, advances matching PREPARED state to COMMIT, confirms it,
  durably clears the marker, and rejects mismatched or orphaned records. Normal
  preparation now rejects any transaction affecting more than one peer group
  before endpoint resolution or plugin mutation, providing an explicit safe
  limit until an atomic multi-group journal is designed. The coordinator and
  controller now expose explicit prepare, commit, and abort stages: verified
  remote confirmed commits can remain journaled as PREPARED while the local
  datastore crosses its durable snapshot boundary. The generic backend now
  invokes that staged API for ordinary NETCONF commits, publishes the exact
  marker before finalization, cancels on local apply or persistence failure,
  and retains PREPARED for restart if selecting COMMIT fails after local
  durability. A shared configurable monotonic deadline now bounds all forward
  stages and each TLS wait while leaving rollback and recovery free to finish.
  Hostname resolution and every address attempt now share that connection
  budget, with a fail-closed cap on uncancellable platform resolver workers.
  The generic `dangd-peer-transactions` operational model now reports
  lifecycle, counters, and per-participant progress without private transaction
  material, and protects the whole subtree with default-deny NACM. Add
  fail-closed degraded-peer policy, `dangctl` integration, packaging, and
  Linux/FreeBSD multi-host evidence before advertising pair-wide atomicity.
  See [`docs/PEER_TRANSACTIONS.md`](docs/PEER_TRANSACTIONS.md).

- [ ] Define client-visible handling for a standards-compliant NETCONF result
  that exceeds the configured reply ceiling. Internal provider paging protects
  backends but still produces one logical `<rpc-reply>`; evaluate filtering,
  explicit failure, and a separately advertised pagination extension without
  silently truncating standards-defined replies or weakening NACM.
- [ ] Evaluate BaseX as a deferred configuration-datastore backend behind the
  existing persistent-state transaction seam. Compare its transaction,
  concurrency, validation, query, durability, backup/restore, access-control,
  operational complexity, packaging, and Linux/FreeBSD behavior with the
  current store before deciding whether to prototype or adopt it. Keep YANG
  validation, NACM, candidate/running/startup semantics, commit ordering, and
  plugin rollback in dangd rather than delegating policy to the database.

### Low priority

- [ ] Evaluate one or more FD.io VPP plugins. Define which YANG modules and
  resources VPP would own, use VPP's supported programmatic APIs, determine
  whether routing, interface, ACL, and other domains belong in one transaction
  provider or separately packaged plugins, and validate on isolated Linux
  systems without attaching host LAN interfaces. VPP is Linux-only, so this
  work is explicitly exempt from the normal FreeBSD plugin requirement. The
  initial architecture separates `hardware-interface-ownership` from ordinary
  VPP configuration, defaults to an empty PCI allowlist, and requires trusted
  management-path denial plus an independent recovery watchdog. The external
  repository now has shell-free Linux inventory, a validation-only ownership
  model, and a fail-closed evaluator for exact PCI, MAC, vendor/device, default
  route, and live SSH evidence. Read-only validation identified `ens18`/PCI
  `0000:06:12.0` as protected on both Linux hosts; only `ens19`/PCI
  `0000:06:13.0` is a future private-LAN candidate. The generated C++ VAPI
  adapter and reversible loopback transaction pass failure-injected tests and a
  live plugin-free VPP 26.06 lifecycle on both hosts, using isolated unpacked
  packages because FD.io has no Ubuntu 26.04 repository. Expose the ownership
  and software-interface configuration through a loadable provider. The
  external ABI-v7 provider now embeds both models, claims distinct resource
  domains, resolves live loopbacks after restart, and applies its ordered plan
  as one compensated hardware action; provider-level create/enable/rollback,
  complete live loopback publication, and applied-state reconciliation pass on
  both Linux hosts. Add bridge/bond/VLAN-parent/required-route evidence and a
  recovery watchdog, then
  design and test the reversible physical ownership transition.
- [ ] Design and implement transparent Berkeley-socket compatibility for
  applications using VPP-owned networking. Use VCL's `vppcom` session API as
  the underlying mechanism, but keep VPP connection setup, application
  namespaces, worker registration, session lifecycle, and descriptor mapping
  behind a dangd/provider abstraction so NETCONF users and ordinary plugin
  configuration do not need VPP-specific socket knowledge. Define explicit
  opt-in and fail-closed fallback rules: dangd's SSH, TLS, NETCONF, recovery,
  and other management sockets must remain on the protected host stack, and a
  missing or unhealthy VPP session must never silently redirect them. Cover
  blocking and nonblocking I/O, `poll`/`select`/`epoll`, threads and process
  lifecycle, error translation, restart/reconciliation, observability, package
  integration, and rollback. Validate both direct `vppcom` use and any POSIX
  compatibility/interposition layer against isolated VPP loopbacks before
  permitting private-LAN traffic.

### Deferred — lowest priority

- [ ] Complete the separately packaged FRR-native provider only when BGP can
  be supported as a first-class protocol. The existing provider retains its
  tested transaction, rollback, reconciliation, drift, operational, RPC,
  notification, schema-inventory, RIP, and RIPng work, but it is not considered
  a complete or supported FRR offering without native BGP configuration and
  operational state through the contracted programmatic `mgmtd` interface.
  Installed `frr-bgp` YANG sources are insufficient: a running bgpd must expose
  its implemented schema and usable backend in the live RFC 8525 library and
  mgmtd. Once upstream provides that boundary, add the complete BGP schema
  closure, owned and augmented roots, atomic apply/readback/rollback, state,
  RPC and notification handling, restart/drift behavior, packaging, and real
  IPv4/IPv6 peer and route-policy tests on both Linux and FreeBSD. Do not
  substitute CLI execution or claim partial FRR product support. Revisit other
  upstream-gated daemon and notification gaps as part of that deferred effort;
  current detailed evidence and variances remain in `docs/COMPLIANCE.md` and
  the external provider guide.
