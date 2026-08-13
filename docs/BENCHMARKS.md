<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Performance benchmarks

The benchmark executable uses fixed generated inputs and only project
dependencies. It avoids an additional framework dependency and emits versioned
JSON suitable for archiving or comparing in CI:

```sh
cmake --preset benchmarks
cmake --build --preset benchmarks
./build-benchmarks/yang_benchmarks
```

Each result is milliseconds per operation from five independently timed
batches after an untimed warm-up. The report includes minimum, median, maximum,
iteration counts, workload size, and an observation checksum that prevents the
measured work from being optimized away. Timings are evidence, not test
assertions: shared-machine scheduling, thermal state, compiler changes, and
dependency versions make hard thresholds unreliable. Release automation should
compare medians from equivalent machines and require investigation rather than
automatically declaring a functional failure.

## Initial baseline

Recorded 2026-08-13 on a 48 GB Apple M4 Pro MacBook Pro running macOS 26.6.1,
Apple Clang 21.0.0, CMake 4.4.2, and a Release build. The workload contains a
500-leaf generated schema and a configuration containing all 500 leaves.

- Schema compilation: 1.100 ms median (1.026–1.584 ms).
- Configuration parse plus complete validation: 4.833 ms median
  (4.777–5.014 ms).
- RFC 6242 framed-message decode: 0.0000668 ms median
  (0.0000655–0.0000674 ms).
- NETCONF `get-config`: 0.0347 ms median (0.0346–0.0351 ms).

These figures establish scale and a repeatable comparison point; they are not
performance guarantees. Keep workload constants and the output format stable
within a release series. If either changes, increment the report format and
record a new baseline rather than comparing unlike results.
