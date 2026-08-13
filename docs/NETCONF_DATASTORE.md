<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# NETCONF datastore transactions

`yang/netconf_datastore.h` supplies a thread-safe, in-memory transaction layer
over the immutable configuration documents. It models `running`, `candidate`,
and `startup`, including datastore locks, edit validation options, error
options, commit, confirmed commit, cancel, timeout rollback, discard, copy, and
startup deletion.

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
