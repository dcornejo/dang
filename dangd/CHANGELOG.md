<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd changelog

## [Unreleased]

### Changed

- Refreshed the server overview and IP-management example description to match
  the implemented SSH/TLS, NACM, worker-plugin, and native Linux behavior.
- Added the tested framed IPC foundation for moving plugin ownership into
  supervised worker processes without sharing library or opaque state pointers.
- Added worker startup/load handshakes, manifest/source discovery, malformed
  request rejection, clean shutdown, and attributed load-failure reporting.
- Added a stateful worker client that serializes requests, independently bounds
  worker output, and kills/reaps a worker after callback timeout or crash.
- Added separate worker prepare/validate phases plus idempotent abort, preserving
  dependency-wide two-phase validation without exporting prepared pointers.
- Added bounded worker hardware-action descriptions and named apply/rollback
  requests, including a synthetic transaction action for pre-v4 plugins.
- Added global worker action coordination with module-owner validation,
  dependency qualification, deterministic ordering, and rollback on failure.
- Added worker applied-state reconciliation requests with bounded XML,
  disposition, path, reason, and provider revalidation in the parent client.
- Added atomic coordinator reconciliation with schema and ownership validation,
  unique outcome enforcement, commit-after-validation, and rollback on failure.
- Added operation invocation to the worker protocol and client, preserving
  operation-not-supported attribution and containing callback failure.
- Added the `PluginRuntime` server-facing interface so live worker ownership can
  replace `PluginManager` without changing datastore or NETCONF layers.
- Added `PluginWorkerRuntime`, which owns one supervised process per plugin and
  implements the complete server-facing runtime contract using copied IPC data.
- Added live application selection through `plugin_worker_executable`; the CLI
  resolves its worker automatically and supports `--plugin-worker FILE`.
- Added typed IPv4/IPv6 MTU and neighbor intent to the IP platform boundary and
  rejected invalid, incomplete, or duplicate modeled kernel input.
- Implemented the Linux IP backend with direct, acknowledged rtnetlink link,
  address, and neighbor operations instead of invoking `/sbin/ip`.
- Preserved observed Linux flags and MTU until commit so transaction rollback
  restores actual pre-apply state, with reverse partial-failure compensation
  and privileged isolated-network tests.
- Added live Linux RFC 8343/8344 compatibility publication for managed link
  status, MTU, and kernel-assigned IPv4/IPv6 address prefixes.
- Published the live Linux ARP and IPv6 neighbor cache from an authenticated
  rtnetlink dump, including static/dynamic origin and IPv6 state metadata.
- Added native FreeBSD `SIOC*` application for link flags, MTU, and IPv4/IPv6
  addresses with exact transaction rollback and partial-failure compensation.
- Added acknowledged FreeBSD route-netlink application and rollback for static
  IPv4/IPv6 neighbors, preserving address/neighbor dependency ordering.
- Added native FreeBSD compatibility-state publication for live link status,
  MTU, assigned IPv4/IPv6 prefixes, and complete ARP/ND neighbor entries.
- Added live FreeBSD drift repair for configured addresses and neighbors,
  including exact inverse operations and preservation of unrelated kernel state.
- Added equivalent Linux address and neighbor drift repair with live snapshots,
  exact rollback, and isolated-namespace regression interactions.
- Covered permanent FreeBSD IPv6 neighbor application, publication, drift
  repair, and rollback in the privileged disposable-epair interaction.
- Added opt-in privileged FreeBSD backend tests and removed the obsolete
  command-execution helper from the IP plugin.
- Verified concurrent operational publication with a thread-safe test provider,
  eight overlapping sessions, 200 complete retrievals, and response validation.
- Added a bounded environment override for scaling that identical workload into
  a long-running ASan/UBSan release soak without slowing routine test runs.
- Completed that soak at 1,000 requests per worker: 8,000 valid responses in
  315.7 seconds with no ASan/UBSan finding.
- Enforced the 16 MiB XML ceiling at the operational plugin callback boundary
  before unbounded string construction, with fail-closed end-to-end coverage.
- Added atomic reload coverage across deviation and plugin inventory changes,
  including content identifiers, current/legacy notifications, five datastore
  schema references, compiled plugin data, and post-reload `get-schema`.
- Added cross-product retrieval coverage for every conventional NMDA datastore,
  both selection forms, `config-filter`, `max-depth`, invalid non-operational
  origin requests, and an unsupported datastore identity.
