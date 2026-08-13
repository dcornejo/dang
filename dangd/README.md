<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# dangd

`dangd` is the host-application layer for a future production NETCONF
configuration server. It is kept separate from the reusable `yang` library.

The current foundation:

- loads one root YANG module and its import/include dependency closure;
- binds and completely validates an initial XML configuration;
- constructs running, candidate, and startup NETCONF datastores;
- optionally restores and saves an atomic datastore snapshot;
- provides `--check` startup validation; and
- provides an RFC 6242 stdin/stdout session for supervised integration tests.

Build and validate a configuration:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/dangd --model models/appliance.yang --config config.xml --check
```

The `--stdio` mode exercises a complete framed NETCONF session, but it does not
authenticate or encrypt the peer. The supplied `--username` is trusted. Do not
connect this mode directly to a socket; use it only in tests or behind a local
supervisor that has already authenticated the peer.

Production SSH/TLS listeners, authentication, NACM policy loading, operational
state, and a transactional device backend remain future `dangd` work.
