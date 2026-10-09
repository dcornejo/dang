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
  changes reach the generic tracker, but the values remain absent from XML and
  configuration until the optional load-balance feature is implemented end to
  end. Object-backed routes remain
  operational/read-only because the base model view cannot preserve the object
  ID and group topology required for exact rollback. Configured routes and
  imperative RPCs now reject `local-only=true` before mutation instead of
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
  Complete the remaining kernel route-kind and RFC attribute fidelity, then
  advertise and test the module end to end.
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
