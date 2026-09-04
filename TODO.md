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

- [ ] Complete RFC 9642 beyond the implemented central cleartext symmetric-key
  slice: add asymmetric key-pair verification, genuinely hidden and encrypted
  key representations, encryption and zeroization beyond the private snapshot
  boundary, built-in operational keys, CSR/certificate behavior, and
  independent behavioral interoperability evidence.
- [ ] Complete RFC 8431 after the external plugin's schema, strict portable
  route parser, delta planner, and transaction-safe native execution: wire the
  executor into the plugin ABI, implement all seven RPCs and both notifications,
  publish observed operational state, resolve FreeBSD interface-only nexthops,
  replace numeric-only RIB names with an explicit platform mapping, then
  advertise and test the module end to end.
- [ ] Implement an FRR-backed routing plugin for Linux and FreeBSD without
  attaching tests to host LAN interfaces. Define its exact model surface and
  use a supported FRR management interface rather than parsing interactive CLI
  output. Before advertising any module also define delegation or strict
  mutual exclusion with the native RFC 8431 provider: packages should declare
  the conflict, while dangd must independently reject duplicate runtime module
  ownership.
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
