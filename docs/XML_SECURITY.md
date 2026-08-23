<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# XML security contract

Untrusted XML is parsed through `ParseUntrustedXml`. The helper enforces the
configured byte, node, and depth limits; validates every byte as UTF-8 and every
decoded code point as an XML 1.0 character; rejects DTD and entity declarations;
and normally requires exactly one document element. It invokes pugixml with
`parse_default` and explicit UTF-8 encoding. External entities are therefore
never resolved and XInclude is never processed.

Comments, CDATA sections, and processing instructions are accepted as inert XML
syntax. They do not create modeled configuration nodes. An `xi:include` element
is an ordinary namespaced element and is subsequently rejected wherever it is
not present in the active schema. Edit payloads are the one intentional
exception to the single-document-element policy because the internal edit
representation may contain several top-level configuration elements; each is
still subject to the same encoding, declaration, and resource checks.

## Boundary inventory

The strict helper currently protects:

- NETCONF client `<hello>` and `<rpc>` messages;
- complete datastore configuration and edit fragments;
- datastore-managed and file-loaded NACM policy;
- subtree and XPath filter documents and the data passed through them.

Generated error paths and XML text use XML escaping, while YANG Library and
other structured replies are constructed as XML nodes so the serializer escapes
values. NACM instance predicates use dedicated quote-selection rules and fail
closed when a value cannot be represented safely.

The remaining audit covers notification filters and event payloads, URL-source
documents, plugin RPC/action output, plugin operational fragments, persistence
snapshots, and every internal reparse of generated XML. Those paths must either
adopt the helper or document why their producer is trusted, and need boundary
regressions plus fuzz seeds before the project closes the XML-injection TODO.
