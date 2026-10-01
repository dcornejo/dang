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

- [ ] Complete the top-priority, separately packaged FRR provider. Its initial
  Linux/FreeBSD transaction evidence covers `frr-routing`, `frr-zebra`, and
  `frr-staticd`; portable configuration plumbing also covers live
  `frr-interface` and `frr-vrf` parents. The external plugin now publishes the
  runtime-matched native import-and-submodule schema closure, uses the
  programmatic `mgmtd` frontend protocol, exports ABI v8, claims the ABI-v7
  `routing` resource domain, validates in disposable sessions, commits
  atomically, restores the before-image on rollback, and publishes live
  owned top-level and augmented protocol operational XML through native mgmtd
  `GET_DATA`. It reads the managed running roots back after apply for ABI-v6
  applied-state reconciliation and compares later running reads to detect
  out-of-band changes during operational retrieval, while a read-only watcher
  emits deduplicated ABI-v8 drift events through host schema and NACM checks.
  Live parent roots participate in the same transaction so interface-level
  zebra, RIP, and IS-IS configuration cannot escape commit or rollback. The
  public native mgmtd RPC codec and generic `frr-zebra` dispatch are wired
  and covered by portable correlated-session tests. An active FRR 10.5.1 zebra
  validation backend registers configuration and operational paths but no
  `/frr-zebra` RPC path, and rejects `get-vrf-info` as unimplemented. Re-run
  successful live RPC interoperability when an FRR backend registers the
  modeled RPC subtree. FRR 10.7.1 ships the complete `frr-bgp` source family,
  but its live RFC 8525 library omits `frr-bgp` and running bgpd registers no
  mgmtd backend; add BGP only after upstream exposes a native configuration and
  operational path. The native notification codec now builds
  bounded `NOTIFY_SELECT` requests and validates modeled XML `NOTIFY` frames;
  the transport/session layer supports one-way selection, safe idle waits,
  resumed unsolicited reads, and session attribution. The plugin-owned reader
  now conditionally loads, selects, bounds, and forwards `frr-isisd`/`frr-ripd`
  events only when FRR's live library implements the module. Live Linux RIP
  validation found and fixed exact-selector and separate-root ownership gaps,
  then reached FRR 10.7.1's `assure_notify_msg_cache()` assertion after ripd
  emitted the modeled event. Re-run successful Linux/FreeBSD delivery after
  upstream mgmtd can encode top-level notifications. The runtime gate now
  supports BFD, EIGRP, OSPFv2, Pathd, PIM, RIP, RIPng, IS-IS, and VRRP model
  ownership, standalone roots, parent augments, operational reads, and modeled
  RPC dispatch. A read-only Linux/FreeBSD inventory now starts each installed
  optional daemon with mgmtd and zebra in a disposable pathspace and reports
  actual live module registration without creating interfaces, addresses, or
  routes; it passes independently on both Linux and both FreeBSD validation
  hosts. YANG Library advertisement does not prove backend ownership: the BFD
  behavioral test on all four hosts found that mgmtd accepts but drops a profile
  because `bfdd` registers no backend. Post-commit readback now rejects this
  silent no-op and leaves rollback available. Re-test BFD when upstream exposes
  its backend. RIP and RIPng interface-free instance configuration, committed
  readback, basic instance visibility through native operational `GET_DATA`,
  and exact rollback now pass on Linux and FreeBSD. Two-peer native testing on
  the sterile private LAN also proves RIP and RIPng neighbor discovery and a
  learned `/32` or `/128` route in each platform's operational tree. Their
  `clear-rip-route` and `clear-ripng-route` RPCs remove and then relearn those
  routes with either platform acting as the clear endpoint. Extend learned-state
  and RPC evidence to each remaining applicable protocol,
  complete notification behavior, then exercise every newly advertised daemon
  end to end on both platforms without attaching production or management
  interfaces.
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
  each native base-nexthop path without emitting invalid empty nexthops;
  weights and nexthop-object-ID expansion remain absent. Complete the remaining
  kernel route-kind and RFC attribute fidelity, then advertise and test the
  module end to end.
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
  versioned `--peer-recovery` file now validates stable participant-to-endpoint
  and trust mappings at startup and reload. Startup and reload now use those
  mappings to resume durable pending confirmations and proceed only after the
  journal is complete. Implement the remaining prepare,
  apply, verify, cancel, and release transport operations, add provider-specific
  candidate translation and health verification,
  a total transaction deadline beyond the implemented per-I/O timeouts,
  fail-closed degraded-peer policy, NACM/observability, `dangctl` integration,
  and Linux/FreeBSD multi-host evidence before advertising pair-wide atomicity.
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
