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

- Resolve SSH server integration with `dangd`. Select and document whether SSH
  is embedded in `dangd`, supplied by a supervised sidecar, or connected through
  a stable host adapter; do not leave two components responsible for session
  authentication or NETCONF framing. Require host-key and user authentication
  before constructing `TransportIdentity`, accept only the exact `netconf`
  subsystem, pass the authenticated username through the shared exact mapper,
  mark external groups trusted only when supplied by the authenticated SSH
  authorization source, and propagate disconnect, cancellation, timeout,
  lock-release, notification, and confirmed-commit lifecycle events. Add
  negative tests for unauthenticated peers, wrong subsystems, mapping failures,
  spoofed groups, duplicate session IDs, abrupt disconnects, and backpressure,
  plus multi-session NACM and independent RFC 6242 interoperability coverage.
- Perform an end-to-end XML injection audit across every untrusted XML input and
  generated XML/XPath output. Inventory parser entry points and parse flags;
  verify fail-closed, resource-bounded handling of DTD and external entities,
  entity expansion, XInclude, multiple roots, namespace rebinding, embedded
  NULs, malformed UTF-8, CDATA, comments, and processing instructions. Test
  XPath predicate quoting and XML escaping for configurations, RPC errors,
  notifications, YANG Library, and model retrieval. Add focused regressions and
  coverage-guided fuzz seeds for every issue found, and document any parser
  behavior that is intentionally accepted. NETCONF `<hello>` and `<rpc>`
  transport boundaries and complete datastore parsing now reject multiple
  top-level document elements; intentional edit fragments remain a separately
  defined input shape.
- Run interoperability and negative-security tests against at least one
  independent RFC 8341 implementation, run a sustained coverage-guided NACM
  fuzz campaign, and document any intentional deviations before claiming
  compliance. The deterministic sanitizer smoke target mutates NACM XML and
  attacker-controlled keyed/leaf-list instance paths through policy loading,
  CRUD authorization, and read filtering. Schema-aware read filtering rejects
  unmodeled elements instead of applying annotation-free default access.

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
