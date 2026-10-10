#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTDIR="$(mktemp -d)"
trap 'rm -rf "$TESTDIR"' EXIT
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -I"$ROOT/psz" \
    "$ROOT/tests/analog.cpp" -o "$TESTDIR/analog"
"$TESTDIR/analog"
