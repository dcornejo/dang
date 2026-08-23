<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Writing a dangd configuration plugin

This guide defines the contract between `dangd` and a dynamically loaded
configuration provider. It is both a how-to and the behavioral specification
for plugin authors. The first ABI targets POSIX shared libraries only. Windows
loading and ABI conventions are intentionally out of scope.

## Responsibilities

`dangd` owns transport authentication, NETCONF framing and RPC processing,
datastore locks, candidate construction, common YANG validation, NACM
enforcement, transaction coordination, persistence, and RFC 8525 YANG Library
publication. A plugin owns the implementation-specific behavior for one or
more YANG modules: hardware checks, external resource checks, preparation,
application, and rollback.

Every implemented module has exactly one owning plugin. One plugin may own a
closely related family of modules. Multiple plugins may supply the same
byte-identical import-only module, but they may not both claim its
implementation.

NACM is deliberately not a plugin. `ietf-netconf-acm` configuration is stored
in the normal datastores, while `dangd` compiles and enforces the active policy
inside the trusted core.

## Build and entry point

Include `dangd/plugin_api.h`, build a `.so` or `.dylib`, and export exactly this
symbol with C linkage:

```cpp
extern "C" const DangPluginV1* dang_plugin_init_v1();
```

The returned table and all strings referenced by it must remain valid until
`destroy` is called or the library is unloaded. The table must declare
`DANG_PLUGIN_ABI_V1`. Do not pass C++ standard-library objects, exceptions, or
compiler-specific class layouts across the ABI.

The reference implementation is `dangd/plugins/example_plugin.cc`; CMake
builds it as `dangd_example_plugin`.

The more complete `dangd/plugins/ip_management_plugin.cc` example owns the
normative RFC 8343 `ietf-interfaces` and RFC 8344 `ietf-ip` modules. The build
embeds the pinned YANG sources in `dangd_ip_management_plugin`, so the shared
library remains self-contained. It converts each interface or IP delta into a
retained forward and reverse action plan. Its demonstration `apply` and
`rollback` callbacks print those actions to the daemon's diagnostic stream and
assume they succeeded; replace those two callbacks when adapting it to real
network interfaces.

For example, start `dangd` with the plugin using the module filename produced
by CMake:

```sh
./build/dangd --model dangd/examples/appliance.yang \
  --config dangd/examples/config.xml \
  --plugin ./build/dangd_ip_management_plugin.so --stdio \
  --username admin
```

An `<edit-config>` that creates `/interfaces/interface[name='eth0']`, enables
IPv4, and adds `192.0.2.1/24`, followed by `<commit>`, produces diagnostic
lines resembling:

```text
ip-management: create /{urn:ietf:params:xml:ns:yang:ietf-interfaces}interfaces/...
ip-management: create /{urn:ietf:params:xml:ns:yang:ietf-ip}ipv4/... with value "192.0.2.1"
```

The paths are the exact schema-qualified paths supplied by `dangd`, rather
than paths reconstructed by the plugin from XML. The regression test
`IpManagementPluginPublishesRfc8344AndPrintsApplyPlan` contains a complete
NETCONF request and verifies model discovery, validation, commit, logging, and
the resulting running configuration.

Plugins that implement YANG RPCs or actions use ABI v2 and export
`dang_plugin_init_v2`. `DangPluginV2` retains the complete ABI-v1 table as its
first member and adds `invoke`. Configuration-only ABI-v1 plugins remain
supported without recompilation.

Plugins that publish operational state use ABI v3 and export
`dang_plugin_init_v3`. `DangPluginV3` preserves the ABI-v2 prefix and adds
`get_operational_data`. The callback returns one self-contained XML data
element or fragment using `DangOperationalDataV1`; the bytes are borrowed and
copied before the callback returns. `dangd` parses each expanded data node as
partial instance data and rejects fragments with unknown schema nodes, invalid
shapes or scalar values, missing list keys, choice conflicts, invalid visible
references, or duplicate instances. After validating each fragment alone,
`dangd` validates each cumulative provider snapshot, seeded with daemon-owned
operational data, in plugin load order. Built-in data is authoritative and
earlier providers take precedence: if a provider
introduces a duplicate singleton, list-key collision, choice conflict, or
another deterministically invalid merge, its entire fragment is omitted.
Constraints that remain
indeterminate because providers supplied only selected data are not yet
enforced. Accepted data is merged into the read-only operational snapshot
before origin handling, NACM, and RFC 8526 filters. The callback must be
read-only, bounded, and safe to invoke for each retrieval. It must not return
configuration that has not actually been applied.

A failed callback or rejected fragment is omitted and reported under
`dangd-reconciliation:hardware-reconciliation/operational-provider-failure`.
The record identifies the plugin, callback, validation, or merge stage, best
available instance path, and reason. Providers should still log platform
failures locally; the telemetry describes the current retrieval and is not a
durable event log.

The IP-management example uses ABI v3 to publish RFC 8343
`/interfaces-state`, deriving `oper-status` from its last successfully applied
configuration. This remains simulated state: it does not inspect host network
interfaces.

Load plugins explicitly:

