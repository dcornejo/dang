<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd changelog

## [Unreleased]

### Fixed

- Kept NACM denial messages generic instead of appending internal datastore
  paths, while retaining namespace-correct NETCONF `error-path` output.
- Removed a data race between managed NACM replacement and concurrent server,
  notification, and operational policy readers.

### Added

- Kept notification replay and live fanout internally consistent while managed
  NACM policy is replaced concurrently.
- Verified NACM execute denial, error paths, accounting, and non-disclosure for
  every standard operation dispatched by `dangd`.
- Verified live managed-policy replacement while several NETCONF sessions take
  independent request snapshots.
- Verified that managed NACM policy preserves rule-list order for identities
  belonging to several configured groups.
- Rejected every multiple-selector NACM rule combination using XML leaf
  presence, including paths that cannot be expanded.
- Verified remote-to-remote URL copy failures preserve provider errors and
  target content without incrementing NACM denial counters.
- Verified NACM write semantics for virtual defaults and explicitly stored
  nodes whose value equals the schema default.
- Applied NACM leaf-list rules to individual values for reads and writes,
  retaining precise predicates when values contain apostrophes.
- Added ordered list and leaf-list move deltas, NACM update authorization for
  reordering, and English before/after position descriptions for plugins and
  operators.
- Verified that RFC 8526 `<edit-data>` selects effective NACM create, update,
  and delete permissions and preserves the target after wrong-bit denial.
- Verified confirmed-commit cancel and timeout rollback after live NACM policy
  replacement, without treating restoration as a new session write.
- Verified end-to-end NACM create/update/delete selection for effective
  `<edit-config>` changes and atomic wrong-bit denial.
- Verified NACM filtering and atomic denial for datastore-to-URL and
  URL-to-datastore copy operations.
- Returned namespace-correct XPath for configuration and NACM data errors
  instead of internal expanded-name paths.
- Corrected `<copy-config>` NACM behavior for filtered datastore sources,
  complete target replacement, and the running-to-startup special case.
- Made managed NACM policy compilation fail closed on invalid model structure,
  foreign elements, and group or membership uniqueness violations.
- Prevented empty managed NACM operation and selector values from becoming
  wildcard access during policy compilation.
- Verified that an in-flight RPC retains one NACM policy snapshot while a new
  managed policy becomes effective for subsequent requests.
- Added RFC 8341 operation-identifying `error-path` serialization for denied
  NETCONF and model-defined RPC execution.
- Added an RFC 8341 section-indexed compliance matrix covering `dangd` policy
  bootstrap, session identity, operation/data/notification enforcement, and
  remaining production evidence.
- Preserved complete backend/plugin deltas while excluding implicit YANG
  `choice` and `when` removals from separate NACM write authorization.
- Connected notification instance binding to `dangd`'s live operational view,
  failing publication when associated parents or list keys do not exist.
- Required authorized plugin actions to select an existing operational parent
  instance with all list keys before the plugin can be invoked.
- Documented `dangd` compliance gaps and integration boundaries for NETCONF,
  NACM, NMDA, YANG Library, transport, notifications, and example models.
- Expanded the RFC 8344 example's comments with practical Linux backend,
  transaction ordering, rollback, concurrency, and observed-state guidance.
- ABI v3 schema-bound operational publication, used by the RFC 8344 example
  for simulated interface state, plus `ietf-origin:intended` annotations and
  RFC 8526 origin selection.
- A read-only `operational` datastore snapshot containing applied intended
  configuration plus core YANG Library, monitoring, and NACM state, with
  `config-filter` selection and RFC 8525 publication.
- RFC 8526 conventional-datastore access through `<get-data>` and
  `<edit-data>`, with pinned `ietf-netconf-nmda`, `ietf-origin`, metadata, and
  NETCONF dependency models exposed through YANG Library and `get-schema`.
- A read-only RFC 8342 `intended` datastore, initially identical to `running`,
  with RFC 8525 datastore publication and write-rejection regression tests.
- A self-contained `dangd_ip_management_plugin` example that owns the
  normative RFC 8343 `ietf-interfaces` and RFC 8344 `ietf-ip` models, prepares
  reversible actions from schema-qualified deltas, and prints apply or
  rollback activity while assuming the simulated platform operation succeeds.
- An application-level regression test covering RFC 8344 model discovery, a
  validated IPv4 interface edit, commit-time action output, and publication to
  the running datastore.
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

- Made managed NACM loading fail closed on unmodeled XML attributes and
  character content embedded in structural containers.
- Preserve schema module and inherited NACM annotation context while checking
  action and data-associated notification ancestors, so module-wide rules work
  consistently with path rules.
- Resolve associated-notification ancestors from the compiled schema and one
  exact instance path instead of trusting caller-supplied ancestor metadata.
- Validate application RPC and action input/output structure, mandatory nodes,
  element counts, and scalar types before crossing the plugin boundary, and
  filter operation output relative to its actual output schema and action path.
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

- Added regressions for module-only ancestor permissions, schema-derived keyed
  notification ancestors, typed RPC input rejection before dispatch, and
  module-aware operation output filtering.
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
