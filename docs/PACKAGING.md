<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Native packages

The CMake install manifest is the single source of truth for Debian and
FreeBSD packages. It includes the YANG library and headers, command-line
tools, dangd, the supervised plugin worker, built-in plugins, models, examples,
and documentation. Packaging does not install an enabled daemon service:
dangd requires deployment-specific model, datastore, NACM, and transport-key
choices that cannot be selected safely by a package script. The compiled-in
`dangd-superuser` is a dangd-only recovery name, not an operating-system user;
packages intentionally create no account, password, key, or certificate for it.

## Debian

Install the build dependencies, configure a release build, run its tests, and
ask CPack for a native package:

```sh
cmake -S . -B build-package -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-package
ctest --test-dir build-package --output-on-failure
cpack --config build-package/CPackConfig.cmake -G DEB
dpkg-deb --info dangd_0.1.0_amd64.deb
```

The DEB generator derives shared-library dependencies with `dpkg-shlibdeps`.
The resulting package can be installed with `apt install ./dangd_*.deb`, which
also resolves those dependencies.

## FreeBSD

Use the same process with the FreeBSD generator:

```sh
cmake -S . -B build-package -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-package
ctest --test-dir build-package --output-on-failure
cpack --config build-package/CPackConfig.cmake -G FREEBSD
pkg info -F dangd-0.1.0.pkg
```

Install with `pkg add dangd-0.1.0.pkg`. The package records the native origins
for fmt, pugixml, libxml2, libssh, and nlohmann-json. Build packages on the
target operating system; the project does not claim cross-packaging support.
