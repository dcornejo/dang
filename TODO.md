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

- Close every open evidence item in
  [`docs/NACM_COMPLIANCE_MATRIX.md`](docs/NACM_COMPLIANCE_MATRIX.md), especially
  the remaining data-write denial vectors and the protocol-level
  operation-to-CRUDX matrix. Keep each vector indexed to its RFC 8341 section.
  Namespace-correct
  RPC denial paths, single-RPC policy snapshots, structural loader constraints,
  leaf-list uniqueness/name restrictions, and empty lexical values are covered.
  The managed loader rejects unmodeled attributes and non-whitespace character
  content in structural containers while allowing namespace declarations.
  Every prohibited pairwise and three-way `rule-type` selector combination is
  covered, and selector choice validation is based on leaf presence.
  Global first-match ordering across rule-lists when a user belongs to several
  configured groups is covered.
  Every supported standard operation has protocol-level denial evidence, with
  namespace-correct error paths, no error-info, and the `close-session`
  exception verified.
  Datastore-source copy filtering and the running-to-startup execute-only
  exception are also covered. URL/datastore copy combinations are covered;
  remote source-read, target-inspection, and atomic write failures preserve
  provider errors and target content; independent provider authorization
  remains host policy. Data-write denial paths now use namespace-correct XPath,
  including keyed instances. Effective
  `<edit-config>` and RFC 8526 `<edit-data>` create/update/delete mapping is
  covered, as are ordered-by-user moves and instance-specific leaf-list read,
  create, and delete authorization. Virtual defaults are excluded from write
  deltas while explicitly stored default-valued nodes require create/delete
  access. Confirmed-commit cancellation and timeout rollback under a changed
  policy are covered.
- Make managed NACM policy replacement and durable datastore persistence one
  recoverable transaction, retaining or restoring the last known-good policy
  and running configuration when snapshot persistence fails. Schema validation,
  policy construction, and in-memory backend publication are already staged
  atomically before the live policy is replaced.
- Harden authentication-to-NACM identity plumbing for production transports:
  define canonical username handling, certificate/SSH name mapping, trusted
  external-group provenance, recovery-session auditing, and fail-closed
  behavior when identity mapping is absent or ambiguous. Add multi-session
  identity-mapping tests. Concurrent policy replacement and per-RPC snapshots
  are covered in the core server.
- Run interoperability and negative-security tests against at least one
  independent RFC 8341 implementation, fuzz NACM XML and instance-identifier
  paths, and document any intentional deviations before claiming compliance.

### RFC 8342 / RFC 8526 NMDA compliance closure

- Replace the current whole-running-tree approximation with an applied-state
  model. Track, per configuration node, whether intended configuration was
  accepted, transformed, rejected, delayed, or remains as remnant
  configuration. Build `<operational>` from what the device is actually using,
  not merely from the latest committed `<running>` tree.
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

### Backend transaction ordering

- Design and implement a transactional hardware application planner for
  `dangd`: preflight dynamic platform limits, derive generic and backend-specific
  action dependencies, enforce activation-last/deactivation-first ordering,
  apply the resulting graph with rollback, and publish the running datastore
  only after the hardware transaction succeeds. Include failure-injection tests
  for unsafe ordering, exhausted resources, partial application, successful
  rollback, and rollback failure with explicit state-divergence reporting.
