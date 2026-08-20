<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes are documented here. The format follows Keep a Changelog,
and releases follow Semantic Versioning.

## [Unreleased]

### Added

- Added schema-aware NACM authorization for application RPCs, YANG 1.1
  actions, and data-associated notifications, including action and
  notification ancestor checks and schema-derived `default-deny-all`.
- Added a host operation-provider boundary and NACM filtering of successful
  application operation output.

### Fixed

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
