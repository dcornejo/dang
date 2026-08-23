<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# RFC 8341 NACM compliance matrix

This executable-evidence index follows RFC 8341 section order. A listed test is
evidence for the stated behavior, not a substitute for independent
interoperability testing. Open items remain in [TODO.md](../TODO.md); the
high-level status and variance statement is in [COMPLIANCE.md](COMPLIANCE.md).

## Sections 3.1–3.3: model and policy controls

- User, configured-group, multiple-group, wildcard-group, module, rule-type,
  operation-bit, action, and ordered-first-match behavior:
  `NacmTest.LoadsOrderedIetfNetconfAcmConfiguration`,
  `NacmTest.AppliesOrderedGroupRulesAndDefaults`, and
  `NacmTest.AppliesWildcardGroupsModulesNamesAndCrudxBitsInOrder`.
- Recovery-session bypass: `NacmTest.RecoveryUsersBypassRules` and
  `NacmTest.PreservesRecoveryIdentityWhenManagedPolicyChanges`. Recovery and
  disabled-enforcement read-filter bypasses still enforce XML syntax, size,
  node-count, depth, one namespace-valid `<data>` document envelope, and any
  explicitly supplied runtime schema.
- Host recovery identities must be unique canonical UTF-8 names. Empty,
  oversized, padded, control-containing, embedded-NUL, malformed UTF-8, and
  duplicate values fail before privilege is installed or `dangd` starts:
  `NacmTest.RecoveryUsersBypassRules` and
  `DangdApplicationTest.RejectsUnsafeOrDuplicateRecoveryUsers`.
- `enable-nacm`, `read-default`, `write-default`, `exec-default`, and denial
  accounting while enforcement is disabled:
  `NacmTest.DisabledEnforcementBypassesRulesWithoutCountingDenials` and
  `NacmTest.CountsDeniedOperationsWritesAndNotifications`.
- `enable-external-groups`: `NacmTest.HonorsExternalGroupsSwitch`.
- `default-deny-all` and `default-deny-write` inheritance and explicit-rule
  precedence: `NacmTest.EnforcesDefaultDenyAnnotationsAfterExplicitRules`,
  `ConfigValidationTest.PropagatesNacmDefaultDenyAnnotations`, and
  `ConfigValidationTest.PreservesNacmAnnotationsFromYin`.
- Model loading rejects malformed switches, operations, selectors, duplicate
  names, duplicate singleton leaves, foreign/config-false elements, invalid
  group wildcards, duplicate group membership/selectors/operation bits, and
  missing actions. It also requires exactly one top-level NACM container so no
  sibling policy content is silently ignored:
  `NacmTest.RejectsMalformedModelConfiguration` and
  `NacmTest.RejectsInvalidModelShapeAndLeafListValues`.
- Every pairwise and three-way combination of `rpc-name`, `notification-name`,
  and `path` is rejected according to the model's `rule-type` choice:
  `NacmTest.RejectsEveryMultipleRuleTypeCombination`.
- A user matching several configured groups and ordered rule-lists receives the
  first matching rule in global model order, while lists for nonmember groups
  are skipped: `NacmTest.PreservesRuleListOrderWhenSeveralGroupsMatch`.
- XML shape validation permits namespace declarations but rejects unmodeled
  attributes at every depth and non-whitespace character content in structural
  containers: `NacmTest.RejectsUnmodeledXmlAttributesAndContainerText`.
- Explicit empty YANG `bits`, RPC-name, notification-name, and module-name
  values retain their distinct no-bit/no-match meaning instead of broadening to
  wildcard access: `NacmTest.DoesNotBroadenExplicitlyEmptyLexicalValues`.
- Managed bootstrap and datastore policy replacement:
  `DangdApplicationTest.UsesSecureNacmDefaultsWhenSubtreeIsAbsent`,
  `DangdApplicationTest.SeedsAndCommitsDatastoreManagedNacm`, and
  `DangdApplicationTest.LoadsNacmAndUsesAuthenticatedSessionIdentity`.
- Policy replacement is synchronized with snapshots taken by several active
  sessions, preventing requests from observing fields from different policy
  generations: `NetconfServerTest.ReplacesPolicySafelyAcrossConcurrentSessions`.

No open core-server evidence item remains for model and policy controls.

## Section 3.4.1: initial operation

- Absent managed NACM configuration permits reads and operations but denies
  ordinary writes, while recovery users can seed policy:
  `DangdApplicationTest.UsesSecureNacmDefaultsWhenSubtreeIsAbsent` and
  `DangdApplicationTest.SeedsAndCommitsDatastoreManagedNacm`.
- Server-initiated initial configuration loading is schema validated without
  being blocked by the not-yet-installed policy: application load tests.
