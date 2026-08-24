<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes are documented here. The format follows Keep a Changelog,
and releases follow Semantic Versioning.

## [Unreleased]

### Changed

- Refocused the top-level README on the `dangd` server, its primary use cases,
  runnable transport example, and documentation map. Moved detailed compiler,
  library, package-consumer, and API-documentation material into focused guides.
- Started supervised plugin isolation with a bounded, deadline-aware POSIX
  worker protocol that classifies timeout, exit, truncation, and I/O failures.
- Added an installed standalone worker that exclusively loads one plugin and
  exposes copied manifest and YANG source discovery over the bounded protocol.
- Added a parent supervisor and isolated operational command with independent
  startup/callback deadlines, crash/timeout containment, reaping, and
  untrusted-response revalidation.
- Moved transaction prepare, validate, and abort protocol phases into the
  worker, retaining opaque preparation only inside the isolated process.
- Added worker hardware-action discovery, execution, and compensation so the
  parent can build one global plan from copied descriptors without loading a
  plugin or receiving its prepared pointers.
- Added a parent-side worker coordinator that merges local and inter-module
  dependencies into one deterministic hardware plan with reverse compensation.
- Added bounded worker-side ABI-v6 applied-state reporting, copying actual
  configuration and per-node outcomes while retaining opaque transaction state.
- Retained successful worker hardware plans until parent-side applied-state
  validation succeeds, with reverse compensation on report rejection.
- Added supervised worker RPC/action dispatch with bounded copied inputs,
  outputs, and attributed NETCONF errors.
- Decoupled application, backend, operational, and operation-provider wiring
  from the in-process loader through a common plugin runtime contract.
- Added a complete worker-owned plugin runtime with isolated discovery,
  dependency validation, transactions, operational data, and operation routing.
- Switched daemon plugin startup to the worker runtime, with automatic worker
  path resolution, an explicit override, and no silent in-process fallback.
- Extended common RFC 8344 platform intent extraction with per-family MTUs and
  static neighbors, duplicate-key checks, and fail-closed malformed data.
- Replaced the Linux IP-management command adapter with acknowledged direct
  rtnetlink operations for link state, MTU, addresses, and static neighbors.
- Retained observed Linux link state through the transaction decision, with
  reverse-order partial-failure compensation and exact rollback of live MTU
  and flags; added opt-in privileged network-namespace regression tests.
- Published live Linux administrative/operational status, MTU, and assigned
  IPv4/IPv6 prefixes for configured interfaces instead of echoing intended
  values into the deprecated RFC 8343/8344 compatibility state tree.
- Added acknowledged Linux neighbor-table dumps and published complete IPv4
  ARP and IPv6 ND entries with link-layer address, origin, router indication,
  and representable reachability state.
- Replaced the FreeBSD `/sbin/ifconfig` adapter with native interface ioctls
  for flags, MTU, and IPv4/IPv6 addresses, including retained observed state,
  reverse partial-failure compensation, and disposable-epair validation.
- Added acknowledged native FreeBSD route-netlink operations for permanent
  IPv4/IPv6 neighbors, ordered after address creation and before address removal,
  with exact rollback and privileged disposable-epair coverage.
- Published live FreeBSD administrative/operational status, MTU, assigned
  IPv4/IPv6 prefixes, and complete neighbor tables for configured interfaces
  from native kernel APIs, including FreeBSD permanent-neighbor classification.
- Removed the now-unused external command runner from the IP plugin.
- Added deterministic multi-session NMDA stress coverage that drives 200
  operational retrievals through eight simultaneously active plugin callbacks.
- Made the per-session NMDA stress duration safely configurable for
  sanitizer-backed release soaks and documented the direct invocation.
- Completed the NMDA concurrency release soak with 8,000 validated retrievals
  over eight sessions and 315.7 seconds under ASan/UBSan without findings.
- Bounded operational callback XML before copying it into host memory, rejecting
  payloads above the common XML limit with an attributed atomic NETCONF error;
  added a real oversized-provider regression fixture.
- Completed the RFC 8525 datastore-schema and reload audit: verified all five
  advertised datastores reference the rebuilt compiled schema, and covered
  deviation/plugin inventory changes, content IDs, both update notifications,
  runtime schema availability, and source retrieval after atomic reload.
- Completed the internal RFC 8526 retrieval matrix across running, candidate,
  startup, intended, and operational: added conventional-datastore subtree,
  XPath, configuration/state, depth, origin-error, and unknown-identity cases.
