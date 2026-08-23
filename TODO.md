<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Project TODO

This file contains only unfinished work. Add new tasks in dependency order and
remove each task after its implementation, tests, documentation, clean build,
and install/export checks pass. The commit that completes an item must remove
or narrow it here, update the corresponding status and variance in
[`docs/COMPLIANCE.md`](docs/COMPLIANCE.md), update focused documentation and
changelogs, and cite the verification performed. A partially completed item
stays in this file with its remaining work rewritten precisely.

## Active tasks

### RFC 8343/8344 native IP management closure

- Replace the initial Linux `/sbin/ip` and FreeBSD `/sbin/ifconfig` adapters
  with direct kernel APIs, publish live link/address/neighbor state, reconcile
  external drift, cover MTU and neighbor configuration, and test privileged
  apply plus partial-failure compensation on both operating systems.

### RFC 8341 NACM compliance closure

- Run interoperability and negative-security tests against at least one
  independent RFC 8341 implementation, run a sustained coverage-guided NACM
  fuzz campaign, and document any intentional deviations before claiming
  compliance. Include a broader independent OpenSSH RFC 6242 matrix and
  sustained concurrent SSH-session/backpressure coverage; a single OpenSSH
  public-key/subsystem/RPC/close smoke interaction has passed. The deterministic
  sanitizer smoke target mutates NACM XML and
  attacker-controlled keyed/leaf-list instance paths through policy loading,
  CRUD authorization, and read filtering. Schema-aware read filtering rejects
  unmodeled elements instead of applying annotation-free default access.

### RFC 8342 / RFC 8526 NMDA compliance closure

- Run an external NMDA interoperability suite and a long-duration concurrency
  soak under sanitizers. The deterministic end-to-end stress test completes
  200 retrievals across eight sessions while all eight provider callbacks are
  simultaneously active.
  Isolate operational callbacks in a supervised worker boundary so a hung or
  crashed plugin can be timed out without leaving an in-process thread running
  against unloaded plugin state; add timeout/crash recovery tests. Malformed
  typed data and oversized callback XML are rejected atomically with provider
  attribution and bounded copying. Document unsupported optional features and
  deviations before making an RFC 8342 or RFC 8526 compliance claim.
