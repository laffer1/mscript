#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Lucas Holt. All rights reserved.

# tests/wine/run.sh — run Wine's VBScript conformance .vbs files under mscript.
#
# The wine test sources are LGPL and are NOT vendored into this repo; point
# WINE_VBS_DIR at the wine port's extracted test directory, e.g.:
#   WINE_VBS_DIR=/usr/mports/emulators/wine/work/wine-11.0/dlls/vbscript/tests
# Runs lang.vbs, error.vbs, regexp.vbs (api.vbs needs the C harness's `test`
# object and is skipped by default; pass --all to include it).
#
# Usage: sh tests/wine/run.sh [--all]
# Prints per-file ok/not-ok counts and the failing assertions. Exit status:
# 2 if the wine test dir is missing, 1 if mscript crashes on a file, else 0
# (a report, not a gate).

cd "$(dirname "$0")/../.." || exit 1
BIN=./mscript
DIR=${WINE_VBS_DIR:-/usr/mports/emulators/wine/work/wine-11.0/dlls/vbscript/tests}
ALL=0
[ "$1" = "--all" ] && ALL=1

[ -d "$DIR" ] || { echo "wine test dir not found: $DIR (set WINE_VBS_DIR)"; exit 2; }

FILES="lang.vbs error.vbs regexp.vbs"
[ "$ALL" = "1" ] && FILES="$FILES api.vbs"

rc=0
for f in $FILES; do
    src="$DIR/$f"
    [ -f "$src" ] || { echo "skip $f (not found)"; continue; }
    tmp=$(mktemp /tmp/mscript-wine-XXXXXX)
    # mscript registers Dim at execution time, so the shim (which declares the
    # harness globals) must run before the wine file's top-level statements.
    # Hoist "Option Explicit" to line 1 and strip it from the wine file so the
    # wine file's own Option Explicit stays in effect script-wide.
    {
        if grep -qiE '^[[:space:]]*option[[:space:]]+explicit[[:space:]]*$' "$src"; then
            echo 'Option Explicit'
        fi
        cat tests/wine/shim.vbs
        grep -viE '^[[:space:]]*option[[:space:]]+explicit[[:space:]]*$' "$src"
        # TAP summary footer: runs after the wine file's top-level statements
        echo 'WScript.Echo "1.." & g_no'
        echo 'If g_fail > 0 Then WScript.Echo "# failed: " & g_fail & " of " & g_no'
    } > "$tmp"
    out=$("$BIN" "$tmp" 2>&1)
    st=$?
    rm -f "$tmp"
    total=$(printf '%s\n' "$out" | sed -n 's/^1\.\.//p' | tail -1)
    fails=$(printf '%s\n' "$out" | grep '^not ok' | grep -vc '# TODO')
    oks=$(printf '%s\n' "$out" | grep -c '^ok ')
    printf '%-12s ok=%-4s not-ok=%-4s (of %s)%s\n' "$f" "$oks" "$fails" "${total:-?}" \
        "$([ "$st" -ne 0 ] && printf ' CRASH rc=%s' "$st")"
    if [ "$st" -ne 0 ]; then
        printf '%s\n' "$out" | grep -v '^ok ' | head -3 | sed 's/^/    /'
        rc=1
    fi
    if [ "$fails" -gt 0 ]; then
        printf '%s\n' "$out" | grep '^not ok' | grep -v '# TODO' | head -40 | sed 's/^/    /'
    fi
done
exit $rc