- Closed operational-publication validation by documenting selected versus
  complete provider semantics as an explicit ABI contract, and made dynamic
  configuration datastores an explicit non-advertised scope boundary with
  release gates for any future implementation.
- Scoped operational-provider completeness to canonical instance paths rather
  than top-level schema names, preventing one complete keyed list entry from
  making a different provider's partial entry falsely fail mandatory checks.
- Preserved schema-validated RFC 8342 origin metadata from ABI-v6 applied-state
  sources and generalized origin selection to inherited `default`, `system`,
  `learned`, `dynamic`, `unknown`, and derived identities. Ordinary `<get>` and
  unannotated `<get-data>` replies suppress the internal metadata.
- Added plugin ABI v6 applied-state reconciliation: dependency-ordered plugins
  may return a schema-validated actual configuration plus unique per-path
  applied, transformed, rejected, or delayed outcomes. Operational reads now
  publish that accepted snapshot and modeled outcome telemetry, never replaced
  requested intent; invalid reports fail the commit and trigger compensation.
- Applied `max-depth` from each terminal nested subtree-filter selection,
  retaining containment ancestors without incorrectly consuming the selected
  subtree's depth allowance.
- Applied RFC 8526 `max-depth` relative to each XPath-selected node instead of
  the reply wrapper, retaining required ancestors without charging them against
  the selected subtree's depth allowance.
- Added combined RFC 8526 retrieval coverage for NACM read denial,
  configuration filtering, origin annotation, subtree selection, and maximum
  depth, verifying denied and out-of-scope data remain absent.
- Accepted RFC 8526 datastore identity leaves for `<lock>` and `<unlock>`, and
  returned `invalid-value` for read-only NMDA targets as required; added full
  `<edit-data>` default-operation, rollback, and lock interaction coverage.
- Validated RFC 8526 `<get-data>` and `<edit-data>` inputs against their enabled
  YANG operation schema, rejecting unknown and duplicate parameters before any
  mutation and returning `invalid-value` for unsupported `with-defaults`.
- Made `<get>` and operational `<get-data>` fail atomically when an operational
  provider callback, validation, or merge fails, returning structured NETCONF
  errors with provider, stage, path, and reason instead of partial data.
- Made XPath empty-node selections honor explicit collection coverage, allowing
  cross-provider `must` and `when` checks to decide absence in closed state.
- Made required instance-identifiers decisive when they select an absent node
  inside an operational subtree previously declared complete.
- Preserved ABI-v5 operational subtree completeness across cumulative provider
  merges, making cross-provider state leafrefs reject targets proven absent.
- Added plugin ABI v5 operational completeness assertions, allowing providers
  to make omitted children decisive for mandatory and nested leafref checks.
- Enforced cross-provider `must` and `when` constraints once cumulative
  operational state contains the expressions' operands, blaming the later provider.
- Verified cumulative `unique` constraints across independently valid ABI-v3
  provider list entries, rejecting and attributing the conflicting later provider.
- Validated cumulative operational-provider state against the backend's
  complete applied configuration, enforcing required leafrefs to config data.
- Seeded operational-provider arbitration with daemon-owned state so built-in
  operational trees remain authoritative over plugins.
- Arbitrated ABI-v3 operational publication in deterministic plugin load order
  by validating each cumulative provider snapshot, retaining earlier accepted
  data and reporting a conflicting later fragment as a merge-stage failure.
- Attributed ABI-v3 callback and instance-validation failures by provider,
  stage, path, and reason in modeled operational reconciliation telemetry while
  continuing to omit the unsafe fragment.
- Validated each ABI-v3 operational fragment as typed partial instance data,
  rejecting unknown nodes, malformed shapes and values, missing list keys,
  choice/reference violations, and visible duplicates before publication.
- Lowered identity inheritance into the common runtime schema and made RFC 8526
  origin selection namespace- and derivation-aware, including repeated and
  negated filters, base-identity matches, and explicit invalid-value errors.
- Retained incomplete hardware rollback actions as modeled operational
  reconciliation records containing action ID, instance path, and failure
  reason; published the model through YANG Library and clear stale records only
  after a completely successful later hardware transaction.
- Annotated every schema-known applied configuration node with its explicit
  NMDA `intended` origin, preserved provider-supplied origin metadata, and kept
  config-false operational state free of fabricated configuration origins.
- Based NMDA `<operational>` configuration content on the backend's accepted
  working snapshot instead of the server's running-tree input, establishing the
  applied-state boundary needed for later transformation and remnant reporting.
