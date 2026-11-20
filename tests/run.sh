#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Lucas Holt. All rights reserved.

# mscript test runner: runs each tests/*.vbs and diffs output against tests/<name>.expected
# neg_* tests are expected to fail (nonzero exit); their .expected holds the stderr text.
cd "$(dirname "$0")/.." || exit 1
BIN=./mscript
pass=0
fail=0
tmp=$(mktemp)

# Start the local HTTP test server for the 18_xmlhttp test (needs python3).
SRV_PID=
if command -v python3 >/dev/null 2>&1; then
    python3 tests/httpserver.py 18099 >/dev/null 2>&1 &
    SRV_PID=$!
    i=0
    while [ "$i" -lt 25 ]; do
        if python3 -c "import socket; s=socket.create_connection(('127.0.0.1',18099),0.2); s.close()" 2>/dev/null; then
            break
        fi
        i=$((i + 1))
        sleep 0.2
    done
    if ! python3 -c "import socket; s=socket.create_connection(('127.0.0.1',18099),0.2); s.close()" 2>/dev/null; then
        kill "$SRV_PID" 2>/dev/null
        SRV_PID=
    fi
fi
trap '[ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null' EXIT

for t in tests/*.vbs; do
    name=$(basename "$t" .vbs)
    exp="tests/$name.expected"
    [ -f "$exp" ] || continue
    case "$name" in
    18_*|20_*) [ -n "$SRV_PID" ] || continue ;; # need the local test server
    esac
    "$BIN" "$t" hello 42 >"$tmp" 2>&1
    rc=$?
    case "$name" in
    neg_*)
        if [ "$rc" -ne 0 ] && cmp -s "$tmp" "$exp"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL $name (rc=$rc)"
            diff "$exp" "$tmp" | head -10
        fi
        ;;
    *)
        if [ "$rc" -eq 0 ] && cmp -s "$tmp" "$exp"; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL $name (rc=$rc)"
            diff "$exp" "$tmp" | head -20
        fi
        ;;
    esac
done
rm -f "$tmp"
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
