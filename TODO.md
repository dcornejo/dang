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

- Publish duplicate-address status on Linux and FreeBSD. FreeBSD
  now applies enabled state, equal per-family MTUs, and IPv4/IPv6 addresses
  through native `SIOC*` ioctls and static neighbors through acknowledged
  route-netlink requests, with retained live link snapshots and reverse
  partial-failure compensation. It publishes live administrative/operational status, MTU,
  IPv4/IPv6 addresses, and the complete neighbor table for configured
  and system-controlled interfaces, including native packet/octet/error/drop
  counters with an interface-epoch discontinuity timestamp. It restores missing
  or altered configured address and neighbor entries without deleting unrelated
  kernel-owned state. Automated privileged
  tests target a disposable interface; a FreeBSD 16 epair smoke passed
  address/MTU/IPv4 and IPv6 neighbor apply, live-state publication, out-of-band
  removal repair, exact rollback, and forced partial-failure compensation.
  Linux applies enabled state, IPv4/IPv6 addresses, equal per-family MTUs, and
  static neighbors with acknowledged
  rtnetlink requests. It observes live link flags/MTU, compensates a partially
  failed request sequence, and retains the exact observed link snapshot until
  the enclosing transaction commits or rolls back. It repairs missing or
  altered configured addresses and neighbors without deleting unrelated kernel
  state. Privileged isolated Linux tests cover address/neighbor apply, removal,
  out-of-band removal repair, partial failure, exact MTU rollback, and live
  link/MTU/address/neighbor publication. Linux now inventories configured and
  system-controlled interfaces and publishes native packet/octet/error/drop
  counters with a kernel-boot discontinuity timestamp.

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

- Run an external NMDA interoperability suite. The concurrency release soak
  completed 8,000 validated operational retrievals across eight sessions in
  315.7 seconds under ASan/UBSan while all eight callbacks were simultaneously
  active. Isolate operational callbacks in a supervised worker boundary so a
  hung or crashed plugin can be timed out without leaving an in-process thread
  running against unloaded plugin state; add timeout/crash recovery tests. The
  bounded deadline-aware framing layer now distinguishes timeout, clean worker
  exit, truncated protocol data, oversized messages, and system failures. The
  installed standalone worker now exclusively loads one plugin and returns a
  ready handshake plus copied manifest/YANG discovery data; parent-side worker
  ownership now enforces separate startup/request deadlines, bounds copied data,
  and contains/reaps operational callback hangs and crashes. Live application
  cutover, parent operation-provider routing, and restart policy remain. Worker
  prepare,
  validate, abort, action discovery, named apply, named rollback, and ABI-v6
  reconciliation and RPC/action invocation already retain and use opaque
  transaction state entirely inside the worker process. The parent worker
  coordinator combines copied actions into a global dependency plan and
  compensates action failure in reverse. It now retains a successfully applied
  plan until all reconciliation reports pass schema, module-ownership, and
  outcome-uniqueness checks, then compensates if they fail.
  Application, backend, operational publication, and operation dispatch use the
  common `PluginRuntime` contract. `PluginWorkerRuntime` now implements that
  contract with one supervised process per plugin. Daemon startup now selects
  it without an in-process fallback; automatic restart policy remains.
  Malformed typed data and oversized callback XML are rejected atomically with
  provider attribution and bounded copying. Document unsupported optional
  features and deviations before making an RFC 8342 or RFC 8526 compliance claim.
