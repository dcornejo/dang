<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Peer transaction coordination

Some configurations describe a service implemented by multiple independent
dangd servers. A Kea hot-standby pair is the first concrete case: changing one
member without the other can interrupt service or leave the pair with
incompatible configuration. A local plugin transaction cannot make two dangd
processes atomic, and ordinary sequential NETCONF commits cannot distinguish a
lost reply from a failed mutation.

`PeerTransactionCoordinator` provides the transport-neutral state machine for
this boundary. ABI v9 now provides the generic plugin planning and verification
contract, including isolated-worker transport, while the authenticated
participant transport and automatic startup recovery are also implemented.
Dangd does not advertise pair-wide commit support yet because the final
datastore commit entry point, endpoint resolution, policy, and observability
are not connected.

## Plugin planning contract

Plugins never open peer connections. For each affected prepared transaction,
ABI v9 returns a set of stable group/participant identities, participant roles,
confirmed-commit timeouts, complete module-scoped `<config>` images, and opaque
JSON verification contexts. Dangd generically composes contributions from
several plugins. It requires the same complete module set on every participant,
one owner for each module image, consistent roles/timeouts, at least two peers,
and exactly one primary. Every fragment is checked against its claimed module;
the combined candidate is then validated against the full schema so cross-
module dependencies are evaluated only with the complete changed tree.
The ordinary backend preparation path performs this collection and composition
immediately after every affected plugin has prepared and validated. A callback
failure or invalid composed plan aborts all retained plugin state before any
local or remote mutation. Validated groups remain attached to that preparation
for the future coordinator invocation; they are discarded on abort or after
the current local apply path completes.

After authenticated readback, dangd routes the running and operational replies
to each contributing plugin's verifier together with only that plugin's opaque
context. The core retains exclusive ownership of endpoint mappings, trust,
credentials, sessions, NETCONF ordering, journaling, and recovery. This is the
only supported seam: plugins may not depend on daemon-private APIs or require
module-specific logic in dangd.

## Required participant operations

Each peer supplies idempotent callbacks that an eventual NETCONF adapter can
map as follows:

1. `prepare`: lock and edit the candidate, then run `<validate>` without
   changing running configuration;
2. `apply_confirmed`: issue a persistent confirmed commit with a bounded
   rollback timeout;
3. `verify`: read back authoritative configuration and service health;
4. `confirm`: confirm the persistent commit;
5. `cancel`: cancel the confirmed commit before a durable commit decision; and
6. `release`: unlock and release candidate/session resources.

The participant identity must be stable across restart. Callbacks must be
idempotent because a reply can be lost after the remote operation succeeds.
In particular, an apply error is ambiguous until reconciled: before the durable
commit decision, the coordinator sends cancellation to that attempted peer as
well as every earlier peer. Cancellation must therefore also succeed harmlessly
when the apply did not take effect. A failed prepare is included in resource
release because it may already have acquired a remote lock.

## State and ordering

```text
prepare every peer
        |
        v
apply standbys, then primary
        |
        v
verify configuration and pair health
        |
        v
durably record COMMIT decision
        |
        v
confirm every peer --------> retry pending confirmations after restart
        |
        v
durably record COMPLETE
```

Before `COMMIT` is durably recorded, any failure cancels every already-applied
confirmed commit in reverse order. A failed cancellation is reported as an
unresolved rollback instead of being hidden. If the journal cannot record the
decision, the transaction also rolls back.

After `COMMIT` is durable, cancellation is forbidden. A missing or failed
confirmation becomes `commit-pending`; recovery replays confirmation only for
the peers not already durably acknowledged. Each acknowledgement is atomically
recorded before it is reported as confirmed. If every peer confirms but the
final journal update fails, the result remains pending solely for journal cleanup.
This separation prevents a lost confirmation reply from causing one process to
roll back after the group has already chosen commit.

Prepare uses a stable participant-id order so concurrent coordinators can use
the same lock order. Apply and verification place every standby before the
single primary to preserve service where the provider supports that ordering.
The coordinator rejects duplicate identities and groups without exactly one
primary.

`PeerTransactionFileJournal` implements the private recovery record. Its
versioned JSON contains a bounded transaction identity and proposal digest plus
each peer's stable identity, role, persistent confirmed-commit token, and
confirmation state. It uses mode-0600 temporary files, file and directory
synchronization, atomic replacement, bounded parsing, and owner/type checks.
It refuses to replace an unresolved journal. Completion removes the record and
synchronizes the parent directory.

A failure before atomic replacement proves that COMMIT was not recorded and
permits reverse cancellation. A failure after replacement makes the decision
outcome unknown: the coordinator performs neither confirmation nor rollback
and leaves every peer pending. Startup recovery must inspect the journal before
choosing the next action. This conservative state prevents a storage error from
turning into contradictory decisions across a crash.