```sh
dangd --model appliance.yang --config config.xml \
  --plugin /usr/lib/dangd/plugins/example_plugin.so --check
```

Plugin initialization and model discovery happen before `dangd` accepts a
connection.

## Supplying YANG sources

`yang_source_count` and `yang_source_at` return `DangYangSourceV1` records.
Return complete immutable source bytes, not just a pathname. This keeps the
plugin relocatable and ensures that the schema being compiled is exactly the
schema being advertised.

Each source has one role:

- `DANG_YANG_IMPLEMENTED_V1`: the plugin implements protocol-visible behavior.
- `DANG_YANG_IMPORT_ONLY_V1`: definitions are needed only by imports/includes.
- `DANG_YANG_DEVIATION_V1`: deviations alter implemented schema conformance.

`module_name` and `revision` must agree with the declarations in the supplied
source. `source_uri` is optional provenance and is published as an RFC 8525
location. A URI does not replace the source bytes. `enabled_features` names
the features enabled for an implemented module; `dangd` rejects unknown names
and publishes the accepted set in YANG Library.

`dangd` copies every descriptor and source before completing plugin discovery.
It combines implemented plugin modules with core modules and the root model,
resolves the complete import/include closure, and compiles one effective
schema. Startup fails on invalid YANG, unresolved imports, incompatible
revisions, duplicate implementation ownership, or invalid deviations.

The resulting inventory is returned by NETCONF `<get>` under
`/ietf-yang-library:yang-library`. It includes implemented and import-only
modules, revisions, namespaces, submodules, locations, datastore mappings, and
a content identifier. `<get-config>` does not return this operational data.

The configured plugin paths are reloaded on POSIX `SIGHUP`. `dangd` stages
fresh shared-library images, compiles the complete replacement schema, and
validates the current running configuration against it before publication. A
failed reload leaves the current schema, plugins, and datastore untouched. A
successful reload changes the RFC 8525 content identifier, publishes
`yang-library-update`, and makes the new schema active for subsequent
sessions. Existing TLS sessions are notified and closed so they cannot keep
using the superseded schema negotiated in their server hello.

Every advertised module and submodule can be retrieved from the daemon with
the RFC 6022 `get-schema` operation in YANG format. The returned bytes are the
same immutable sources used for compilation.

## RPC and action dispatch

An ABI-v2 plugin's `invoke` callback receives `DangOperationV1` after `dangd`
has resolved the operation against the plugin's implemented YANG module and
completed NACM authorization and validated its input against the compiled
schema. `module_name` and `operation_name` identify the
schema node, `input_xml` is a self-contained request element, and
`instance_path` is non-null only for an action. For actions, `dangd` has also
verified read access to every ancestor data instance.

Return application output as one self-contained XML fragment through
`DangOperationResultV1.output_xml`. The string is borrowed only for the
duration of the callback and is copied immediately. `dangd` applies NACM read
filtering in the operation's output-schema context before serializing the RPC
reply. Invalid or incomplete output is rejected as a plugin failure. Return
zero and populate
`DangPluginErrorV1` for an application failure. A plugin that owns a module but
does not provide `invoke` receives `operation-not-supported` for that module's
RPCs and actions.

Authentication, NACM, schema dispatch, and NETCONF error serialization must
not be duplicated in the callback. The plugin should perform only the actual
module operation and return its result.

## Runtime dependencies

`dependency_count` and `dependency_at` name implemented modules whose provider
must precede this plugin. These are runtime dependencies, not ordinary YANG
imports. Importing a typedef or identity does not create a runtime dependency.

Dependencies serve two purposes:

1. A change to a dependency also marks the dependent plugin as affected.
2. Dependency providers are prepared and validated before their dependents;
   the hardware planner makes all provider actions prerequisites of every
   dependent action.

The initial ABI rejects missing providers and cyclic runtime dependencies.
Model relationships that form a cycle must therefore be validated from the
common proposed tree without declaring a cyclic apply dependency. ABI v4 also
permits finer ordering across modules. A dependency without `:` is local to the
declaring plugin. A fully qualified `plugin-name:action-id` dependency can name
another plugin's action; the target must be present in the same affected
transaction or planning fails.

## Transaction input

An affected plugin receives one `DangTransactionV1` containing:

- `before_xml`: the complete current running configuration.
- `proposed_xml`: the complete validated configuration that would become
  running.
- `changes_json`: exact schema-aware changes with module, path, kind, before,
  and after values.

All affected plugins receive the same immutable snapshots. A plugin should
read its own module data and any dependency data directly from
`proposed_xml`. It must not ask another plugin for a mutable configuration
copy. This is what makes cross-module validation deterministic.

Strings in the transaction are owned by `dangd` and remain valid through
`release`. A plugin must copy anything needed beyond that call.

## Transaction lifecycle

The lifecycle is:

```text
prepare every affected plugin
validate every affected plugin
collect and verify every hardware action plan
apply actions in dependency-safe order
release every prepared object
publish the proposed running datastore
```

### prepare

`prepare` parses relevant configuration, resolves references, calculates an
implementation plan, and may acquire temporary reservations. It returns an
opaque prepared object through `prepared`.

