#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
${CC:-cc} -O2 -g -Isrc -pthread -Wl,-z,nodelete bench/round9/epoch.c -o "$TMP/epoch"
timeout 30 "$TMP/epoch"