When `--peer-journal FILE` is configured, application startup and staged
`SIGHUP` reload inspect that path before serving requests. An absent file means
there is no durable COMMIT to recover. An unsafe, malformed, or unresolved file
fails startup; the diagnostic names the transaction, durable confirmation
count, and pending peer identities without exposing the proposal digest or
persistent commit tokens. The journal path must differ from `--state`.
The recovery layer now has a programmatic confirmation-only NETCONF/TLS
adapter. It opens a fresh mutual-TLS session, verifies the peer certificate and
hostname, requires NETCONF base 1.0 and persistent confirmed-commit 1.1,
validates bounded untrusted XML, correlates the reply `message-id`, and accepts
only an unambiguous `<ok/>`. Persistent tokens are serialized with the XML API
and are never included in diagnostics. Each socket connection and TLS I/O wait
is bounded and messages have byte ceilings; hostname resolution and a total
wall-clock transaction deadline remain integration boundaries. The adapter
invokes no command-line client.

`MakeTlsTransactionParticipant` maps the complete coordinator contract onto a
single reusable authenticated session. Before sending any RPC it requires the
candidate 1.0, validate 1.1, and confirmed-commit 1.1 capabilities. Preparation
locks candidate, uses `<copy-config>` with a complete safe `<config>` image,
and validates candidate. Apply starts a bounded persistent confirmed commit.
Verification retrieves both authoritative running configuration and the
combined configuration/operational view, then passes both authenticated replies
to a provider-supplied configuration and service-health callback. Confirmation uses
the persistent token; pre-decision cancellation also uses that token and
reconnects when an ambiguous transport failure closed the original session.
Cancellation treats an already absent pending commit as the required rolled-
back state. Release attempts candidate unlock and close-session, then closes
the TLS resources idempotently. Persistent tokens are built with the XML API,
including tokens containing XML metacharacters.

The adapter has live coverage for successful two-peer durable coordination on
two independent mutual-TLS servers. That test proves complete candidate
replacement, authenticated running and operational readback, journal decision
and acknowledgements, permanent confirmation, cleanup, and final running state. A
separate live failure-path test proves confirmed-commit cancellation restores
the previous running configuration and that repeated cancellation is harmless.

`--peer-recovery FILE` supplies the stable target mapping as a private,
versioned JSON document. Version 2 binds the exact `(group-id,
participant-id)` pair from the public peer-plan contract to a host, port,
client certificate, private key, trust anchor, and optional per-I/O timeout.
Dangd owns the canonical `group-id/participant-id` coordinator and journal
identity; neither plugin-specific topology nor endpoint knowledge enters the
daemon. Relative credential paths resolve against the configuration file.
Startup and reload validate the complete file even when the journal is absent,
reject duplicate pairs, allow the same participant name in different groups,
and reject an unresolved journal if any paired identity lacks a target. Version
1 participant-only mappings are deliberately unsupported because they become
ambiguous when several peer groups exist. The state, journal, and recovery
configuration paths must all differ.

Normal backend preparation resolves every participant in every composed plan
against this core-owned map before permitting any mutation. A missing target
returns `peer-target-missing`, aborts all plugin preparations, and leaves both
running configuration and hardware unchanged. Entries for other inactive peer
groups are allowed. Plugins never receive endpoint or credential data and
cannot use an undocumented dangd facility to discover it.

When both files are valid, startup and staged reload automatically resume the
durable COMMIT decision. Already acknowledged peers are skipped. Each pending
peer is confirmed through the authenticated adapter and its acknowledgement is
atomically saved before the next lifecycle step. The journal is removed and
its directory synchronized only after every peer is durable. Recovery failure
blocks the replacement application and preserves the reduced pending set for a
later retry; it never converts COMMIT into cancellation.
An exclusive mode-0600 sibling lock is held across journal load, network replay,
acknowledgement writes, and cleanup so competing daemon processes cannot recover
the same decision concurrently.

## Remaining integration

The coordinator is not reachable from NETCONF or `dangctl` yet. Production
preflight already collects, validates, and resolves every composed participant
to a core-owned authenticated endpoint, but pair-wide
management still requires:

- generic production invocation of composed ABI-v9 plans. The external Kea
  provider now supplies complete two-member hot-standby module images plus a
  strict dual-view verifier, while the core validates and composes those
  transport-neutral contributions. The normal NETCONF commit path does not yet
  invoke the resulting plan;
- policy for unreachable or degraded peers, defaulting to rejection;
- a total transaction deadline beyond the implemented per-I/O timeouts,
  observability, NACM rules, packaging, and Linux/FreeBSD interoperability
  tests; and
- CLI support that submits the logical change through dangd rather than
  bypassing NETCONF validation, authorization, ordering, and rollback.

Until those pieces are integrated, each Kea member remains an independently
managed dangd instance and pair-wide atomicity must not be claimed.
