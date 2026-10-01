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
this boundary. It does not advertise pair-wide commit support yet. Network
transport, authentication, durable journal storage, startup recovery, policy,
and operator-facing configuration remain required before the feature can be
enabled.

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

## Remaining integration

The coordinator is not reachable from NETCONF or `dangctl` yet. Production
pair-wide management still requires:

- authenticated NETCONF client sessions with pinned peer identities;
- wiring the implemented private journal into application lifecycle and
  operator diagnostics;
- restart recovery that resumes a durable commit decision before accepting a
  conflicting transaction;
- provider-specific translation into complete per-peer candidates and a
  post-apply health check;
- policy for unreachable or degraded peers, defaulting to rejection;
- bounded timeouts, observability, NACM rules, packaging, and Linux/FreeBSD
  interoperability tests; and
- CLI support that submits the logical change through dangd rather than
  bypassing NETCONF validation, authorization, ordering, and rollback.

Until those pieces are integrated, each Kea member remains an independently
managed dangd instance and pair-wide atomicity must not be claimed.
