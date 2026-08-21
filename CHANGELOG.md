<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes are documented here. The format follows Keep a Changelog,
and releases follow Semantic Versioning.

## [Unreleased]

### Fixed

- Synchronized NACM policy mutation, replacement, and snapshot copying so
  concurrent sessions cannot observe a mixture of policy generations.

### Added

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
