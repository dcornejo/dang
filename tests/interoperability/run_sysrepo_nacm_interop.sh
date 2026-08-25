#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

# Run as root on an isolated Linux validation host with sysrepo development
# packages installed. The script installs only the disposable test module and
# removes its configuration and schema on exit.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
module=dang-nacm-interop
binary=$(mktemp /tmp/dang-nacm-interop.XXXXXX)

cleanup() {
  rm -f "$binary"
  sysrepoctl --uninstall "$module" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

if [ "$(id -u)" -ne 0 ]; then
  echo "run as root so the disposable sysrepo module can be installed" >&2
  exit 2
fi

for command in cc pkg-config sysrepoctl; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "missing required command: $command" >&2
    exit 2
  fi
done

sysrepoctl --uninstall "$module" >/dev/null 2>&1 || true
sysrepoctl --install "$script_dir/$module.yang"

cc -std=c11 -Wall -Wextra -Wpedantic -Werror \
  $(pkg-config --cflags sysrepo libyang) \
  "$script_dir/sysrepo_nacm_interop.c" -o "$binary" \
  $(pkg-config --libs sysrepo libyang)

"$binary"
