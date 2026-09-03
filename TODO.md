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

### Configuration lifecycle and privileged recovery

- [ ] Complete the configuration save/restore workflow. The current
  `--state FILE` implementation atomically persists and validates conventional
  NETCONF datastores and confirmed-commit recovery state; finish the
  administrator-facing backup/restore lifecycle, permissions and recovery
  guidance, and restored-plugin hydration so the complete device configuration
  is available before requests are served.
- [ ] Detect and load the initial configuration at startup, including delivery
  of the restored configuration to affected plugins before serving requests.
- [ ] Create and ship a default super-user identity exclusively for dangd
  privileged access. It must not authenticate to, authorize, or provision any
  other operating-system or application service, and its bootstrap, rotation,
  recovery, audit, and removal behavior must be documented and tested.

### Standards and models

- [ ] Implement and document RFC 9644 YANG SSH client/server groupings, with
  conformance and interoperability tests and any variance recorded in
  `docs/COMPLIANCE.md`.
- [ ] Implement and document the RFC 9642 keystore model, with conformance and
  interoperability tests and any variance recorded in `docs/COMPLIANCE.md`.
- [ ] Implement and document the RFC 8431 RIB model, with conformance and
  interoperability tests and any variance recorded in `docs/COMPLIANCE.md`.
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