- When persistence is configured and no snapshot exists, `dangd` durably saves
  the validated initial datastores and seeded managed NACM before startup
  succeeds. Fault injection at every atomic-save milestone proves restart sees
  either no snapshot and reseeds safely, or a complete enforceable snapshot:
  `DangdApplicationTest.PersistsManagedNacmBeforeFirstBootCompletes`.

No open core-server evidence item remains for initial operation.

## Section 3.4.2: session establishment

- Authenticated usernames remain separate from NETCONF session identifiers and
  transport groups are passed without policy-side invention:
  `DangdApplicationTest.LoadsNacmAndUsesAuthenticatedSessionIdentity`,
  `NetconfTransportTest` identity tests, and
  `NacmTest.HonorsExternalGroupsSwitch`.
- TLS certificate mapping requires exactly one bounded, nonempty UTF-8 common
  name and rejects duplicate names, surrounding whitespace, controls, embedded
  NUL, missing names, and failed certificate verification:
  `DangdTlsTransportTest.MapsOnlyOneCanonicalCertificateCommonName` and mutual
  TLS integration tests.
- TLS deployments may explicitly select common name, DNS SAN, or URI SAN as the
  username source. SAN selection requires exactly one safe value of the chosen
  type and ignores other SAN types:
  `DangdTlsTransportTest.SelectsExactlyOneConfiguredSanIdentity`.
- A transport-neutral exact mapper converts authenticated identities to local
  NACM usernames. Invalid or duplicate rules fail closed, and deployments may
  reject every unmapped identity. `dangd` applies it after verified TLS field
  extraction: `NetconfTransportTest.MapsAuthenticatedUsernamesExactlyAndFailClosed`.
- Transport-supplied external groups are accepted only with an explicit trusted
  authentication-provenance assertion. Empty, oversized, control-containing,
  and duplicate group values are rejected, and simultaneous sessions retain
  independent group sets:
  `NetconfTransportTest.RequiresTrustedCanonicalExternalGroups` and
  `NetconfTransportTest.KeepsTrustedGroupsIsolatedBetweenSessions`.
- Every RPC attempted by a recovery identity invokes a host audit sink before
  XML parsing. Records contain the session ID, authenticated username, and byte
  count but no payload or policy details; ordinary identities do not emit them:
  `NetconfServerTest.AuditsEveryRecoveryUserRpcAttempt` and
  `DangdApplicationTest.EmitsSafeRecoveryAuditRecords`.
- `dangd` embeds libssh, loads an explicit username/public-key authorization
  table, maps only the authenticated username through the shared exact mapper,
  and derives trusted external groups only from that same local authorization
  record. It rejects unauthorized keys and every subsystem other than exact
  `netconf`: `DangdSshTransportTest.AuthenticatesPublicKeyRequiresNetconfAndExchangesRpc`.

No open core-server or `dangd` SSH integration item remains for session
establishment. A manual OpenSSH public-key authentication, `netconf` subsystem,
RPC, and clean-close smoke interaction passed on 2026-08-23 using the documented
command. A broader independent interoperability matrix remains release evidence.

## Section 3.4.3: access-denied errors

- Denied RPC execution and writes return NETCONF `access-denied` without
  publishing state: `NetconfServerTest.EnforcesNacmBeforePublishingWrites`,
  `NetconfServerTest.AuthorizesUrlTargetReplacementBeforeWriting`,
  `NetconfDatastoreTest.AuthorizesExactCommitAndCopyChangesAtomically`, and
  `DangdApplicationTest.LoadsNacmAndUsesAuthenticatedSessionIdentity`.
- Protocol-operation denials use `error-type` `application`, identify the
  requested standard or schema-defined operation with a namespace-correct
  `error-path`, and include no `error-info`:
  `NetconfServerTest.NacmRpcDenialIdentifiesNetconfOperation` and
  `NetconfServerTest.AuthorizesAndDispatchesSchemaRpc`.
- A table-driven matrix denies every supported standard operation before body
  dispatch, covering NETCONF base, RFC 5277 notifications, RFC 6022 monitoring,
  and RFC 8526 NMDA; each denial increments the operation counter, while
  `close-session` remains unconditionally allowed:
  `NetconfServerTest.DeniesEverySupportedStandardOperationBeforeDispatch`.
- Data-write denials serialize internal expanded instance names and keyed
  predicates as namespace-bound NETCONF XPath rather than exposing the
  implementation's `{namespace}name` notation:
  `NetconfServerTest.EnforcesNacmBeforePublishingWrites`.
- NACM error messages remain generic and do not repeat internal expanded paths,
  rejected non-key values, rule names, or policy details. The required
  namespace-bound `error-path` remains present, including requester-supplied
  list keys: `NetconfServerTest.EnforcesNacmBeforePublishingWrites`.

No open core-server evidence item remains for access-denied serialization.

## Section 3.4.4: incoming RPC validation

