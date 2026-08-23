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

- Extend the backend-applied configuration foundation into a per-node state
  model. `<operational>` now takes configuration from the backend's accepted
  working snapshot rather than echoing `<running>`. Add plugin/backend reports
  for transformed, rejected, delayed, and remnant nodes; merge those reports
  deterministically and expose actionable reconciliation state without
  publishing unapplied intent as device state.
- Generalize origin metadata from the current inherited
  `ietf-origin:intended` value to per-node origins. Support `intended`,
  `default`, `system`, `learned`, `dynamic`, `unknown`, and derived identities;
  obey the non-presence-container inheritance rules and preserve plugin origins
  without fabricating metadata for config-false nodes.
- Implement RFC 8526 origin filtering over identity derivation, not literal
  string recognition. Correctly handle repeated positive or repeated negated
  filters, reject their simultaneous use through model validation, and retain
  ancestors and list keys needed to encode selected descendants.
- Validate ABI-v3 operational fragments as complete typed instance data before
  publication: namespace and schema binding, scalar types, list keys,
  `mandatory`, `when`, `must`, `leafref`, `instance-identifier`, `unique`, and
  duplicate/collision handling across providers. Define deterministic provider
  precedence or reject conflicting values, and surface callback failures as
  actionable NETCONF errors and telemetry.
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