- Added a transactional hardware action planner and plugin ABI v4. Plugins can
  publish reversible actions with paths, safety classes, and dependencies;
  dangd preflights all affected plugins, applies a deterministic dependency
  graph with deactivation first and activation last, rolls back partial work,
  reports incomplete rollback as explicit hardware-state divergence, and only
  then publishes running. The RFC 8344 example now uses the action interface.
- Added an embedded public-key-only libssh host to `dangd`, with exact subsystem
  enforcement, authenticated identity mapping, trusted local group provenance,
  lifecycle cleanup, reload support, test keys, live negative coverage, and an
  independent OpenSSH smoke interaction.
- Reconciled the NACM TODO with its compliance matrix and added an explicit
  end-to-end XML injection audit and regression-testing gate.

### Fixed

- Used complete child-collection coverage when deciding whether a missing
  operational leafref target is invalid rather than merely indeterminate.
- Included config-false nodes in the common effective data view during
  operational validation so XPath constraints can inspect published state.
- Preserved QName namespace declarations when serializing configuration trees,
  allowing identityrefs and instance-identifiers to survive snapshot round trips.
- Included config-false schema nodes in collection-cardinality validation when
  validating operational instance data, closing duplicate state-node gaps.
- Added a shared resource-bounded UTF-8 XML parser at the NETCONF hello/RPC,
  configuration/edit, NACM, and filter boundaries. It rejects embedded NULs,
  malformed encodings, forbidden XML characters, DTD/entity declarations, and
  unexpected multiple roots while keeping edit fragments explicitly supported.
- Extended strict parsing to notification filters and events, URL-provider
  configuration, plugin operation and operational output, NACM readable data,
  and snapshot XML imported through datastore validation.
- Completed the XML construction/reparse audit, routed all core internal
  reparses through the strict parser, added UTF-8-safe output escaping and
  protocol fuzz seeds, and fixed raw YANG Library notification ID insertion.
- Rejected multiple top-level configuration elements before datastore schema
  binding instead of validating only the first root and ignoring siblings.
- Rejected multiple top-level XML elements at the NETCONF `<hello>` and RPC
  boundaries instead of dispatching only the first root.
- Accepted closing brackets inside quoted NACM node-instance predicate values
  instead of mistaking them for the end of the predicate.
- Rejected managed NACM XML with multiple top-level elements instead of loading
  only the first policy container and silently ignoring siblings.
- Removed schema-aware list and leaf-list instances whose identity contains
  both XPath quote forms and therefore cannot be represented without forbidden
  functions in an NACM node-instance identifier, without treating ordinary
  container children as predicates.
- Built schema-aware NACM list paths from declared keys in model order and
  removed entries with missing or duplicate keys before authorization.
- Kept an explicitly supplied runtime schema active during recovery-user and
  disabled-NACM read bypass, pruning unmodeled provider data without applying
  authorization rules.
- Restricted readable-data envelopes to the NETCONF/NMDA namespaces or the
  internal unqualified form, preventing a modeled `data` node from becoming a
  false transport wrapper.
- Required the readable-data filter's documented `<data>` envelope so a data
  node supplied as the document root cannot escape NACM authorization.
- Rejected readable-data XML with multiple top-level elements so later roots
  cannot escape NACM traversal and be serialized without authorization.
- Applied XML parsing and resource limits before recovery-user or disabled-NACM
  read-filter bypass, so authorization bypass cannot bypass input safety.
- Validated schema-aware notification bodies before NACM authorization and
  queueing, rejecting unknown, missing, duplicate, over-limit, ill-typed,
  keyless, non-unique, or conflicting/absent choice data.
- Bound schema-aware notification publication to the exact modeled XML event
  identity before NACM authorization, replay storage, or subscriber delivery.
- Made schema-aware NACM read filtering omit unmodeled XML elements instead of
  exposing them through the default read policy without module annotations.
- Rejected unsafe or duplicate host recovery identities before granting NACM
  bypass privilege.
- Rejected transport-supplied NACM external groups unless their authenticated
  provenance is explicitly trusted, and rejected malformed or duplicate groups.
- Coupled persistent datastore mutations to durable snapshot publication and
  restored the prior file, live running backend/plugins, and managed NACM policy
  before returning an error when persistence fails.
- Made TLS-to-NACM username mapping fail closed for duplicate, absent, empty,
  oversized, whitespace-padded, control-containing, or embedded-NUL common names.
- Made configured first-boot persistence fail closed, durably saving validated
  initial datastores and seeded NACM before application startup succeeds.