- Ordered module/RPC/exec matching, defaults, `default-deny-all`, unconditional
  `close-session`, and special default denial of `delete-config` and
  `kill-session`: `NacmTest.DeniesSensitiveRpcByRuleAndDeleteConfigByDefault`,
  `NacmTest.EnforcesDefaultDenyAnnotationsAfterExplicitRules`, and server RPC
  dispatch tests.
- YANG 1.1 actions require readable ancestors, execute permission, a complete
  keyed path, and an existing operational parent before dispatch:
  `NacmTest.AppliesRfc8341ActionDecisionSequence`,
  `NetconfServerTest.RequiresReadableAncestorsBeforeDispatchingAction`, and
  `NetconfServerTest.RequiresActionParentInstanceAndCompleteListKeys`.
- One immutable policy copy governs an RPC and its data operations; counters are
  shared with the live policy:
  `NetconfServerTest.UsesOneNacmSnapshotForEntireRpcDuringPolicyReplacement`.

No open core-server evidence item remains for policy snapshot isolation.

## Section 3.4.5: data-node access validation

- Read-denied data is silently omitted before subtree/XPath selection:
  `NacmTest.SilentlyFiltersDeniedReadSubtrees`,
  `NacmTest.FiltersSpecificKeyedListInstances`, and NETCONF filter tests.
  Missing/incorrect envelopes and multi-root input fail closed so neither the
  document root nor a sibling root escapes traversal. Envelope namespaces are
  limited to NETCONF, NMDA, and the internal unqualified representation.
- Schema-aware read filtering fails closed for elements that cannot be resolved
  in the advertised runtime schema, preventing unknown data from bypassing
  module rules or inherited NACM annotations. Schema pruning remains active for
  recovery and disabled enforcement even though authorization rules do not:
  `NetconfServerTest.OmitsUnmodeledSchemaAwareNacmReadData`.
- Expanded names, complete path segments, descendants, list keys, omitted-key
  wildcards, and non-textual-prefix matching:
  `NacmTest.MatchesKeyedPathsAndTreatsMissingKeysAsWildcards` and
  `NacmTest.DoesNotUseTextualPathPrefixes`. Quoted predicate values may contain
  closing brackets without prematurely terminating the predicate:
  `NacmTest.AcceptsClosingBracketInsidePathPredicateLiteral`. Schema-aware read
  paths use exactly the declared keys in model order and remove entries with
  missing or duplicate keys:
  `NetconfServerTest.OmitsUnmodeledSchemaAwareNacmReadData`.
- Read/create/update/delete bit selection and first-match ordering:
  `NacmTest.AppliesWildcardGroupsModulesNamesAndCrudxBitsInOrder` and
  `NetconfServerTest.MapsEffectiveEditConfigChangesToCrudBits` and
  `NetconfServerTest.MapsEffectiveEditDataChangesToCrudBits`. The protocol
  tests verify keyed create/delete, scalar update, wrong-bit denial, and
  unchanged target state after denial for both classic `<edit-config>` and
  RFC 8526 `<edit-data>`.
- An existing `ordered-by user` list or leaf-list instance whose relative
  position changes is represented as an effective move and requires NACM
  `update` access. Moves are included in backend deltas and a denial preserves
  the target order: `ConfigEditTest.ReportsMinimalOrderedByUserMoves` and
  `NetconfServerTest.RequiresNacmUpdatePermissionForOrderedMove`.
- Leaf-list values use RFC 7950 self predicates in their instance paths, so
  rules can independently filter reads and authorize creation or deletion of a
  particular value. Apostrophes select a double-quoted predicate literal:
  `NetconfServerTest.AuthorizesSpecificLeafListInstances`. Values containing
  both quote forms fail closed because NACM paths prohibit the function needed
  to concatenate an XPath 1.0 literal. Ordinary containers never acquire
  predicates from their scalar children.
- Virtual schema defaults do not create datastore write events or phantom NACM
  checks. Explicitly storing a default-valued node requires `create`, and
  removing that explicit node requires `delete`, even though the effective
  value remains available: `NetconfServerTest.AuthorizesExplicitDefaultsWithoutPhantomWrites`.
- Exact edits, candidate commit, inline/URL replacement, and copy operations are
  authorized atomically before publication. Implicit `choice` and `when`
  removals do not demand separate permission:
  `NetconfDatastoreTest.AuthorizesExactCommitAndCopyChangesAtomically`,
  `NetconfDatastoreTest.DoesNotAuthorizeImplicitChoiceSideEffect`, and
  `NetconfDatastoreTest.DoesNotAuthorizeImplicitWhenSideEffect`.
- Confirmed-commit cancellation and timeout restore the server-held rollback
  snapshot even after the live policy changes to deny writes. Restoration is
  server-initiated and does not increment session denied-write counters:
  `NetconfServerTest.DoesNotApplySessionNacmToConfirmedCommitRollback`.
