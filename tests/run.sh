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

for t in tests/*.vbs tests/*.asp; do
    name=$(basename "$t"); name="${name%.*}"
    exp="tests/$name.expected"
    [ -f "$exp" ] || continue
    case "$name" in
    18_*|20_*) [ -n "$SRV_PID" ] || continue ;; # need the local test server
    esac
    # .asp tests run as classic-ASP templates with a scratch state db and a
    # fixed query string; REQUEST_METHOD is left unset so output is body-only
    if [ "${t##*.}" = "asp" ]; then
        MScript_ASP_STATE_DB="$tmp.db"
        export MScript_ASP_STATE_DB
        QUERY_STRING="name=mscript&n=42"
        export QUERY_STRING
        # 39_* exercises CGI headers/cookies: run as GET with a fixed valid
        # session id so the Set-Cookie line is deterministic
        case "$name" in
        39_*)
            REQUEST_METHOD=GET
            export REQUEST_METHOD
            HTTP_COOKIE="MScriptSessionID=0123456789abcdef0123456789abcdef; theme=dark"
            export HTTP_COOKIE
            ;;
        esac
    fi
    # tests/<name>.args, if present, replaces the default arguments
    # (whitespace-separated, no quoting or globbing)
    args="hello 42"
    [ -f "tests/$name.args" ] && args=$(cat "tests/$name.args")
    set -f
    # shellcheck disable=SC2086
    "$BIN" "$t" $args >"$tmp" 2>&1
    rc=$?
    set +f
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

# global.asa hook test (own subdirectory so other .asp tests are unaffected);
# body-only output (no REQUEST_METHOD) keeps the fresh session id out of it
if [ -f tests/aspapp/page.asp ]; then
    name=aspapp_page
    exp=tests/aspapp/page.expected
    env -u REQUEST_METHOD -u HTTP_COOKIE \
        MScript_ASP_STATE_DB="$tmp.db" QUERY_STRING="" \
        "$BIN" tests/aspapp/page.asp >"$tmp" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ] && cmp -s "$tmp" "$exp"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name (rc=$rc)"
        diff "$exp" "$tmp" | head -20
    fi
fi
rm -f "$tmp" "$tmp.db"
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
