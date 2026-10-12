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
  including direct observation of every FreeBSD FIB. Durable empty RIB
  registrations now remain visible in operational state before their first
  route or nexthop and after restart. Linux ECMP observations now preserve
  each native base-nexthop path without emitting invalid empty nexthops,
  including recursive expansion of simple and grouped kernel nexthop-object
  IDs through `RTM_GETNEXTHOP`. Native Linux and FreeBSD weights are now
  preserved exactly in the internal observation contract and weight-only
  changes reach the generic tracker. The portable parser now accepts the
  feature-enabled `nexthop-lb` structure, validates and canonicalizes its
  reusable members and 1-through-99 weights, and retains every member binding
  for the datastore route. Exact Linux and FreeBSD native execution plus
  compensated plugin rollback are implemented and covered by isolated live
  tests. Weighted operational XML now preserves one modeled route, exact
  weights, and durable managed member identities; external native groups use
  deterministic snapshot-local identities. The plugin advertises the optional
  feature after direct schema and native round-trip validation. Object-backed
  routes remain operational/read-only because the base model view cannot
  preserve the object ID and group topology required for exact rollback.
  Ordinary applied routes now recover their configured `route-index` after
  native readback through the generic reconciliation callback; resolved
  reusable routes also publish their modeled nexthop ID, sharing policy, and
  expanded definition. Unmanaged external routes retain deterministic
  synthetic indexes. Native notification polling now uses that same modeled
  projection, so managed indexes remain stable and one representable ECMP
  route produces one route-level change rather than per-path synthetic events.
  Configured routes and imperative RPCs now reject `local-only=true` before mutation instead of
  misrepresenting an ordinary forwarding route as a kernel-owned receive path;
  real native local routes remain observable read-only state. Direct `discard`
  and `discard-with-error` nexthops are configurable, observable, and
  reversible on both native backends, including durable reusable `nh-add`
  objects and exact `nexthop-ref` resolution, while `receive` remains
  kernel-owned. `route-add` now inventories the live RIB, returns RFC error
  code 1 for repeated destinations, uses exclusive native creation, and
  rejects modeled destination-key collisions before they can overwrite or
  collapse routes. Native nexthop installed-state transitions now preserve the
  exact RFC `resolved-nexthop` and `unresolved-nexthop` reasons in operational
  state and notifications without inventing reasons for unrelated changes.
  FreeBSD route preference now round-trips through its distinct native metric
  rather than being conflated with an ECMP path weight.
  The supported slice now passes one real candidate lock/edit/validate/commit,
  operational read, delete/commit, unlock, and close session through dangd and
  supervised interface/RIB plugin workers on isolated Linux and FreeBSD
  kernels. That transaction configures an RFC 8343/8344 interface and an RFC
  8431 gateway route whose outgoing-interface leafref targets it, exercising
  generic two-plugin validation and apply ordering. Unsupported instance
  membership, router ID, lookup limit, enabled RPF, and datastore-created
  nexthop IDs fail closed rather than being ignored. Attribute-selected
  `route-update` now applies one portable replacement to every exact
  `route-preference`/`local-only` match in the named RIB with the same per-route
  rollback semantics as prefix selection. Exact portable base-nexthop and
  complete weighted load-balance-set selection are also complete. The optional
  vendor-attribute selector is intentionally not advertised because RFC 8431's
  grouping is empty; a future augmented model must define its semantics first.
  Concrete base route attributes are complete: preference round-trips, and
  local-only is observed but unsafe to configure. Augmented attribute content
  now fails closed. Define explicit cross-platform capability designs before
  adding the remaining non-portable source, MPLS, MAC, interface-match, chain,
  replication, protection, tunnel, RIB-lookup, or MAC-nexthop forms. The native
  end-to-end matrix covers every implemented imperative RPC, including
  duplicate-route reporting and the safe FreeBSD whole-FIB refusal.
- [ ] Implement and document a separately packaged RFC 9249 NTP plugin using
  the pinned `ietf-ntp@2022-07-05` module. The authoritative schema and import
  closure, direct interoperability test, operator migration procedure, generic
  exclusive `ntp` resource-domain policy, and RFC 7317 provider/package claim
  are complete. The side-effect-free parser and dependency-ordered transaction
  plan now cover listener, reference-clock, modern symmetric authentication,
  ACL binding, and unicast server/peer configuration. A development-only ABI-v7
  plugin now embeds the model closure, advertises only the portable parser-
  backed features, claims the `ntp` resource domain, exposes ordered generic
  hardware actions, and fails closed at apply, rollback, and operational
  publication. A typed adapter contract now retains per-action native
  before-image/acknowledgement checkpoints and selects only chrony on Linux or
  base ntpd on FreeBSD; invalid choices fail during prepare. Linux now has a
  side-effect-free chrony translator for the supported configuration and key
  images, with daemon-specific semantic gaps rejected during validation. The
  adapters are still non-mutating and the plugin is intentionally not installed
  or packaged. A tested POSIX per-file replacement/rollback primitive now
  preserves bytes and metadata, detects external edits and retains compensation
  state after post-rename sync failure. The chrony pair now has ordered
  installation/removal, automatic compensation and retryable rollback failure
  state. A service-bracket coordinator now tests acknowledged stop before
  changes, activation failure compensation, initial service-state restoration
  and retry after recovery failure through an injected service boundary.
  Next implement its native systemd D-Bus boundary and ABI action wiring for
  acknowledgement, rollback, and readback on Linux; then implement the FreeBSD
  translation and lifecycle before enabling production installation.
  Cover configuration and NMDA
  operational state for NTPv4 and the model's NTPv3 compatibility, including
  supported association modes, authentication, access rules, interfaces,
  clock state, association state, statistics, and the statistics-reset RPC.
  RFC 9249 has no VRF node; select and pin a separate augmentation before
  claiming VRF binding. Prefer native programmatic daemon APIs and record
  daemon-specific unsupported features and standards variances in the
  compliance ledger.
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

- [ ] Extend the generic peer-transaction journal from one affected group to
  an atomic decision covering several independent groups. The current
  production path rejects such proposals with `peer-group-limit` before any
  endpoint resolution or mutation.
- [ ] Support safe initiation when the local participant is a standby. The
  implemented production ordering supports a locally owned primary: every
  remote standby reaches verified PREPARED before local apply. A local standby
  currently fails explicitly with `peer-local-standby-unsupported` rather than
  applying the standby before an unreachable remote primary.

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
