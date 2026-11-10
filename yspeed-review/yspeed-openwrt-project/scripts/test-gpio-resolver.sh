#!/usr/bin/env bash
set -Eeuo pipefail
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TEST_SOURCE="$PROJECT_DIR/tests/gpio-resolver-test.c"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
CC="${CC:-cc}"
"$CC" -std=gnu11 -O2 -Wall -Wextra -o "$TMP_DIR/gpio-resolver-test" "$TEST_SOURCE"
"$TMP_DIR/gpio-resolver-test"
