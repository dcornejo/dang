#!/bin/sh
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 /path/to/yanglint" >&2
  exit 2
fi

yanglint=$1
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
model_dir=$script_dir/../../dangd/models

for module in ietf-ssh-common ietf-ssh-client ietf-ssh-server; do
  "$yanglint" -p "$model_dir" \
    "$model_dir/$module@2024-10-10.yang"
done