- Datastore-source `<copy-config>` silently omits read-denied nodes before exact
  target replacement, while running-to-startup requires only operation execute
  permission as specified by RFC 8341 Section 3.2.6:
  `NetconfServerTest.AppliesNacmCopyConfigSourceAndStartupSpecialCase` and
  `NetconfServerTest.CopiesCompleteInlineConfigurationAtomically`.
- URL combinations apply NACM to each datastore operand: datastore sources are
  read-filtered before provider writes, and URL sources are fully authorized
  against datastore targets before atomic publication:
  `NetconfServerTest.AppliesNacmToDatastoreSidesOfUrlCopies` and
  `NetconfServerTest.AuthorizesUrlTargetReplacementBeforeWriting`.
- Remote-to-remote provider failures preserve precise error tags/messages and
  target content. Source-read and target-inspection failures occur before a
  write; an atomic provider write failure also leaves the target unchanged.
  These transport failures do not increment NACM denial counters:
  `NetconfServerTest.FailsRemoteCopiesBeforeTargetMutation`.
- Denied write accounting: `NacmTest.CountsDeniedOperationsWritesAndNotifications`.

No open core-server data-node evidence item remains. External interoperability,
host-provider authorization policy, and long-running concurrency evidence remain
integration responsibilities.

## Section 3.4.6: outgoing notification authorization

- Top-level notification module/name/read matching, defaults, recovery bypass,
  and denied-notification counters:
  `NacmTest.AppliesOrderedNotificationRulesAndRecoveryBypass` and
  `NacmTest.CountsDeniedOperationsWritesAndNotifications`.
- Data-associated notifications require concrete existing keyed ancestors and
  readable access to each ancestor, and are authorized per subscription before
  replay or live queueing:
  `NacmTest.AppliesRfc8341DataAssociatedNotificationDecisionSequence`,
  `NetconfNotificationsTest.DerivesAssociatedNotificationAncestorsFromSchema`,
  and `NetconfNotificationsTest.AppliesNacmBeforeQueueing`.
- Schema-aware publication binds the claimed module and notification name to
  both the modeled XML event root and any associated instance path before NACM
  authorization or queueing. The complete event body is first checked for
  modeled children, scalar types, mandatory nodes, singleton duplication, and
  list/leaf-list cardinality, required list keys, duplicate key tuples, and
  duplicate leaf-list values, plus conflicting or absent mandatory choices:
  `NetconfNotificationsTest.BindsTopLevelPublicationToModeledIdentity` and
  `NetconfNotificationsTest.DerivesAssociatedNotificationAncestorsFromSchema`.
- Each replay subscription and live multi-session fanout uses one immutable
  policy snapshot. Concurrent replacement therefore cannot split one operation
  across policy generations:
  `NetconfNotificationsTest.UsesOnePolicySnapshotForReplayAndLiveFanout`.

Open evidence: an external RFC 5277/NACM interoperability run.

## Sections 3.5 and 5: model and security considerations

- The pinned normative `ietf-netconf-acm@2018-02-14.yang` is compiled, exposed
  through YANG Library and `get-schema`, and used to validate managed policy
  configuration before `LoadNacmPolicy` constructs the runtime policy.
- Denial counters are core-owned operational data. Recovery identities and
  counters survive managed policy replacement.
- Resource limits and malformed NACM XML/path inputs fail closed. The protocol
  fuzz target combines arbitrary NACM XML with attacker-controlled expanded
  instance paths, then exercises read/create/update/delete authorization and
  readable-data filtering. Its corpus includes keyed and leaf-list predicates,
  namespaces, apostrophes, and mixed quote forms.
- NETCONF transport negotiation and RPC dispatch each require exactly one XML
  document element, preventing a second top-level `<hello>` or `<rpc>` from
  being silently ignored: `NetconfFramingTest.ClosesOnMalformedHelloAndFraming`
  and `NetconfServerTest.RejectsMalformedRpcAndInvalidOptions`.
- Complete datastore XML parsing likewise rejects sibling document roots before
  schema binding, so persisted, URL-sourced, or inline configuration cannot
  differ from the tree that is validated and authorized:
  `ConfigValidationTest.ReportsMalformedXmlAndMissingContext`.

Live managed-policy commits are transactionally coupled to snapshot
persistence. Fault injection before and after atomic replacement verifies that
the prior durable snapshot, running tree, backend, and NACM policy are restored
before an `operation-failed` reply is returned.

Open evidence: broader independent SSH interoperability and sustained concurrent
transport testing, the end-to-end XML injection and broader security reviews,
sustained coverage-guided fuzzing release results, and independent
interoperability. No full RFC 8341 compliance claim is made until those TODO
items and every open entry above are closed.
