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
  runtime-matched native schema closure, uses the programmatic `mgmtd` frontend
  protocol, claims ABI-v7 `routing`, validates in disposable sessions, commits
  atomically, restores the before-image on rollback, and publishes live
  owned top-level and augmented zebra operational XML through native mgmtd
  `GET_DATA`. It reads the managed running roots back after apply for ABI-v6
  applied-state reconciliation and compares later running reads to detect
  out-of-band changes during operational retrieval. Use the now-available ABI-v8
  schema-validating, NACM-enforcing event path to add unsolicited drift
  notification; add RPCs and native notifications, resolve the
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

- [ ] Evaluate BaseX as a configuration datastore. Compare its transaction,
  concurrency, validation, query, durability, backup/restore, access-control,
  operational complexity, packaging, and Linux/FreeBSD behavior with the
  current store before deciding whether to prototype or adopt it.

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
