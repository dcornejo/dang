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
full participant transport, configured peer identities, automatic startup
recovery, policy, and operator-facing configuration remain required before the
feature can be enabled.

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

`--peer-recovery FILE` supplies the stable target mapping as a private,
versioned JSON document. Each entry binds an exact journal participant ID to a
host, port, client certificate, private key, trust anchor, and optional per-I/O
timeout. Relative credential paths resolve against the configuration file.
Startup and reload validate the complete file even when the journal is absent,
and reject an unresolved journal if any participant lacks a target. The state,
journal, and recovery configuration paths must all differ.

The validated mapping is deliberately not used to mutate a peer yet. An
unresolved journal still fails startup and reload closed until lifecycle replay
is connected, rather than treating configuration alone as proof that automatic
recovery is safe.

## Remaining integration

The coordinator is not reachable from NETCONF or `dangctl` yet. Production
pair-wide management still requires:

- lifecycle integration that uses the validated participant endpoint mapping
  to resume a durable commit decision before accepting a conflicting
  transaction;
- transport adapters for prepare, apply, verify, cancel, and release (only
  recovery confirmation is currently implemented);
- provider-specific translation into complete per-peer candidates and a
  post-apply health check;
- policy for unreachable or degraded peers, defaulting to rejection;
- a total transaction deadline beyond the implemented per-I/O timeouts,
  observability, NACM rules, packaging, and Linux/FreeBSD interoperability
  tests; and
- CLI support that submits the logical change through dangd rather than
  bypassing NETCONF validation, authorization, ordering, and rollback.

Until those pieces are integrated, each Kea member remains an independently
managed dangd instance and pair-wide atomicity must not be claimed.
