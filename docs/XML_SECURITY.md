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
- subtree and XPath filter documents and the data passed through them;
- notification subscription filters, published event content, and instance data;
- host URL-provider configuration;
- plugin RPC/action output and operational-data fragments;
- persistent snapshot XML, through strict datastore import validation.

Generated error paths and XML text use XML escaping, while YANG Library and
other structured replies are constructed as XML nodes so the serializer escapes
values. NACM instance predicates use dedicated quote-selection rules and fail
closed when a value cannot be represented safely.

## Output and internal-reparse inventory

YANG Library, monitoring, notification IDs, and other structured daemon output
are built as XML nodes and serialized by pugixml. NETCONF replies that must be
assembled around protocol payloads pass every attribute and text value through
`EscapeXmlText`; malformed UTF-8, NULs, and forbidden XML characters become the
replacement character before markup escaping. Model retrieval is text content,
so its YANG/YIN source is escaped rather than interpreted as reply markup.

Raw XML embedded in an envelope is limited to `ConfigDocument::ToXml`, already
validated plugin operation output, or already validated notification content.
All core and daemon internal reparses use `ParseUntrustedXml`, even when their
producer is trusted. The RFC 8344 teaching plugin has one local reparse of
`active_configuration`; that string can only be assigned from dangd's validated
proposed snapshot after every hardware action succeeds and never crosses the
plugin ABI as unvalidated input.

Namespace declarations are resolved at each element during schema binding;
rebinding a prefix therefore changes the expanded name and cannot retain the
authority of its former namespace. Predicate values use dedicated quote rules,
and unrepresentable values fail closed. The deterministic protocol corpus
contains DTD/entity, multiple-root, CDATA, comment, processing-instruction, and
inert-XInclude seeds. This completes the code-level XML-injection inventory;
sustained fuzzing and independent interoperability remain separate release
evidence.
