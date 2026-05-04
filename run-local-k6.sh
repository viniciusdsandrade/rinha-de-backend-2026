#!/usr/bin/env bash

set -euo pipefail

tmpdir="$(mktemp -d)"
log_file="$(mktemp)"
trap 'rm -rf "$tmpdir" "$log_file" test/results.json' EXIT

mkdir -p "$tmpdir/test"
grep -v "k6-summary" test/test.js > "$tmpdir/test.js"
cp test/test-data.json "$tmpdir/test-data.json"
rm -f test/results.json

if ! (cd "$tmpdir" && k6 run test.js > "$log_file" 2>&1); then
    cat "$log_file" >&2
    exit 1
fi

cp "$tmpdir/test/results.json" test/results.json
jq . test/results.json
