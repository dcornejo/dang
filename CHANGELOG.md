<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Changelog

All notable changes are documented here. The format follows Keep a Changelog,
and releases follow Semantic Versioning.

## [Unreleased]

### Fixed

- Drained already-buffered SSH channel data after client EOF, emitted replies
  decoded from one pipelined receive as one ordered transport write, and flushed
  libssh's bounded output buffer before channel close. OpenSSH closes its
  standard input after pipelined NETCONF requests; under concurrency the prior
  sequence could omit the final `<close-session>` reply and close prematurely.

### Added

- Recorded native FRR RIP and RIPng operational readback on the current
  isolated Debian 13 and FreeBSD 16.0-CURRENT guests. Each interface-free
  default instance is visible through mgmtd `GET_DATA` while committed and its
  exact empty before-image is restored; learned-route, neighbor, RPC, and
  notification coverage remains open.
- Recorded successful cross-platform RIP and RIPng configuration evidence:
  interface-free instances commit, read back, and roll back to the exact empty
  before-image on both Linux and both FreeBSD validation hosts without creating
  network state or traffic.
- Recorded the cross-platform BFD behavioral result and the external provider's
  new post-commit verification boundary. FRR advertises `frr-bfdd` on all four
  hosts but silently drops a profile because `bfdd` registers no mgmtd backend;
  dang-frr now rejects that acknowledged no-op at the affected path while
  preserving rollback, and the guarded interaction verifies restoration.
- Recorded the external FRR provider's isolated Linux optional-daemon inventory
  and kept its scope explicit: live RFC 8525 registration is observed without
  creating network state. The same inventory is now available for FreeBSD,
  where independent runs on both 16.0-CURRENT hosts advertised the same BFD,
  RIP, and RIPng modules. Daemon-by-daemon behavioral interoperability remains
  unfinished. Independent FRR 10.7.1 Linux runs
  on both Ubuntu 26.04.1 hosts advertised BFD, RIP, and RIPng and correctly
  excluded the six other installed optional daemons with no live mgmtd module.
- Recorded package-level exclusivity between the external FRR-native and RFC
  8431 RIB providers on Debian and FreeBSD, while retaining runtime rejection
  through their shared ABI-v7 `routing` resource domain.
- Reconciled the FRR compliance account and remaining-work description with the
  external ABI-v8 provider, including exact notification selectors and the
  distinction between standalone protocol roots and augment-only modules.

- Recorded the Linux-only VPP ownership architecture and test-host safety
  boundary: `ens18` is protected as the SSH/default-route path, `ens19` is the
  only future private-LAN candidate, and physical claims require stable PCI
  identity, explicit allowlisting, and independent recovery.
- Recorded completion of the external VPP provider's read-only Linux inventory
  and validation-only ownership policy. Exact PCI, MAC, and vendor/device
  identity plus live management/default-route denial now pass on both Linux
  hosts; binary-API discovery, plugin exposure, broader topology evidence, and
  reversible transfer remain open.
- Recorded the external provider's reversible, programmatic VPP loopback
  transaction seam and its failure-injected Linux coverage. Live generated
  VAPI validation remains open because the Ubuntu 26.04 test hosts have no
  matching official FD.io package repository.
- Recorded completion of generated C++ VAPI loopback interoperability on both
  Linux hosts. The isolated VPP 26.06 runs loaded no PCI plugins and restored
  loopback absence; loadable-provider wiring and physical ownership safeguards
  remain open.
- Recorded the external VPP stable-instance loopback model and ordered snapshot
  planner. ABI-v7 provider action wiring remains the next software-interface
  milestone.
- Recorded completion of the external loadable ABI-v7 VPP provider and live
  provider-level loopback apply/rollback validation. Physical ownership safety
  work remains open.
- Recorded completion of live VPP loopback operational publication and
  applied-state reconciliation with unrelated datastore content preserved.
- Added the VPP application-socket integration requirement. A transparent
  dangd/provider boundary will use VCL `vppcom` without exposing VPP mechanics
  to NETCONF users, while explicitly keeping every management and recovery
  socket on the protected host stack and testing POSIX behavior, failures,
  lifecycle, and rollback before private-LAN use.

