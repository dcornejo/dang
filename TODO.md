<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Project TODO

This file contains only unfinished work. Add new tasks in dependency order and
remove each task after its implementation, tests, documentation, clean build,
and install/export checks pass.

## Active tasks

- Design and implement a transactional hardware application planner for
  `dangd`: preflight dynamic platform limits, derive generic and backend-specific
  action dependencies, enforce activation-last/deactivation-first ordering,
  apply the resulting graph with rollback, and publish the running datastore
  only after the hardware transaction succeeds. Include failure-injection tests
  for unsafe ordering, exhausted resources, partial application, successful
  rollback, and rollback failure with explicit state-divergence reporting.
