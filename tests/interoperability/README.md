<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# NACM interoperability evidence

`run_sysrepo_nacm_interop.sh` checks the project's RFC 8341 interpretation
against sysrepo's independently implemented NACM engine. It uses the public
sysrepo API at the real datastore authorization boundary and verifies:

- ordered group/rule matching;
- default-deny read, write, and execute behavior;
- silent filtering of a denied leaf while preserving its readable sibling;
- permitted and rejected updates; and
- permitted and rejected RPC execution.

Run this on a disposable Linux host because it temporarily installs the
`dang-nacm-interop` module in the host sysrepo repository:

```console
sudo apt-get install sysrepo sysrepo-modules libsysrepo-dev libyang-dev pkg-config
sudo tests/interoperability/run_sysrepo_nacm_interop.sh
```

The script removes the test module and data on exit. The same policy cases are
covered in dang by `NacmTest.AppliesOrderedGroupRulesAndDefaults`,
`NacmTest.AppliesWildcardGroupsModulesNamesAndCrudxBitsInOrder`, and the
protocol-level tests referenced by `docs/NACM_COMPLIANCE_MATRIX.md`.

This focused decision comparison does not claim compatibility with every
sysrepo or Netopeer2 release, nor does it replace transport interoperability
testing. The exact independent versions and results used for compliance
evidence are recorded in the NACM compliance matrix.

## External RFC 5277 notification filtering

`run_ncclient_notification_interop.sh` starts the production mutual-TLS server
around a deterministic event trigger and connects with the independent Python
ncclient implementation. One YANG Library publication produces a current and a
legacy event. NACM permits `yang-library-update`, suppresses
`yang-library-change`, and increments `denied-notifications` exactly once.

On an isolated Ubuntu validation host:

```console
sudo apt-get install python3-ncclient
cmake -S . -B build -DYANG_BUILD_TESTS=ON -DYANG_BUILD_DANGD=ON
cmake --build build --target nacm_notification_interop_server
tests/interoperability/run_ncclient_notification_interop.sh build
```

The successful transcript is:

```text
ncclient subscription: PASS
received yang-library-update: PASS
suppressed yang-library-change: PASS
dangd delivery: yang-library-update PASS
dangd suppression: yang-library-change PASS
denied-notifications counter: 1 PASS
```