- Closed the NMDA provider-validation work item after exact-instance
  completeness coverage, and documented that dynamic configuration datastores
  are neither implemented nor advertised.
- Retained ABI-v5 completeness for exact operational instances instead of all
  nodes sharing a top-level QName; added complete-plus-partial keyed-list
  provider coverage.
- Retained validated per-node origin metadata in the applied XML snapshot and
  added inheritance- and identity-aware positive/negated filtering for every
  RFC 8342 origin while keeping metadata out of replies that did not request it.
- Added ABI-v6 post-apply configuration reconciliation and modeled per-node
  applied/transformed/rejected/delayed results, with deterministic plugin
  ordering, duplicate-path rejection, schema validation, and compensation on
  invalid reports.
- Corrected nested subtree-filter depth limiting so containment ancestors do
  not prune or consume the allowance of deeply selected nodes.
- Corrected XPath plus `max-depth` retrieval so depth begins at each selected
  node and does not prune deep selections merely because of their ancestors.
- Added an end-to-end operational retrieval interaction test combining NACM,
  `config-filter`, `with-origin`, subtree selection, and `max-depth`.
- Completed `<edit-data>` default-operation and rollback coverage and enabled
  RFC 8526 datastore identity targets for `<lock>` and `<unlock>`.
- Added schema-bound RFC 8526 RPC input validation, including atomic rejection
  of unknown or duplicate parameters and unsupported NMDA `with-defaults`.
- Returned operational-provider callback, validation, and merge failures from
  `<get>` and operational `<get-data>` as structured, actionable NETCONF errors
  while suppressing otherwise accepted partial operational payloads.
- Added ABI-v5-only cross-provider `must` and `when` coverage for operands
  absent from an earlier complete subtree while preserving open-data results.
- Added ABI-v5-only cross-provider instance-identifier resolution and rejection
  for paths whose target is proven absent from a complete operational subtree.
- Preserved complete operational subtrees across later provider merges and
  added positive and negative multi-plugin state-to-state leafref coverage.
- Added multi-plugin coverage proving that an ABI-v5 publisher's complete child
  collection enforces mandatory operational nodes when a separate ABI-v1
  plugin owns the YANG model, with failure attributed to the publisher.
- Added separate Linux and FreeBSD RFC 8343/8344 platform backends for enabled
  state and IPv4/IPv6 address reconciliation, with shell-free execution,
  best-effort compensation, a safe unsupported-host backend, parser tests, and
  documented privilege and compliance limits.
- Added ABI v5 `get_operational_data_v2`, whose explicit completeness assertion
  enables absence-sensitive validation without changing ABI v3/v4 behavior.
- Added paired ABI-v3 provider coverage for cumulative `must` and `when`
  violations with deterministic merge-stage attribution.
- Added paired real-plugin coverage for cross-provider `unique` violations and
  deterministic later-provider merge failure telemetry.
- Added a second provider merge gate using the backend's applied configuration
  as complete context, rejecting unresolved state-to-configuration leafrefs.
- Seeded provider merge validation with built-in operational data, preventing
  plugins from shadowing daemon-owned state.
- Added deterministic first-provider-wins arbitration for operational data:
  cumulative snapshots are validated in plugin load order and later conflicts
  are omitted with merge-stage reconciliation telemetry.
- Published current operational-provider callback and validation failures in
  `dangd-reconciliation` instead of silently dropping invalid fragments.
- Replaced top-level-name checks for plugin operational data with reusable typed
  partial-instance validation before fragments enter `<operational>`.
- Made NMDA origin filters use runtime identity inheritance rather than literal
  identity names, with namespace validation and repeated-filter coverage.
- Added the built-in `dangd-reconciliation` operational model and retained
  per-action hardware remnants after incomplete rollback until a later hardware
  transaction succeeds.
- Expanded NMDA origin reporting from top-level inheritance to explicit
  per-configuration-node `intended` metadata while preserving provider origins
  and leaving config-false state unannotated.
- Made operational configuration originate from the hardware backend's accepted
  working snapshot rather than an approximation copied from running.
- Added plugin ABI v4 and the common transactional hardware planner, including
  dynamic preflight, generic and plugin-declared ordering, reverse compensation,
  explicit state-divergence errors, legacy ABI compatibility, and a fine-grained
  RFC 8344 example implementation with failure-injection coverage.
