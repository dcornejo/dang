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

### RFC 8343/8344 native IP management closure

- Replace the initial Linux `/sbin/ip` and FreeBSD `/sbin/ifconfig` adapters
  with direct kernel APIs, publish live link/address/neighbor state, reconcile
  external drift, cover MTU and neighbor configuration, and test privileged
  apply plus partial-failure compensation on both operating systems.

### RFC 8341 NACM compliance closure

- Run interoperability and negative-security tests against at least one
  independent RFC 8341 implementation, run a sustained coverage-guided NACM
  fuzz campaign, and document any intentional deviations before claiming
  compliance. Include a broader independent OpenSSH RFC 6242 matrix and
  sustained concurrent SSH-session/backpressure coverage; a single OpenSSH
  public-key/subsystem/RPC/close smoke interaction has passed. The deterministic
  sanitizer smoke target mutates NACM XML and
  attacker-controlled keyed/leaf-list instance paths through policy loading,
  CRUD authorization, and read filtering. Schema-aware read filtering rejects
  unmodeled elements instead of applying annotation-free default access.

### RFC 8342 / RFC 8526 NMDA compliance closure

- Finish the RFC 8526 operation matrix: exercise all datastore identities,
  subtree and XPath choices, `config-filter`, `max-depth`, `with-defaults`,
  `with-origin`, URL-feature behavior if enabled, and protocol-accurate error
  tags. All three `<edit-data>` `default-operation` values, mandatory rollback
  on validation failure, and RFC 8526 datastore-identity lock/unlock targets
  are covered; read-only lock targets return `invalid-value`. (`test-option`
  and `error-option` belong to `<edit-config>`, not RFC 8526 `<edit-data>`.)
  `<get-data>` and
  `<edit-data>` inputs are now checked against their enabled YANG schema before
  execution, including unknown nodes, duplicate singleton parameters, scalar
  types, and mandatory elements; unsupported NMDA `with-defaults` is rejected
  with the RFC-required `invalid-value`. Basic NACM effective
  create, update, delete, wrong-bit denial, and atomic preservation are covered;
  operational subtree selection combined with `config-filter`, `with-origin`,
  `max-depth`, and NACM read denial is covered without denied-data leakage.
  XPath selections now apply `max-depth` relative to every selected node while
  retaining otherwise uncounted ancestor paths; nested subtree selections use
  the same selection-relative rule. Add the remaining origin selection,
  defaults, and non-operational cross-product cases.
- Verify that every advertised RFC 8525 datastore schema is accurate and that
  `<operational>` is a permitted superset of every configuration-datastore
  schema. Add reload tests for module, feature, deviation, plugin, datastore,
  and content changes, including update notifications and built-in source
  retrieval.
- Run an external NMDA interoperability suite and long-running concurrency,
  malformed-provider, provider-timeout, and resource-limit tests. Document
  unsupported optional features and deviations before making an RFC 8342 or
  RFC 8526 compliance claim.
