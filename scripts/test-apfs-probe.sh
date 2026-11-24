#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cc="${CC:-cc}"
"$cc" -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I"$root/common/apfs" \
  "$root/common/apfs/b2l_apfs_probe.c" \
  "$root/tests/apfs/test_apfs_probe.c" \
  -o "$tmp/test_apfs_probe"
"$tmp/test_apfs_probe"
