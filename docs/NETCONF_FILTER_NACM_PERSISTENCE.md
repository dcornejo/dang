<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Retrieval filtering, NACM, and persistence

## Subtree filtering

`ApplySubtreeFilter` implements RFC 6241 subtree filtering and is used by
`get` and `get-config`. It supports namespace selection and wildcards,
attribute matches, containment nodes, selection nodes, content-match nodes,
and multiple list instances. An empty filter returns empty data.

`ApplyXPathFilter` implements the RFC 6241 `:xpath` capability using XPath 1.0.
Namespace bindings come from the `filter` element, the context is the
conceptual datastore root, and the result must be a node-set of data elements.
Selected subtrees are returned with their ancestor paths without duplication.
Malformed expressions, scalar results, missing `select`, and nonempty XPath
filter elements produce protocol-appropriate errors. NACM filtering runs
first, so XPath expressions cannot infer or select unreadable data.

## Access control

`NacmPolicy` provides ordered RFC 8341 group and rule-list evaluation for
CRUDX operations. Defaults match RFC 8341: read and ordinary
RPC execution are permitted, writes are denied, and `delete-config` is denied
unless a rule permits it. `close-session` is always permitted. Recovery users
bypass enforcement.

```cpp
yang::netconf::NacmPolicy nacm;
nacm.AddUserToGroup("alice", "operators");
nacm.AddRule({
    "permit-interface-updates", "operators", "",
    "/{urn:ietf:params:xml:ns:yang:ietf-interfaces}interfaces",
    static_cast<std::uint8_t>(
        yang::netconf::AccessMask(yang::netconf::AccessOperation::kRead) |
        yang::netconf::AccessMask(yang::netconf::AccessOperation::kUpdate)),
    yang::netconf::AccessAction::kPermit});

yang::netconf::NetconfServer server(stores, &nacm);
```

The standard NETCONF XML representation of the `ietf-netconf-acm` container
can be loaded directly:

```cpp
auto loaded = yang::netconf::LoadNacmPolicy(nacm_xml);
if (!loaded.policy) {
  // Report loaded.errors and retain the last known-good policy.
}
yang::netconf::NetconfServer server(stores, &*loaded.policy);
```

The loader supports global enable/default controls, configured groups,
ordered rule lists, wildcard groups, module rules, protocol-operation rules,
notification selectors, data-node paths, CRUDX bit sets, and actions.
Namespace-qualified data paths are compiled into expanded XML names. A host
can attach authenticated groups with `NetconfSession::set_external_groups`;
`enable-external-groups` controls whether they participate. Each RPC uses a
policy copy that remains stable for the complete request.

Data-node paths support RFC 8341 key and leaf-list predicates. A present key
predicate selects only the instance with the specified value; an omitted key
predicate matches every instance of that list. Matching compares expanded
names and complete path segments, so similarly prefixed names such as
`interface` and `interfaces` cannot match accidentally. In XML NACM
configuration, node and key names require explicit namespace prefixes, as
required for instance-identifiers by RFC 7950 Section 9.13.2. This follows the
resolution recorded for rejected RFC 8341 Erratum 6493.

`AuthorizeNotification` applies the same ordered group/module/name rules to
event delivery. Denied RPC executions, data writes, and notifications update
thread-safe RFC 8341 counters exposed by `NacmPolicy::counters()`. Per-request
policy copies share the counter state, so snapshot isolation does not lose
operational accounting.

RPC execution is checked before dispatch. Proposed edit changes are checked
inside the datastore transaction before publication. Read-denied nodes are
silently removed. Recovery-session identification remains a host decision.
For a YANG 1.1 action, authorization of readable ancestors and execute access
precedes parent-instance resolution. An authorized request must name every list
key and the selected ancestor instance must exist in the operational view;
otherwise dispatch fails with `missing-element` or `data-missing`. Performing
authorization first prevents the different existence errors from becoming a
datastore probe for an unauthorized user.
The compiler lowers `nacm:default-deny-all` and `nacm:default-deny-write` into
effective runtime-schema flags inherited by descendants. These flags apply
after ordered explicit rules, allowing an explicit permit to grant access as
required by RFC 8341.

The runtime-schema builders preserve these annotations from both compiled
YANG and YIN. The YIN adapter recognizes the two standard no-argument RFC 8341
extension elements through the imported `ietf-netconf-acm` namespace; other
application-defined YIN extensions still require the full YANG compilation
path because their argument metadata is not available to the adapter.

Edits, candidate commits, and datastore or URL `copy-config` replacements use
a schema-aware before/after diff. Every affected instance is authorized before
any new tree is published or URL provider write is invoked. When NACM is
enabled, an existing URL target must be readable and schema-valid so removed
nodes can also be checked; inability to inspect it fails closed. Notification
transport and delivery remain part of the separate RFC 5277 task, while the
NACM notification authorization decision is available to that adapter.

RFC 8341 does not require permission for changes that are side effects of
validating an explicitly authorized edit. The editor therefore marks nodes
removed only because another `choice` case became active or a `when` expression
became false. Those implicit deletions are omitted from the NACM authorization
set. They remain in the complete before/after delta supplied to plugins and the
running backend, so application, rollback, persistence, and diagnostics still
describe the device's complete resulting change.

## Durable snapshots

`SaveDatastoreSnapshot` writes versioned JSON containing running, candidate,
startup, and confirmed-commit recovery state. It writes a same-directory
temporary file, flushes and synchronizes it, atomically renames it, and
synchronizes the containing directory on macOS/POSIX systems.

```cpp
using yang::netconf::LoadDatastoreSnapshot;
using yang::netconf::SaveDatastoreSnapshot;

SaveDatastoreSnapshot("/var/lib/my-server/netconf.json", stores);

// During restart, after constructing the manager with the same schema:
auto loaded = LoadDatastoreSnapshot(
    "/var/lib/my-server/netconf.json", stores);
```

Restore parses and validates stored running, startup, and rollback trees before
changing live state. Candidate is parsed but may intentionally remain invalid
after a NETCONF `test-option` of `set`. Locks are never persisted. Confirmed
commits retain their wall-clock expiration across downtime; an already expired
snapshot rolls running back during restore.

The host decides when to snapshot, file ownership and permissions, encryption,
backup rotation, and how to react to storage failure. A production server
should save after every successful persistent state transition.
