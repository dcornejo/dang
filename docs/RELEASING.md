<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Release and compatibility policy

The project follows Semantic Versioning. Before 1.0, a minor version may make
documented source or ABI changes; patch versions remain backward compatible.
Starting with 1.0, removing or incompatibly changing public declarations under
`include/yang` requires a major release. Additive APIs and behavior that does
not invalidate documented contracts may ship in a minor release. Fixes that
preserve the API ship in a patch release. The static library does not promise
ABI stability before 1.0.

## Preparing a release

1. Move the relevant entries from `Unreleased` in `CHANGELOG.md` into a dated
   version section and update its comparison links.
2. Set the identical version in `project(... VERSION ...)` in `CMakeLists.txt`.
3. Run the normal, sanitizer, documentation, installation, and package-consumer
   checks, plus both native package builds described in `PACKAGING.md`. Create
   and push an annotated tag named `v<version>`.
4. The release workflow verifies that the tag and CMake version match. It then
   creates the source archive twice and rejects non-reproducible output.
5. The workflow publishes the archive, SHA-256 checksum file, HTML and PDF
   documentation, and a GitHub/Sigstore signed build-provenance attestation.

Verify a downloaded archive with:

```sh
shasum -a 256 -c SHA256SUMS
gh attestation verify yang-cpp-0.1.0.tar.gz --repo OWNER/REPOSITORY
```

The attestation is the release signature: it binds the archive digest to the
GitHub Actions workflow identity using a short-lived Sigstore certificate. No
long-lived signing key is stored in the repository. Release assets must never
be replaced after publication; issue a new patch release instead.

## Changelog assistance

`scripts/generate_changelog.py` groups Conventional Commit subjects since a
specified tag. Its Markdown output is a draft for human review, not an
automatic substitute for describing compatibility and migration effects.

```sh
python3 scripts/generate_changelog.py --since v0.1.0
```

## Build API documentation

Generate the Doxygen HTML and PDF references with:

```sh
brew install doxygen
brew install mermaid-cli
brew install --cask mactex-no-gui
# Open a new terminal after installing MacTeX so pdflatex is on PATH.
cmake -S . -B build-docs -DYANG_BUILD_DOCS=ON
cmake --build build-docs
```

The build creates `build-docs/docs/html/index.html` and
`build-docs/docs/latex/refman.pdf`. Installing this configuration places the
HTML tree under `share/doc/yang/html` and the PDF at
`share/doc/yang/yang-cpp-reference.pdf`.
