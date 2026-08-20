<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd changelog

## [Unreleased]

### Added

- Secure NACM bootstrap with repeatable host-owned `--recovery-user`
  identities that survive managed-policy commits and live reloads.
- Schema-driven application RPC and YANG 1.1 action dispatch through the
  owning ABI-v2 plugin, including action ancestor checks and NACM-filtered
  operation output.
- Data-associated notification authorization with ancestor paths and automatic
  `default-deny-all` discovery for top-level schema notifications.

- Atomic POSIX `SIGHUP` reload of configured schemas and fresh plugin images,
  with current-running validation, failure preservation, RFC 8525
  `yang-library-update` publication, and reconnect semantics for sessions that
  negotiated the superseded library.
- Built-in RFC 6022 `get-schema` retrieval for the exact YANG module and
  submodule sources used to compile the active schema.
- RFC 6022 NETCONF monitoring schema inventory with `NETCONF` retrieval
  locations and capability advertisement, plus RFC 8525's deprecated
  `/modules-state` compatibility view and `yang-library-change` notification.
- The versioned POSIX plugin loader, plugin-supplied YANG schema composition,
  runtime dependencies, two-pass validation, ordered apply, reverse rollback,
  reference plugin, and comprehensive plugin author guide.
- Datastore-managed NACM with seed-only `--nacm` migration and atomic active
  policy replacement after successful backend application.
- RFC 8525 YANG Library operational responses covering core, application,
  plugin, deviation, and import-only modules.
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

- Keep RFC 8341's enabled/read-permit/write-deny/exec-permit defaults active
  when the managed NACM subtree is absent or deleted instead of silently
  disabling enforcement.
- Derive RPC, action, and notification module and `default-deny-all` metadata
  from the compiled schema before making access-control decisions.

- Publish deviation relationships against every affected implemented module,
  suppress library notifications for no-change reloads, and stop advertising
  the NMDA operational datastore until that datastore is actually supported.
- Retained an explicitly configured revisioned root source in the compilation
  overlay so aggregate schema construction and live reload do not depend on a
  repository-style filename.
- Made plugin discovery atomic so a rejected plugin cannot leave partially
  published YANG sources, and reject incomplete or invalid dependency
  descriptors at load time.
- Preserve both the original apply error and every rollback error in the
  NETCONF response, including a distinct `plugin-rollback-failed` app-tag.
- Emit populated `error-app-tag` values in NETCONF RPC errors instead of
  silently dropping diagnostics produced by validators and backends.
- Deferred GoogleTest discovery for sanitizer builds on macOS so the existing
  prebuilt-library compatibility workaround also covers `dangd_tests`.

### Tests

- Added RFC 8341 decision coverage for action ancestors, associated
  notifications, recovery-state preservation, secure absent-policy defaults,
  schema RPC dispatch, action dispatch, and plugin-owned RPC invocation.

- Added RFC 8525 conformance checks for deviation linkage, current and legacy
  inventories, legacy conformance types, schema retrieval locations,
  monitoring capability advertisement, and no-change notification behavior.
- Added realistic provider and consumer plugins that verify dependency-driven
  affected-set expansion, prepare/validate/apply ordering, resource release,
  downstream apply failure, reverse rollback, rollback failure reporting, and
  missing dependency rejection.
- Added end-to-end NETCONF failures for an invalid edit target and for a
  schema-invalid candidate commit, verifying that failed operations preserve
  the running and backend working configurations and emit no backend deltas.
- Extended the invalid-commit interaction to verify that the NETCONF error
  identifies both the defining YANG module and the failing instance path.

### Documentation

- Documented the planned separation of YANG desired-state validation, platform
  preflight, and dependency-ordered transactional hardware application, and
  recorded its implementation and failure-testing work in the project TODO.
