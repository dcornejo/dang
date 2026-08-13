<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# NETCONF configuration editing

`yang/config_edit.h` applies core RFC 6241 node operations to an immutable,
schema-bound configuration. Inputs never change. Success returns a new validated
candidate and deterministic change events; failure returns NETCONF-oriented
errors and no candidate.

```cpp
#include <yang/config_edit.h>

auto edit = yang::config::ParseEditXml(runtime_schema, R"xml(
  <config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
    <interfaces xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces">
      <interface>
        <name>en0</name>
        <enabled>false</enabled>
      </interface>
    </interfaces>
  </config>)xml");

if (edit.document) {
  yang::config::ConfigEditor editor;
  auto result = editor.Apply(
      {runtime_schema, current_config, *edit.document,
       yang::config::EditOperation::kMerge});
  if (result.candidate) {
    std::string xml = result.candidate->ToXml();
  }
}
```

Per-node `nc:operation` accepts `merge`, `replace`, `create`, `delete`, and
`remove` and overrides the request default. Containers and leaves match by
expanded name, lists by typed key tuple, and leaf-lists by value. Selecting a
choice case removes explicitly stored nodes from competing cases.

The editor builds a separate mutable plan, compacts it into a new stable arena,
and validates the candidate. `create` reports `data-exists`; `delete` reports
`data-missing`; `remove` is idempotent. Failed existence checks or final YANG
validation discard the plan and change set.

Datastore orchestration—candidate/running/startup selection, locks, commit,
confirmed commit, `test-option`, and `error-option`—is provided by
`yang/netconf_datastore.h`. Ordered-by-user insertion attributes are supported.
Persistent storage, transport/session lifecycle, NACM, and editing partial
targets without a complete context remain embedding or future concerns.
