<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 6243 with-defaults

`NetconfServer` can opt into RFC 6243 retrieval behavior. Datastores retain
only explicit configuration; schema defaults are synthesized in a read-only
effective view for retrieval and never written back to running, candidate, or
startup.

```cpp
yang::netconf::NetconfServer server(
    datastores, nacm, urls, notifications,
    yang::netconf::WithDefaultsConfig{});
```

Enabling the configuration selects RFC 6243 `explicit` as the basic mode and
advertises `report-all`, `report-all-tagged`, and `trim` as additional modes.
Without it, the capability is not advertised and a supplied `with-defaults`
parameter is rejected with `operation-not-supported`.

The supported request parameter is the `with-defaults` element from the
`urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults` namespace:

```xml
<get-config xmlns="urn:ietf:params:xml:ns:netconf:base:1.0">
  <source><running/></source>
  <with-defaults
      xmlns="urn:ietf:params:xml:ns:yang:ietf-netconf-with-defaults">
    report-all-tagged
  </with-defaults>
</get-config>
```

- `explicit` reports client-supplied nodes, including explicitly supplied
  values equal to their schema defaults.
- `trim` omits explicit scalar nodes whose value equals the schema default.
- `report-all` adds defaults that are in use in the effective data tree.
- `report-all-tagged` additionally marks synthesized defaults with
  `wd:default="true"`.

Default expansion happens before NACM and subtree/XPath filtering. A default
that is not readable is therefore removed by NACM just like an explicit node,
and filters can select synthesized defaults without bypassing access control.
Virtual defaults are not stored configuration and do not generate NACM write
checks during unrelated edits. Explicitly supplying a default-valued node is a
configuration creation, however, and removing that explicit node is a deletion;
both use the corresponding NACM write permission.