Preparation must not make externally visible configuration changes. Every
successful preparation must be releasable even if another plugin later fails.

### validate

`validate` checks the prepared plan against module-specific limitations such
as hardware capacity, unsupported combinations, or unavailable dependency
resources. Common YANG constraints have already been checked by `dangd`.

Validation must not mutate externally visible state. It may rely on
reservations made during preparation.

### apply and ABI v4 hardware actions

`apply` performs the exact retained plan. It must not silently reinterpret the
configuration or redo preparation against a different state. Successful apply
must leave the resource representing `proposed_xml`.

An apply operation should be idempotent wherever possible. The plugin must not
return success until its portion of the proposed state is durable enough to
satisfy its documented behavior.

ABI v1-v3 use `apply` and `rollback` as one action for the whole plugin. ABI v4
retains those callbacks for ABI compatibility but supplies
`hardware_action_count`, `hardware_action_at`, `apply_hardware_action`, and
`rollback_hardware_action` for actual commits. Each descriptor has:

- a stable, nonempty action ID unique within the prepared object;
- the affected schema instance path, when known;
- a normal, activate, or deactivate class;
- zero or more local or fully qualified prerequisite action IDs.

Descriptions are copied during planning; callback input remains borrowed.
Action callbacks receive the original local ID. Dangd adds generic safety
edges, rejects missing or cyclic dependencies, and executes a deterministic
topological order. All deactivations precede normal and activation work; all
activations follow other work. For ancestor paths, creation/update proceeds
parent first and deactivation proceeds child first. Plugins must still declare
backend-specific edges such as program-ACL before attach-ACL.

On action failure, only completed actions are rolled back, in reverse execution
order. A rollback failure produces `hardware-state-diverged`; successful
rollback preserves the previous running datastore. Validate dynamic capacities
and reserve any resources needed to ensure that apply will not race preflight.

### rollback

`rollback` is mandatory in ABI v1. If a later plugin fails to apply, `dangd`
calls rollback for already-applied plugins in reverse apply order. Rollback
uses the retained prepared object and must restore the state represented by
`before_xml`.

Confirmed commits are also sent through the backend when cancelled, expired,
or abandoned. The reverse datastore change is prepared, validated, and
applied like any other transaction. A plugin must therefore support both its
explicit rollback callback and an ordinary transaction whose proposed tree is
an earlier configuration.

If rollback itself fails, the plugin should return the most precise error it
can and preserve diagnostic state for reconciliation. `dangd` reports the
original apply failure together with every rollback failure and uses the
`hardware-state-diverged` NETCONF error app-tag. Production providers should
also log rollback failures through their platform facilities because device
state may require reconciliation. The core retains each failed compensation's
action ID, instance path, and reason in the modeled
`dangd-reconciliation:hardware-reconciliation` operational tree. A fully
successful later hardware transaction clears that report. Legacy ABI actions
have an empty path because their transaction-wide callback cannot identify a
more precise instance.

### release

`release` frees the prepared object and all reservations. It is called after a
successful transaction, after validation failure, after apply failure and
rollback, and when the host aborts preparation. It must be safe for every
object returned successfully by `prepare` and must not throw.

## Failure reporting

Return zero from a failing callback and populate `DangPluginErrorV1`:

- `message` describes the implementation failure in user-facing language.
- `instance_path` identifies the responsible YANG instance when known.

`dangd` converts the error into a NETCONF `operation-failed` response that
includes the plugin name, message, module context, and path. Returned strings
must remain valid until the callback returns.

Do not terminate the process, throw across the ABI, write partial error XML,
or modify the candidate/running datastore directly.

## Concurrency and reentrancy

Configuration callbacks execute under the serialized datastore transaction
boundary. A callback must not reenter `dangd`, issue NETCONF operations, or
wait for a request that requires the datastore lock. Operational-state and
notification callbacks may be added by later ABI versions with separate
threading rules.

Plugins are still responsible for synchronizing their own worker threads and
external callbacks. `destroy` is called only after prepared transactions have
been released.

## Security expectations

A plugin is native code in the `dangd` process and has the daemon's operating
system privileges. Load only trusted libraries. ABI version checks are not a
sandbox or signature mechanism.

Plugins must treat all configuration strings as untrusted input even though
they have passed schema validation. They should bound derived allocations,
avoid shell interpretation, validate external identifiers, and never include
secrets in error messages or logs.

NACM authorization has already succeeded before plugin preparation. Plugins
must not implement an independent, inconsistent authorization policy for the
same configuration nodes.

## Checklist

Before shipping a plugin, verify that it:

- supplies exact module names, revisions, source sizes, and roles;
- declares only genuine runtime dependencies;
- does no visible work during prepare or validate;
- retains one exact apply plan through the transaction;
- classifies activation and deactivation and declares every backend-specific
  action dependency when using ABI v4;
- rolls back every applied operation in reverse-safe form;
- handles a confirmed-commit reversal;
- reports a module path with actionable failures;
- frees every prepared object and reservation;
- is tested for prepare, validation, apply, rollback, and release failures;
- appears correctly in the RFC 8525 YANG Library response.
