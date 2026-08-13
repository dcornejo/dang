<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd changelog

## [Unreleased]

### Added

- Initial `dangd` application boundary with model and configuration loading,
  complete startup validation, datastore ownership, optional snapshot restore
  and save, a validation-only command, and a supervised stdin/stdout NETCONF
  integration transport.
- Build-matrix isolation for fuzzing and benchmark configurations, strict
  session-ID parsing, and installed `dangd` documentation.
- An in-memory running-configuration backend that atomically replaces its
  working configuration and expresses created, deleted, changed, and replaced
  schema-aware deltas in English.
- A mutual-TLS listener that requires trusted client certificates, maps the
  verified certificate common name to the NETCONF/NACM username, and loads an
  optional RFC 8341 policy at startup.
- The `dangctl` interactive TLS client, sample appliance and NACM inputs,
  normative NACM model dependencies, and test-only server/Alice credentials.

### Fixed

- Deferred GoogleTest discovery for sanitizer builds on macOS so the existing
  prebuilt-library compatibility workaround also covers `dangd_tests`.

### Tests

- Added end-to-end NETCONF failures for an invalid edit target and for a
  schema-invalid candidate commit, verifying that failed operations preserve
  the running and backend working configurations and emit no backend deltas.
- Extended the invalid-commit interaction to verify that the NETCONF error
  identifies both the defining YANG module and the failing instance path.

### Documentation

- Documented the planned separation of YANG desired-state validation, platform
  preflight, and dependency-ordered transactional hardware application, and
  recorded its implementation and failure-testing work in the project TODO.
