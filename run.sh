#!/usr/bin/env bash

set -euo pipefail

log_file="$(mktemp)"
trap 'rm -f "$log_file" test/results.json' EXIT

rm -f test/results.json

if ! k6 run test/test.js > "$log_file" 2>&1; then
    cat "$log_file" >&2
    exit 1
fi

jq . test/results.json
