#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>
#
# Configure a separate analyzer build with clang-tidy and sanitizers, build it,
# and run the tests. The normal build tree is left untouched.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${1:-"$root/build-analyze"}

cmake -S "$root" -B "$build_dir" -G Ninja \
    -DKARTON_BUILD_TESTS=ON \
    -DKARTON_CLANG_TIDY=ON \
    -DKARTON_SANITIZE=ON
cmake --build "$build_dir" -j"$(nproc)"
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir "$build_dir" --output-on-failure
