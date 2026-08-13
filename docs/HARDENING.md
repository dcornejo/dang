<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Production hardening

## Sanitizers

The checked-in preset builds the library, tools, examples, and complete test
suite with AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
cmake --preset sanitizers
cmake --build --preset sanitizers
ctest --preset sanitizers
```

On macOS the Homebrew GoogleTest package is not sanitizer-instrumented. The
test preset disables libc++'s container-overflow annotation check to avoid a
false positive across that binary boundary. Heap, stack, lifetime, bounds, and
undefined-behavior instrumentation remain enabled. A sanitizer run exposed a
real use-after-free in partial-tree composition; cloning can grow the node
arena, so the implementation now reacquires nodes by stable ID after growth.

## Fuzzing

`yang_frontend_fuzz` covers UTF-8 decoding, lexing, statement parsing, YIN XML
loading, and YIN-tree JSON loading. `yang_protocol_fuzz` covers RFC 6242
framing, RFC 6241 subtree and XPath filters, RFC 8341 NACM loading, and
versioned datastore snapshot restoration. Persistence exposes an in-memory
restore entry point so fuzzing does not depend on temporary-file behavior.
Inputs larger than 1 MiB are rejected by each harness so every iteration has a
deterministic upper bound.

Apple's Xcode Clang does not include the libFuzzer runtime. The `fuzzers` preset
therefore builds the identical entry point with a deterministic mutation driver
and ASan/UBSan:

```sh
cmake --preset fuzzers
cmake --build --preset fuzzers
ASAN_OPTIONS=detect_container_overflow=0 \
  ./build-fuzzers/yang_frontend_fuzz tests/fuzz/corpus/frontend
ASAN_OPTIONS=detect_container_overflow=0 \
  ./build-fuzzers/yang_protocol_fuzz tests/fuzz/corpus/protocol
```

For continuous coverage-guided fuzzing, configure with Homebrew LLVM and leave
`YANG_FUZZ_STANDALONE=OFF`; CMake then links the target with
`-fsanitize=fuzzer`. Seed files put a one-byte dispatch selector before the
actual payload. Never treat a bounded smoke run as a substitute for sustained
fuzzing.

Performance workloads, execution instructions, comparison policy, and the
initial Apple Silicon baseline are maintained in `docs/BENCHMARKS.md`.

## Persistence and concurrency

Snapshot saving exposes optional checkpoints after the temporary write,
temporary-file synchronization, atomic replacement, and parent-directory
synchronization. Tests interrupt every stage and verify that the canonical
path always contains a complete, schema-valid old or new snapshot. A failure
before replacement preserves the old snapshot; a failure after replacement
leaves the complete new snapshot available for recovery. Hosts normally omit
the checkpoint callback.

The concurrency stress test shares immutable schema/configuration inputs and
the synchronized datastore, NACM counters, notification queues, and session
registry across eight worker threads. It performs 2,000 complete cycles and
checks final datastore validity, exact denial counters, notification filtering,
and registry consistency. The complete 224-test suite passes under ASan and
UBSan as well as the normal build.

## Deterministic resource ceilings

`ResourceLimits` centralizes the default untrusted-input policy. Current
defaults are 8 MiB per YANG source, one million statements, 256 YANG nesting
levels, 16 MiB per XML document, one million XML elements, 256 XML nesting
levels, 64 KiB and 1,024 location steps per XPath expression, and 64 MiB per
persistence snapshot. Filesystem loaders inspect file size before allocating
the input buffer. Source creation and the parser also accept an explicit policy
for applications that require tighter bounds.

Configuration/edit XML, RPCs, URL-returned configuration, filters, NACM,
notifications, YIN documents, and persistence restoration fail closed when a
ceiling is exceeded. NETCONF reports `too-big` where an RPC error is available.
The framing decoder bounds message size, chunk-header length, and bytes handled
by one feed call. Transport output queues and notification event/byte queues
retain their separately configurable ceilings. XML tree inspection is
iterative, preventing the limit checker itself from consuming recursive stack.