- Refreshed the RFC 8431 compliance ledger after the external RIB provider
  implemented `nh-add` and `nh-delete`; the TODO now tracks the narrower
  persistence, operational publication, and route-reference semantics still
  required for reusable nexthops.
- Narrowed the reusable-nexthop gap again after configuration commits and
  `route-add` gained per-RIB `nexthop-ref` resolution; lifetime enforcement and
  `route-update` resolution remain explicitly tracked.
- Recorded `route-update` reusable-nexthop resolution as complete; deletion
  lifetime enforcement remains the last reference-integrity gap.
- Recorded datastore transaction reference reservations and narrowed remaining
  deletion-lifetime work to routes created or changed through imperative RPCs.
- Recorded completion of reusable-nexthop lifetime enforcement for imperative
  route RPCs, including release on successful route and RIB deletion.
- Recorded live RFC 8431 nexthop identifier publication and narrowed the state
  gap to family-ambiguous interface-only objects without an observed RIB.
- Recorded reconciliation-based reconstruction of datastore nexthop references,
  removing dependence on pre-restart in-memory counters before registry
  persistence is introduced.

- Moved the RFC 8343/8344 IP-management provider, pinned `ietf-ip` model,
  platform backends, and focused tests into the separately packaged
  `dang_plugins` repository. Dangd retains only its generic external-plugin
  contract and optional host-side integration test.
- Advanced RFC 8431 from schema-only preparation to an external ABI-v7 runtime
  provider with exclusive routing ownership and tested Linux/FreeBSD route
  apply and rollback; RPC, notification, operational, and mapping gaps remain.
- Corrected the external FRR provider's hardware plan so dangd schedules its
  atomic mgmtd configuration transaction as one normal coordinator action.
- Refined the remaining FRR RPC gap after live 10.5.1 probing confirmed that
  active zebra does not register its modeled RPC subtree with mgmtd.
- Completed the external Kea provider item after both pinned modules gained
  full configuration and state-tree coverage, bounded native enumeration, host
  option-data translation, and truthful ABI-v5 completeness. Linux and FreeBSD
  native interactions verify the complete provider path.
- Narrowed the external Kea completion item after supplemental statistics were
  bounded to exact subnet IDs from the accepted configuration. Complete-state
  schema verification and a truthful ABI-v5 declaration remain open.
- Documented the provider paging contract: backend pages require opaque,
  advancing cursors and independent page, item, byte, and elapsed-time bounds,
  while client-facing NETCONF remains a single fail-closed reply unless a
  separately advertised pagination extension is designed. Narrowed the Kea
  gap after native lease paging and recorded BaseX as a deferred backend behind
  dangd's existing persistent-state transaction seam.
- Narrowed the external Kea completion item again after bounded, paged native
  host-reservation publication joined the existing lease state. Bounded
  statistics ranges and complete-state verification remain open.
- Refined the external Kea completion item after ABI-v3 operational publication
  of DHCPv4/DHCPv6 leases and supplemental lease statistics passed live Linux
  and FreeBSD validation. Safe paging, host reservations, complete-state schema
  verification, and ABI-v5 completeness declarations remain required before
  claiming complete module support.
- Added plugin ABI v8 modeled event publication. Isolated workers copy bounded,
  nonblocking provider events to the trusted core, which validates their YANG
  identity and XML content before applying RFC 5277 subscription filters and
  RFC 8341 NACM. SSH, TLS, and stdin sessions poll the path, concurrent
  in-process drains are serialized, and worker plus end-to-end subscription
  tests cover one-shot delivery.
- Recorded the external FRR provider's adoption of ABI v8 for deduplicated,
  unsolicited configuration-drift events from its read-only mgmtd watcher.
- Recorded generic external-provider dispatch for all installed `frr-zebra`
  RPCs through FRR's public native mgmtd protocol, while retaining live backend
  interoperability as an explicit validation gap.
- Added plugin ABI v7 exclusive resource-domain declarations and copied them
  through supervised worker discovery. In-process loading, worker-runtime
  assembly, recovery identity checks, and the hardware coordinator now reject
  duplicate claims such as two different model families controlling
  `routing`; malformed and duplicate declarations fail discovery.
