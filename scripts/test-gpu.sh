#!/usr/bin/env bash
# Host verification of the Mali job/MMU encodings used by B2LGPU.cpp.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d "${TMPDIR:-/tmp}/b2l-gpu-wire.XXXXXX")"
trap 'rm -rf "$build_dir"' EXIT
"${CC:-clang}" -std=c11 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I "$repo_root/xnu-overlay/iokit/Kernel/arm" \
  "$repo_root/tests/gpu_wire_test.c" -o "$build_dir/gpu_wire"
"$build_dir/gpu_wire"
