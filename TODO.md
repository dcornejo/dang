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

- [ ] Complete the top-priority, separately packaged FRR provider after its
  initial Linux/FreeBSD `frr-routing`, `frr-zebra`, and `frr-staticd`
  configuration implementation. The external plugin now publishes the
  runtime-matched native import-and-submodule schema closure, uses the
  programmatic `mgmtd` frontend
  protocol, claims ABI-v7 `routing`, validates in disposable sessions, commits
  atomically, restores the before-image on rollback, and publishes live
  owned top-level and augmented zebra operational XML through native mgmtd
  `GET_DATA`. It reads the managed running roots back after apply for ABI-v6
  applied-state reconciliation and compares later running reads to detect
  out-of-band changes during operational retrieval, while a read-only watcher
  emits deduplicated ABI-v8 drift events through host schema and NACM checks.
  The public native mgmtd RPC codec and generic `frr-zebra` dispatch are wired
  and covered by portable correlated-session tests. An active FRR 10.5.1 zebra
  validation backend registers configuration and operational paths but no
  `/frr-zebra` RPC path, and rejects `get-vrf-info` as unimplemented. Re-run
  successful live RPC interoperability when an FRR backend registers the
  modeled RPC subtree. FRR 10.7.1 ships the complete `frr-bgp` source family,
  but its live RFC 8525 library omits `frr-bgp` and running bgpd registers no
  mgmtd backend; add BGP only after upstream exposes a native configuration and
  operational path. Add
  notifications from FRR protocol modules, resolve the
  eventual package conflict with an executable IETF RIB provider, and then
  enable each additional
  FRR protocol daemon without attaching host LAN interfaces.
- [ ] Complete RFC 8431 after the external ABI-v7 plugin wired its strict
  portable route parser, delta planner, and transaction-safe native execution
  into one reversible `routing` action and native Linux/FreeBSD route observation:
  persist the reusable-nexthop registry, provide an explicit family source for
  interface-only entries without an observed containing RIB, implement both
  notifications, resolve
  FreeBSD interface-only nexthops,
  replace numeric-only RIB names with an explicit platform mapping, then
  advertise and test the module end to end.
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

- [ ] Audit existing plugins for operating-system work performed by spawning
  command-line programs. Replace it with stable programmatic library, daemon,
  socket, or kernel APIs wherever available, preserving transaction rollback,
  error attribution, and Linux/FreeBSD behavior. In particular, prototype and
  evaluate netlink route and interface operations on both Linux and FreeBSD;
  document any operation for which a command remains unavoidable and test its
  strict argv-only execution. New plugin code must prefer programmatic APIs
  from the outset.
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
  external repository now includes a stable-instance loopback model and ordered
  snapshot planner (create before activation, deactivate before deletion), but
  these operations still need ABI-v7 hardware-action wiring. Then add
  bridge/bond/VLAN-parent/required-route evidence and a recovery watchdog, then
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