- Added low-priority work to replace command-driven plugin backends with
  programmatic APIs, including Linux and FreeBSD netlink evaluation, and to
  evaluate separately packaged, explicitly Linux-only FD.io VPP providers.
- Added an FRR-backed routing plugin work item with Linux and FreeBSD isolation,
  supported-management-interface, and explicit native-RIB ownership/conflict
  requirements.
- Added the first managed RFC 9642 keystore slice: central cleartext symmetric
  keys, NACM-protected edit/retrieval, durable restart restoration, built-in
  model retrieval, and accurate YANG Library feature publication.
- Added the complete pinned RFC 9644 SSH grouping family and dependency closure
  as built-in import-only schemas, including YANG Library advertisement,
  `get-schema` retrieval, internal conformance coverage, and independent
  libyang `yanglint` interoperability validation.

- Added the built-in `dangd-superuser` NETCONF recovery identity. It exists only
  in dangd's NACM recovery set, has no packaged credential or operating-system
  account, is audited like every recovery identity, and can be removed with
  `--no-default-superuser` after an alternate recovery path is verified.
- Added native Debian and FreeBSD package generation from the audited CMake
  install manifest, including platform dependency metadata and packaging
  instructions. Packages remain deliberately service-neutral until an
  administrator supplies deployment-specific model, datastore, NACM, key,
  and authenticated-identity binding configuration.
- Excluded repository-only SSH and TLS test credentials from the production
  install and native package manifests.

### Changed

- Narrowed the external FRR-provider roadmap after adding fail-closed live
  operational XML retrieval through FRR's native mgmtd frontend API, including
  ownership-filtered zebra augments below imported interface and VRF lists,
  post-apply running-datastore reconciliation, and read-triggered detection of
  out-of-band FRR configuration drift. The provider now also derives enabled
  features from the running daemon's RFC 8525 YANG Library and rejects schema
  skew during discovery. Opt-in Linux and FreeBSD tests now exercise an actual
  isolated mgmtd commit, read-back, rollback, and restoration check; the work
  also corrected candidate/running transaction locking in the provider.
- Promoted the FRR-native provider to the highest roadmap priority, immediately
  followed by the IETF RFC 8431 provider. Recorded the decisions to use one
  coordinated plugin, pinned native FRR YANG, the programmatic `mgmtd` API,
  dangd configuration authority with drift detection, and exclusive ownership
  of routing resources.
- Made programmatic library, kernel, and structured daemon APIs the required
  preference for new plugin implementations; command execution now requires a
  documented lack of a suitable API plus argv-only failure/rollback coverage.
- Narrowed the RFC 8431 work item after adding the strict portable route parser,
  delete-before-install delta planning, shell-free Linux/FreeBSD execution,
  reverse compensation, and isolated native route tests in `dang_plugins`;
  dangd continues to advertise no RIB implementation until plugin ABI, RPC,
  notification, and operational behavior are complete.
- Preserve feature-disabled schema branches until standards-valid augments have
  resolved, then prune them before runtime use; lower unprefixed XPath names in
  imported groupings against their effective using-module namespace.
- Resolve XPath data paths through transparent `choice` and `case` schema nodes
  and use the effective namespace for unprefixed paths in imported groupings,
  allowing unmodified modern IETF crypto and SSH modules to compile.

- Completed startup configuration detection and plugin hydration. Initial,
  restored, and reload-provided running trees are activated as one full-tree,
  dependency-ordered plugin transaction before an application is returned;
  snapshot restoration defers backend work to prevent duplicate application,
  and activation failure leaves the daemon unavailable and first-boot state
  unpublished.
- Completed the configuration save/restore TODO with private mode-0600 atomic
  snapshots, same-descriptor restore checks for type, ownership, permissions,
  size and symlinks, cleanup after interrupted saves, and a documented live
  backup and offline restore procedure.
- Refreshed the documentation checkpoint to reconcile current FreeBSD IP
  behavior, transport evidence, persistence boundaries, package security, and
  the newly recorded standards, privileged-identity, startup-hydration, and
  datastore roadmap.
