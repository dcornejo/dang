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

- Extend the backend-applied configuration foundation to transformed, rejected,
  and delayed per-node states. `<operational>` takes configuration from the
  backend's accepted working snapshot, and failed rollback actions are now
  retained by action/path under modeled reconciliation state until a successful
  hardware transaction. Add plugin/backend outcome reports for the remaining
  states and merge them deterministically without publishing unapplied intent
  as device state.
- Derive non-intended per-node origin metadata from actual data sources. Applied
  configuration nodes are now explicitly marked `ietf-origin:intended`,
  provider-supplied origins are preserved, and config-false nodes are not
  annotated. Add sources for `default`, `system`, `learned`, `dynamic`,
  `unknown`, and derived identities; validate those identities and optionally
  compact redundant annotations using the RFC 8342 inheritance rules.
- Extend origin filtering from the current identity-aware applied-`intended`
  selection to the non-intended per-node origins as those data sources are
  added. Runtime identity derivation, namespace validation, repeated positive
  and negated values, conflicting-choice rejection, and preservation of state
  nodes and required configuration structure are implemented.
- Complete ABI-v3 operational publication validation across provider and
  datastore context. Individual fragments are now namespace/schema bound and
  checked as typed partial instance data, including shapes, scalar types, list
  keys, choices, references, and duplicates visible within the fragment.
  Cumulative validation now rejects a provider whose fragment conflicts with
  daemon-owned operational data or already accepted provider data; core data
  is authoritative and earlier plugin load order has deterministic precedence.
  Applied configuration is now supplied as complete context, so provider
  leafrefs to config-true targets are enforced after the standalone collision
  gate. Complete cross-fragment `mandatory`, `when`, `must`, state-to-state
  `leafref`, and `instance-identifier` checks whose outcome remains
  indeterminate under selected-data coverage. Explicit cross-provider `unique`
  violations are enforced with deterministic later-provider rejection.
  Callback and validation failures now omit the unsafe fragment and appear with
  provider, stage, path, and reason in operational reconciliation telemetry;
  additionally surface them as actionable NETCONF errors where RFC operation
  semantics permit rather than returning partial data without an error.
- Define explicit schemas and lifecycle rules for any dynamic configuration
  datastores. Publish each datastore and its schema through RFC 8525, define
  supported protocol operations, validation and persistence semantics, and map
  its applied content and derived origin identity into `<operational>`.
- Finish the RFC 8526 operation matrix: exercise all datastore identities,
  subtree and XPath choices, `config-filter`, `max-depth`, `with-defaults`,
  `with-origin`, URL-feature behavior if enabled, all `edit-data` default/test/
  error options, locks, and protocol-accurate error tags. Basic NACM effective
  create, update, delete, wrong-bit denial, and atomic preservation are covered;
  add cases where filtering, defaults, origin selection, depth limiting, and
  NACM interact.
- Verify that every advertised RFC 8525 datastore schema is accurate and that
  `<operational>` is a permitted superset of every configuration-datastore
  schema. Add reload tests for module, feature, deviation, plugin, datastore,
  and content changes, including update notifications and built-in source
  retrieval.
- Run an external NMDA interoperability suite and long-running concurrency,
  malformed-provider, provider-timeout, and resource-limit tests. Document
  unsupported optional features and deviations before making an RFC 8342 or
  RFC 8526 compliance claim.
