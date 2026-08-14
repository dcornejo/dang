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

The schema and plugin set are fixed for the lifetime of the initial server
implementation. Installing or replacing a plugin requires restarting
`dangd`; live schema migration is not part of ABI v1.

## Runtime dependencies

`dependency_count` and `dependency_at` name implemented modules whose provider
must precede this plugin. These are runtime dependencies, not ordinary YANG
imports. Importing a typedef or identity does not create a runtime dependency.

Dependencies serve two purposes:

1. A change to a dependency also marks the dependent plugin as affected.
2. Dependency providers are prepared, validated, and applied before their
   dependents.

The initial ABI rejects missing providers and cyclic runtime dependencies.
Model relationships that form a cycle must therefore be validated from the
common proposed tree without declaring a cyclic apply dependency. If hardware
application needs finer ordering, the modules should be owned by one plugin
until a future operation-plan ABI is available.

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
apply each plugin in dependency order
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

### apply

`apply` performs the exact retained plan. It must not silently reinterpret the
configuration or redo preparation against a different state. Successful apply
must leave the resource representing `proposed_xml`.

An apply operation should be idempotent wherever possible. The plugin must not
return success until its portion of the proposed state is durable enough to
satisfy its documented behavior.

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
can and preserve diagnostic state for reconciliation. ABI v1 reports the
original apply failure to NETCONF; production providers should also log
rollback failures through their platform facilities. A future ABI may expose
a core reconciliation journal.

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
- rolls back every applied operation in reverse-safe form;
- handles a confirmed-commit reversal;
- reports a module path with actionable failures;
- frees every prepared object and reservation;
- is tested for prepare, validation, apply, rollback, and release failures;
- appears correctly in the RFC 8525 YANG Library response.