- Permit `must` and `when` paths to refer to schema nodes removed by a disabled
  `if-feature`, while continuing to reject genuinely unknown paths. This is
  required by the unmodified RFC 7317 model when RADIUS is not advertised.
- Instantiate nodes from imported groupings in the using module's namespace and
  resolve unprefixed key and unique paths against that effective namespace,
  allowing standards-valid cross-module model families such as Kea DHCP to
  compile correctly.
- Audited the plugin author guide against the live ABI-v1-through-v6 worker
  implementation, corrected stale process, concurrency, entry-point, and
  IP-provider descriptions, and added architecture, transaction, capability,
  and worker-recovery diagrams.
- Propagated the platform thread library from `dangd_core` so production
  executables and downstream consumers link correctly on FreeBSD.
- Completed current-tree FreeBSD validation with the 386-test suite, all three
  privileged epair mutation interactions, and live duplicate IPv6 DAD from a
  temporary VNET-jail peer; added a regression assertion for published status.
- Completed RFC 8342/RFC 8526 conventional-datastore interoperability evidence
  with ncclient 0.6.17 over mutual TLS, including NMDA retrieval, edit/commit,
  intended visibility, origin metadata/filtering, and required error behavior.
- Defined the NMDA compliance claim around advertised features and explicitly
  documented unadvertised dynamic-datastore, defaults, and URL options.
- Added fail-safe plugin-worker recovery for later independent requests after
  crash, timeout, or protocol failure. Replacement workers must rediscover the
  exact loaded manifest and YANG sources, and failed requests are never replayed.
- Completed RFC 8341 compliance evidence with an external ncclient 0.6.17
  mutual-TLS subscription that received a permitted YANG Library notification,
  suppressed its paired denied legacy event, and incremented the denial counter.
- Made idle TLS sessions poll server-originated notification/timer work instead
  of blocking indefinitely for more client bytes.
- Made YANG Library XML construction compatible with packaged pugixml releases
  that require explicit C-string text assignment.
- Isolated the conceptual IP-plugin planning test from native host interfaces;
  production Linux/FreeBSD backends remain covered by their platform tests.
- Added a reproducible NACM decision interoperability check against sysrepo's
  independent RFC 8341 engine. Positive and negative read, update, group,
  default, and RPC cases passed on sysrepo 3.7.11/libyang 3.13.6, and the final
  intentional-deviation review found no deliberate semantic variance.
- Added a dedicated coverage-guided NACM policy/path fuzz target and focused
  corpus; a five-minute Clang 21 ASan/UBSan campaign completed 79,611,946
  inputs without a finding.
- Added direct `<algorithm>` includes for every range-algorithm user exposed by
  the Linux Clang/libstdc++ build instead of relying on transitive headers.
- Added bounded concurrent SSH session execution and slow-reader backpressure
  isolation, with safe application lifetime across SIGHUP reloads.
- Added an automated independent OpenSSH transport matrix covering negative
  authentication/subsystem cases and concurrently initiated NETCONF sessions.
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
- Reconciled FreeBSD managed address and neighbor intent against live kernel
  state even when configuration XML is unchanged, repairing missing or altered
  entries while preserving unrelated system-owned networking state.
- Reconciled Linux managed address and neighbor intent against live kernel
  state with observed-value rollback, including identical-configuration repair
  and preservation of unrelated namespace networking state.
- Extended privileged FreeBSD epair coverage through permanent IPv6 ND apply,
  live publication, out-of-band removal repair, and exact rollback alongside
  the existing IPv4 ARP interaction.
- Published every Linux kernel interface, including unconfigured system-owned
  devices, with inferred base interface types and native packet, octet, error,
  and drop counters using kernel boot time as counter discontinuity time.
- Published every FreeBSD kernel interface with native IANA base-type inference
  and packet, octet, multicast, error, drop, and unknown-protocol counters using
  kernel boot plus interface epoch as the discontinuity timestamp.
- Published RFC 8344 address status from Linux rtnetlink DAD/lifetime flags and
  FreeBSD `SIOCGIFAFLAG_IN6`, with a real duplicate-address veth interaction and
  deterministic FreeBSD flag-precedence coverage.
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