- Prevented NACM `access-denied` messages from echoing internal expanded-name
  paths or diagnostic policy context outside the required `error-path`.
- Synchronized NACM policy mutation, replacement, and snapshot copying so
  concurrent sessions cannot observe a mixture of policy generations.

### Added

- Added a transport-neutral exact authenticated-username mapper with optional
  required-match behavior and fail-closed rule validation.
- Extended protocol fuzzing from NACM XML loading through attacker-controlled
  instance-path CRUD authorization and read filtering, with keyed and leaf-list
  predicate seeds.
- Added fail-closed selection of certificate CN, DNS SAN, or URI SAN as the TLS
  NETCONF/NACM username source.
- Added a structured recovery-user RPC audit hook and concurrent evidence that
  successful and malformed attempts are recorded without retaining payloads.
- Added simultaneous secure-session tests proving trusted external groups do
  not leak authorization between identities.
- Added transaction fault tests before and after atomic snapshot replacement.
- Added restart tests that interrupt initial snapshot persistence at every
  atomic-save milestone and verify managed NACM remains enforceable.
- Applied one immutable NACM snapshot to each notification replay and live
  multi-session fanout, with concurrent replacement stress coverage.
- Added a denial matrix for every supported standard NETCONF, notification,
  monitoring, and NMDA operation, including the `close-session` exception.
- Added a multi-session stress test for live NACM policy replacement and
  per-RPC generation consistency.
- Verified global NACM first-match ordering when a user matches several groups
  and ordered rule-lists.
- Added exhaustive managed NACM rule-type choice tests for every pairwise and
  three-way selector combination.
- Verified fail-closed remote URL copies across source reads, NACM target
  inspection, and atomic provider writes without false NACM denial accounting.
- Verified that NACM ignores virtual defaults for datastore writes while
  enforcing create/delete access for explicitly stored default-valued nodes.
- Added instance-specific NACM filtering and create/delete authorization for
  leaf-list values, including values containing apostrophes.
- Represented `ordered-by user` list and leaf-list reordering as explicit move
  deltas, mapped moves to NACM update access, and described their positions to
  configuration backends.
- Added RFC 8526 `<edit-data>` protocol evidence for effective NACM create,
  update, delete, wrong-bit denial, and atomic target preservation.
- Verified that confirmed-commit cancellation and timeout restoration remain
  server-initiated when a newer NACM policy denies session writes.
- Added protocol-level NACM CRUD evidence for effective keyed creation,
  scalar update, keyed deletion, wrong-bit denial, and atomic preservation.
- Added protocol evidence that URL `<copy-config>` combinations read-filter
  datastore sources and atomically authorize datastore targets.
- Serialized expanded-name validation and NACM data-write `error-path` values as
  namespace-bound NETCONF XPath, including keyed list predicates.
- Applied NACM read filtering to datastore `<copy-config>` sources, honored the
  running-to-startup execute-only exception, and made complete copy sources
  remove omitted top-level nodes from their target.
- Hardened the standalone NACM loader to reject unknown, foreign, config-false,
  and duplicate elements plus invalid or duplicate group and membership values.
- Preserved explicit empty NACM `bits` and selector values as no-access/no-match
  values instead of incorrectly broadening them to wildcard permissions.
- Added a synchronized multi-stage RPC test proving RFC 8341 policy snapshot
  isolation while the live NACM policy is replaced.
- Added namespace-correct RFC 8341 `error-path` values to NACM-denied standard
  and schema-defined RPC replies, without disclosing `error-info`.
- Included the general and NACM compliance documents in installed project
  documentation.
- Added a section-indexed RFC 8341 executable-evidence matrix and tests for
  disabled enforcement, wildcard groups/modules/names, CRUDX bits, ordered
  first-match behavior, and denial-counter bypass.
- Distinguished explicit edits from implicit `choice` and `when` removals so
  RFC 8341 authorization does not demand permission for validation side effects
  while backends continue to receive the complete resulting delta.
- Bound data-associated notifications to concrete keyed instances in a supplied
  operational snapshot and applied NACM ancestor checks to those instances
  before replay or live delivery.
- Added authorization-first datastore-instance binding for YANG 1.1 actions,
  including complete keyed-list selection and missing-parent rejection before
  application dispatch.
- Added a standards compliance ledger covering implemented scope, integration
  boundaries, missing behavior, deliberate variances, reference-only RFCs, and
  the evidence required before an unqualified compliance claim.
