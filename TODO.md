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

### RFC 8342 / RFC 8526 NMDA compliance closure

- Run an external NMDA interoperability suite. The concurrency release soak
  completed 8,000 validated operational retrievals across eight sessions in
  315.7 seconds under ASan/UBSan while all eight callbacks were simultaneously
  active. Operational callbacks now run behind a supervised worker boundary so
  a hung or crashed plugin is timed out without leaving an in-process thread
  running against unloaded plugin state. The
  bounded deadline-aware framing layer now distinguishes timeout, clean worker
  exit, truncated protocol data, oversized messages, and system failures. The
  installed standalone worker now exclusively loads one plugin and returns a
  ready handshake plus copied manifest/YANG discovery data; parent-side worker
  ownership now enforces separate startup/request deadlines, bounds copied data,
  and contains/reaps operational callback hangs and crashes. The next
  independent request automatically starts a replacement, accepts it only when
  rediscovery exactly matches the loaded manifest and YANG sources, and never
  replays a failed request or hardware side effect. Worker prepare,
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
  it without an in-process fallback.
  Malformed typed data and oversized callback XML are rejected atomically with
  provider attribution and bounded copying. Document unsupported optional
  features and deviations before making an RFC 8342 or RFC 8526 compliance claim.
