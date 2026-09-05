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

- [ ] Complete the Kea DHCPv4/DHCPv6 provider beyond its transaction-safe
  configuration scope. The translator now covers every list and leaf-list in
  the pinned configuration models, Kea-specific JSON names, decimal values,
  and modeled JSON-valued leaves, with live isolated validation, apply, and
  rollback on Linux and FreeBSD. ABI-v3 selected operational publication now
  translates leases, host reservations, and supplemental per-subnet lease
  statistics from both native daemons and is exercised live on Linux and
  FreeBSD. Lease and host enumeration now use their respective bounded native
  cursors. Split statistics into bounded subnet ranges, verify the complete
  state schema, and move to ABI v5 with truthful subtree completeness before
  claiming complete module support.
- [ ] Complete the top-priority, separately packaged FRR provider after its
  initial Linux/FreeBSD `frr-routing`, `frr-zebra`, and `frr-staticd`
  configuration implementation. The external plugin now publishes the
  runtime-matched native schema closure, uses the programmatic `mgmtd` frontend
  protocol, claims ABI-v7 `routing`, validates in disposable sessions, commits
  atomically, restores the before-image on rollback, and publishes live
  owned top-level and augmented zebra operational XML through native mgmtd
  `GET_DATA`. It reads the managed running roots back after apply for ABI-v6
  applied-state reconciliation and compares later running reads to detect
  out-of-band changes during operational retrieval, while a read-only watcher
  emits deduplicated ABI-v8 drift events through host schema and NACM checks.
  The public native mgmtd RPC codec and generic `frr-zebra` dispatch are wired
  and covered by portable correlated-session tests; complete live RPC
  interoperability on a validation host with an active zebra backend. Add
  notifications from later FRR protocol modules (the current routing, zebra,
  and staticd model set declares none), resolve the
  eventual package conflict with an executable IETF RIB provider, and then
  enable each additional
  FRR protocol daemon without attaching host LAN interfaces.
- [ ] Complete RFC 8431 after the external plugin's schema, strict portable
  route parser, delta planner, and transaction-safe native execution: wire the
  executor into the plugin ABI, implement all seven RPCs and both notifications,
  publish observed operational state, resolve FreeBSD interface-only nexthops,
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
  work is explicitly exempt from the normal FreeBSD plugin requirement.
