<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# NETCONF datastore transactions

`yang/netconf_datastore.h` supplies a thread-safe, in-memory transaction layer
over the immutable configuration documents. It models `running`, `candidate`,
`startup`, and the read-only RFC 8342 `intended` datastore, including datastore
locks, edit validation options, error
options, commit, confirmed commit, cancel, timeout rollback, discard, copy, and
startup deletion.

The initial NMDA implementation treats `intended` as a logical read-only view
of `running`, as RFC 8342 permits for systems without configuration
transformations. Reads and validation are supported. Lock, edit, copy-target,
and delete operations reject `intended`; a later transformation boundary can
give it independent contents without changing callers of `Read(kIntended)`.

`dangd` exposes conventional datastores through RFC 8526 `<get-data>`, with
datastore identity selection, subtree or XPath selection, `config-filter`,
`max-depth`, and NACM read filtering. RFC 8526 `<edit-data>` supports inline
configuration for writable conventional datastores and always uses atomic
rollback-on-error behavior. URL content is unavailable because the core NMDA
module does not enable the NETCONF `url` feature.
The initial read-only `operational` snapshot combines applied intended
configuration with core YANG Library, NETCONF monitoring, and NACM state.
Schema-aware `config-filter` processing can select its configuration or state
portion while retaining required ancestor shells and list keys.
Every applied configuration node carries explicit `ietf-origin:intended`
metadata when requested. More specific origin metadata supplied by an ABI-v6
applied-state source is schema-validated and preserved, while config-false
state is never assigned a fabricated configuration origin. The RFC 8526
`origin` feature is advertised;
`with-origin`, positive origin filters, and negated origin filters expose or
select that metadata without affecting config-false system state. Filter
identityrefs are resolved by namespace and use transitive identity derivation;
repeated values have union semantics, and invalid or conflicting selections
return `invalid-value`. Standard and vendor-derived non-intended origins use
the same filtering path, including inherited parent annotations. Origin
metadata is removed from ordinary `<get>` and from `<get-data>` unless
`with-origin` is present.

```cpp
#include <chrono>
#include <yang/netconf_datastore.h>

yang::netconf::DatastoreManager stores(runtime_schema, initial_config);
stores.Lock(yang::netconf::Datastore::kCandidate, "session-17");

auto parsed = yang::config::ParseEditXml(runtime_schema, edit_xml);
if (parsed.document) {
  yang::netconf::EditConfigRequest request;
  request.session = "session-17";
  request.target = yang::netconf::Datastore::kCandidate;
  request.edits.push_back(*parsed.document);
  request.test_option = yang::netconf::TestOption::kTestThenSet;
  request.error_option = yang::netconf::ErrorOption::kRollbackOnError;

  if (stores.EditConfig(request).ok) {
    yang::netconf::ConfirmedCommitOptions confirmed;
    confirmed.timeout = std::chrono::seconds(300);
    confirmed.persist = "deployment-42";
    stores.Commit("session-17", confirmed);
  }
}

// A host event loop supplies time; no background thread is created.
stores.ProcessTimeouts();
// Or make the change permanent from any session using the persist token.
stores.ConfirmCommit("session-99", "deployment-42");
```

`test-then-set` validates before storage, `test-only` returns the result without
changing the datastore, and `set` performs structural edit operations without a
final YANG validation pass. `stop-on-error` preserves successful earlier edits,
`continue-on-error` attempts later payloads, and `rollback-on-error` restores
the transaction's initial document.

`Commit`, `CopyConfig`, and `EditConfigRequest` accept the same optional
per-change authorization callback. Changes are calculated by a deterministic,
schema-aware comparison of the old and proposed complete trees, including list
keys and leaf-list values. A denial returns `access-denied` with the affected
instance path and leaves the target unchanged.

The default edit operation accepts `merge`, `replace`, or `none`. With `none`,
unannotated ancestor elements only select existing hierarchy; explicit
per-node `nc:operation` metadata performs the actual changes.

Locks are owned by an explicit session string. Reads remain available while a
datastore is locked. `CloseSession` releases its locks and rolls back its
non-persistent confirmed commit; a persistent confirmed commit survives. The
manager deliberately does not create timer threads,
persist files, transport NETCONF RPCs, or authenticate sessions; those belong
to the embedding server. Call `ProcessTimeouts` from its event loop and persist
the returned datastore documents using the server's storage policy.

An embedding server can provide a `RunningConfigBackend` to the constructor.
It receives the previous and replacement documents plus the exact,
schema-aware `ChangeEvent` delta whenever `running` is replaced by an edit,
commit, copy, confirmed-commit continuation or rollback, timeout, session
cleanup, or snapshot restore. The callback runs under the datastore lock and
must not call back into the same manager. The base interface is an infallible
publication boundary; applications that configure fallible external systems
should stage and validate those actions before invoking the datastore change.

## Ordered-by-user editing

The edit parser accepts RFC 7950 insertion attributes in
`urn:ietf:params:xml:ns:yang:1` for lists and leaf-lists declared
`ordered-by user`:

```xml
<interface xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces"
           xmlns:yang="urn:ietf:params:xml:ns:yang:1"
           yang:insert="before" yang:key="[name='eth2']">
  <name>eth1</name>
</interface>
```

`first` and `last` take no anchor. `before` and `after` require `yang:key` for a
list or `yang:value` for a leaf-list. The anchor must identify an existing
sibling. Insertion attributes on system-ordered collections are rejected.

The key selector parser supports the RFC predicate form with single- or
double-quoted key values. Optional prefixes are resolved using the namespace
bindings in scope on the edited list entry and matched as expanded names.