- Added a detailed RFC 8341 NACM and RFC 8342/RFC 8526 NMDA compliance-closure
  checklist, plus production implementation guidance in the RFC 8344 example
  plugin for planning, platform application, rollback, and operational data.
- Added plugin ABI v3 operational-state publication, schema-bound fragment
  merging, RFC 8344 example interface state, and RFC 8526 `with-origin` plus
  positive and negated origin filtering for applied intended configuration.
- Added an RFC 8342 `operational` retrieval target combining applied intended
  configuration with core schema-bound state, including schema-aware
  `config-filter` separation and read-only enforcement.
- Added RFC 8526 `<get-data>` and `<edit-data>` for conventional datastores,
  including identity-based datastore selection, filters, maximum depth, NACM,
  atomic edits, and the normative NMDA/origin model dependency closure.
- Added the first NMDA datastore increment: a read-only RFC 8342 `intended`
  view that initially mirrors `running` and is published in RFC 8525 YANG
  Library.
- Added the `dangd` RFC 8344 IP-management example plugin and integration
  test.
- Added schema-aware NACM authorization for application RPCs, YANG 1.1
  actions, and data-associated notifications, including action and
  notification ancestor checks and schema-derived `default-deny-all`.
- Added a host operation-provider boundary and NACM filtering of successful
  application operation output.

### Fixed

- Rejected unmodeled attributes and mixed character content in managed NACM
  XML instead of silently ignoring unsupported policy metadata.
- Evaluate `when` expressions declared on choices and cases from their parent
  data-node context, allowing the normative RFC 8526 origin feature schema to
  resolve its datastore condition.
- Permit standard YANG statements within extension invocations, whose
  substatement grammar is defined by the extension rather than the built-in
  statement registry.
- Retained module and annotation metadata for NACM ancestor checks, derived
  nested-notification context from runtime schema paths, and bound RPC/action
  input and output to the compiled operation schema.

### Added

- A POSIX dynamic-plugin ABI whose providers supply implemented and dependency
  YANG sources and participate in prepare, validate, apply, rollback, and
  release phases, with a loadable reference implementation and author guide.
- RFC 8525 YANG Library operational data, including plugin-provided modules,
  datastore schema mappings, and deterministic content identifiers.
- Core-managed RFC 8341 NACM datastore configuration; `--nacm` seeds an absent
  policy, successful commits atomically replace the active policy, and denial
  counters remain continuous core-provided operational state.
- A running-backend preflight and failure boundary so external validation or
  application failure leaves the running datastore unchanged.
- A separate `dangd` server-application foundation with startup model and
  configuration validation, datastore snapshot lifecycle support, and a
  supervised stream integration mode.
- A reusable running-configuration backend notification boundary driven by
  exact schema-aware diffs whenever the effective running datastore changes.
- Initial C++20 YANG/YIN compiler, effective-schema model, configuration
  validation and editing libraries, and transport-neutral NETCONF stack.
- RFC 6241 datastores and operations, RFC 6242 framing, RFC 6243 defaults,
  RFC 5277 notifications, RFC 8341 NACM, persistence, filters, resource limits,
  fuzzing, sanitizers, and deterministic benchmarks.
- Mutual-TLS `dangd` listening, certificate-to-NETCONF identity handoff, NACM
  policy loading, and the interactive `dangctl` XML paste-and-reply client,
  with test-only certificates and RFC 8341 model fixtures.

### Fixed

- Configuration validation no longer requires mandatory `config false` nodes
  or mandatory children belonging to inactive choice cases.

- Applied deferred GoogleTest discovery to every sanitizer-instrumented test
  target on macOS, avoiding false container-overflow failures from linking an
  instrumented executable with Homebrew's prebuilt GoogleTest.
- Corrected edit authorization to compare its iterator against the exact
  change set being searched, avoiding undefined behavior for denied edits.
- Ensured transport adapters flush a successful `close-session` reply before
  shutting down the underlying SSH or TLS stream.

### Tests

- Added NETCONF error-interaction coverage proving that invalid edit targets
  and schema-invalid candidate commits fail without publishing backend state.

### Changed

- NETCONF validation error messages now include the defining YANG module and
  instance path when available, making model failures directly actionable.

### Documentation

- Added a users guide that explains the library and host-application layers,
  then walks through model loading, validation, NETCONF edits and commits,
  diagnostics, persistence, and integration using `dangd` as the example.
- Expanded Doxygen across all library and `dangd` source files, completed the
  daemon's public API contracts, and made documentation warnings fail the build.

## [0.1.0] - 2026-08-13

### Added

- First development release of the complete library foundation.