- Added an embedded libssh NETCONF server with explicit host and authorized
  keys, exact `netconf` subsystem handling, username mapping, trusted NACM group
  records, SIGHUP reload, end-to-end authentication/RPC tests, and an OpenSSH
  interoperability smoke run.
- Added XML injection auditing to the remaining NACM security work and removed
  completed core and TLS evidence from the active TODO narrative.

### Fixed

- Rejected unresolved nested state leafrefs when an ABI-v5 provider declares
  the returned parent collection complete.
- Made the common XPath effective view retain operational state during
  config-false instance validation.
- Retained QName prefix bindings in common configuration serialization so
  applied identityrefs remain valid when dangd reparses backend snapshots.
- Validated duplicate config-false nodes when accepting operational instances.
- Hardened daemon-facing hello, RPC, configuration, edit, NACM, and filter XML
  parsing against byte smuggling, malformed UTF-8, forbidden characters,
  DTD/entity declarations, resource exhaustion, and unexpected multiple roots.
- Applied the same strict contract to notification publication, URL data,
  plugin replies and operational fragments, and restored snapshot content.
- Escaped YANG Library notification identifiers through the XML serializer and
  moved remaining daemon internal reparses onto the common strict parser.
- Closed multi-root XML configuration smuggling before datastore validation.
- Closed multi-root XML message smuggling at NETCONF session negotiation and
  RPC dispatch.
- Matched valid NACM paths whose quoted key or leaf-list value contains `]`.
- Rejected multi-root managed NACM input before compiling or publishing policy.
- Failed closed when list keys or leaf-list values contain both XPath quote
  forms and cannot be represented in an RFC 8341 instance path, while keeping
  container paths free of child-value predicates.
- Rejected missing or ambiguous list identities during schema-aware NACM reads
  and used only declared keys when matching instance-specific rules.
- Kept schema-aware pruning active for recovery and disabled-NACM reads so
  authorization bypass does not expose unmodeled plugin/provider data.
- Rejected `<data>` read-filter envelopes in arbitrary model namespaces while
  accepting NETCONF, NMDA, and the internal unqualified representation.
- Required a `<data>` reply envelope before NACM read filtering so the XML root
  cannot be mistaken for an unauthorizable transport wrapper.
- Rejected multi-root readable-data XML before NACM filtering, including on
  recovery and disabled-enforcement paths.
- Kept XML syntax and resource-limit enforcement active for recovery sessions
  and when NACM policy enforcement is disabled.
- Rejected plugin/host notification bodies that violate their advertised YANG
  structure or scalar constraints before NACM authorization and delivery.
- Rejected plugin/host notifications whose claimed module and name do not match
  the modeled XML event root or associated instance path.
- Omitted unmodeled datastore elements at the schema-aware NACM read boundary
  instead of applying an annotation-free default read decision.
- Made startup fail when recovery-user configuration contains an unsafe,
  malformed, oversized, padded, or duplicate privileged identity.
- Required authenticated provenance for transport-supplied NACM external groups
  and rejected invalid, oversized, or duplicate group identities.
- Moved snapshot saving into the live datastore transaction boundary so a
  failed save rolls back the durable file, backend/plugins, and managed NACM
  policy before NETCONF sends `operation-failed`.
- Rejected ambiguous or unsafe certificate common names before constructing a
  NETCONF/NACM session identity.
- Required the first configured state snapshot, including seeded NACM, to be
  durably written before `dangd` startup completes.
- Kept NACM denial messages generic instead of appending internal datastore
  paths, while retaining namespace-correct NETCONF `error-path` output.
- Removed a data race between managed NACM replacement and concurrent server,
  notification, and operational policy readers.

### Added

- Added repeatable `--username-map AUTHENTICATED=LOCAL` rules and
  `--require-username-map` for fail-closed TLS-to-NACM account mapping.
- Extended NACM sanitizer smoke coverage through instance-path authorization
  and filtering, including keyed and leaf-list predicate corpus seeds.
- Added `--tls-username-source` with exact CN, DNS SAN, and URI SAN identity
  selection and ambiguity rejection.
- Added safely encoded diagnostic audit records for every recovery-user RPC
  attempt without logging request payloads or policy internals.
- Covered live commit persistence failures both before and after atomic file
  replacement, including byte-for-byte durable snapshot restoration.
- Added deterministic first-boot persistence checkpoints and restart coverage
  for every atomic snapshot stage.
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
