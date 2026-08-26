#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd)
build_dir=${1:-"$source_dir/build"}
server="$build_dir/nmda_interop_server"
port=${DANG_NMDA_INTEROP_PORT:-16514}
run_dir=$(mktemp -d /tmp/dang-nmda-interop.XXXXXX)
server_pid=

cleanup() {
  if [ -n "$server_pid" ]; then
    kill "$server_pid" >/dev/null 2>&1 || true
    wait "$server_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$run_dir"
}
trap cleanup EXIT HUP INT TERM

if [ ! -x "$server" ]; then
  echo "missing $server; configure with YANG_BUILD_TESTS and YANG_BUILD_DANGD" >&2
  exit 2
fi
if ! /usr/bin/python3 -c 'import ncclient' >/dev/null 2>&1; then
  echo "python3-ncclient is required" >&2
  exit 2
fi

"$server" "$source_dir" "$port" "$run_dir" >"$run_dir/server.log" 2>&1 &
server_pid=$!

attempt=0
while [ ! -f "$run_dir/server-ready" ] && [ "$attempt" -lt 100 ]; do
  sleep 0.05
  attempt=$((attempt + 1))
done
if [ ! -f "$run_dir/server-ready" ]; then
  echo "server did not become ready" >&2
  sed -n '1,160p' "$run_dir/server.log" >&2
  exit 1
fi

if ! /usr/bin/python3 "$script_dir/ncclient_nmda_interop.py" \
    127.0.0.1 "$port" "$source_dir/dangd/testdata/tls"; then
  sed -n '1,160p' "$run_dir/server.log" >&2
  exit 1
fi
if ! wait "$server_pid"; then
  server_pid=
  sed -n '1,160p' "$run_dir/server.log" >&2
  exit 1
fi
server_pid=
sed -n '1,160p' "$run_dir/server.log"
