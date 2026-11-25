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
        # 39_* exercises CGI headers/cookies: run as GET with a fixed session
        # id, registered as a live session first (unknown ids are rotated to
        # block session fixation) so no session Set-Cookie line is emitted
        case "$name" in
        39_*)
            command -v sqlite3 >/dev/null 2>&1 || { echo "SKIP $name (no sqlite3)"; continue; }
            sqlite3 "$MScript_ASP_STATE_DB" \
                "CREATE TABLE IF NOT EXISTS session_meta(sid TEXT PRIMARY KEY, last REAL, lcid INTEGER, codepage INTEGER);
                 INSERT OR REPLACE INTO session_meta(sid, last) VALUES('0123456789abcdef0123456789abcdef', strftime('%s','now'));" || continue
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
    # a page in a subdirectory shares the app root's global.asa
    name=aspapp_sub_page
    exp=tests/aspapp/sub/page2.expected
    env -u REQUEST_METHOD -u HTTP_COOKIE \
        MScript_ASP_STATE_DB="$tmp.db" QUERY_STRING="" \
        "$BIN" tests/aspapp/sub/page2.asp >"$tmp" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ] && cmp -s "$tmp" "$exp"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $name (rc=$rc)"
        diff "$exp" "$tmp" | head -20
    fi
fi
# ASP security regressions (tests/aspsec): each check runs a page as CGI and
# greps the raw response
if [ -d tests/aspsec ]; then
    sdb="$tmp.sec.db"
    asp_check() { # name, expect(match|nomatch), pattern, page, [env...]
        cname=$1 mode=$2 pat=$3 page=$4
        shift 4
        env -u HTTP_COOKIE -u QUERY_STRING REQUEST_METHOD=GET MScript_ASP_STATE_DB="$sdb" "$@" \
            "$BIN" "$page" >"$tmp" 2>&1 </dev/null
        crc=$?
        if LC_ALL=C grep -aq -- "$pat" "$tmp"; then found=match; else found=nomatch; fi
        if [ "$crc" -eq 0 ] && [ "$found" = "$mode" ]; then
            pass=$((pass + 1))
        else
            fail=$((fail + 1))
            echo "FAIL aspsec_$cname (rc=$crc)"
            head -10 "$tmp"
        fi
    }
    # overlong / truncated UTF-8 must not decode to markup after HTMLEncode
    asp_check overlong nomatch '<script>' tests/aspsec/encode.asp \
        'QUERY_STRING=a=%C0%BCscript%C0%BE&b='
    asp_check quote_swallow match 'title="x?" href=" onmouseover' tests/aspsec/encode.asp \
        'QUERY_STRING=a=x%C3&b=%20onmouseover%3Dalert(1)'
    asp_check utf8_valid match 'caf.!' tests/aspsec/encode.asp 'QUERY_STRING=a=caf%C3%A9!&b='
    # a client-chosen (never issued) session id is replaced, with a hardened cookie
    asp_check fixation nomatch 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa' tests/aspsec/sid.asp \
        'HTTP_COOKIE=MScriptSessionID=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    asp_check cookie_flags match 'MScriptSessionID=[0-9a-f]*; path=/; httponly; samesite=Lax' tests/aspsec/sid.asp
    asp_check cookie_secure match '; secure; httponly' tests/aspsec/sid.asp HTTPS=on
    # huge Response.Expires must not crash (gmtime NULL)
    asp_check expires_huge match 'Expires: .* 9999 ' tests/aspsec/expires.asp
    # Response.Redirect inside Session_OnStart ends the request cleanly
    asp_check onstart_redirect match 'Location: /login' tests/aspsec/app/page.asp
    # BinaryWrite bytes are never transcoded (0xFF 0xD8 0xE9 stay as-is)
    asp_check binarywrite match "$(printf '\377\330\351')" tests/aspsec/binwrite.asp
    # Response.Flush transcodes like the final flush (é -> one 0xE9 byte, both sides)
    asp_check flush_transcode match "$(printf 'caf\351|caf\351.')" tests/aspsec/flush.asp
    # Redirect sends a real 302, drops the buffered page and escapes the URL
    asp_check redirect_status match '^Status: 302 Object moved' tests/aspsec/redirect.asp
    asp_check redirect_nobody nomatch 'SECRET DATA' tests/aspsec/redirect.asp
    asp_check redirect_escaped match 'HREF="/login?a=1&amp;b=&quot;x"' tests/aspsec/redirect.asp
    # header changes after Flush raise ASP 0156 instead of being dropped
    asp_check after_flush match 'add=80004005|cookie=80004005|status=80004005|redirect=80004005' \
        tests/aspsec/afterflush.asp
    # LF-only multipart: no crash, value intact
    printf -- '--B\nContent-Disposition: form-data; name="a"\n\n--B\nContent-Disposition: form-data; name="b"\n\nhello\n--B--' >"$tmp.body"
    env -u HTTP_COOKIE REQUEST_METHOD=POST MScript_ASP_STATE_DB="$sdb" \
        CONTENT_TYPE='multipart/form-data; boundary=B' CONTENT_LENGTH=$(($(wc -c <"$tmp.body"))) \
        "$BIN" tests/aspsec/form.asp <"$tmp.body" >"$tmp" 2>&1
    if [ $? -eq 0 ] && grep -aq '\[\]\[hello\]' "$tmp"; then pass=$((pass + 1)); else
        fail=$((fail + 1)); echo "FAIL aspsec_multipart_lf"; head -10 "$tmp"; fi
    # concurrent writers wait on the lock instead of failing with SQLITE_BUSY
    # (wait on these pids only: a bare `wait` would block on the test server)
    i=0; pids=; : >"$tmp.err"
    while [ $i -lt 16 ]; do
        env -u HTTP_COOKIE REQUEST_METHOD=GET MScript_ASP_STATE_DB="$sdb" \
            "$BIN" tests/aspsec/write.asp >/dev/null 2>>"$tmp.err" &
        pids="$pids $!"
        i=$((i + 1))
    done
    for p in $pids; do wait "$p"; done
    if [ ! -s "$tmp.err" ]; then pass=$((pass + 1)); else
        fail=$((fail + 1)); echo "FAIL aspsec_concurrency"; sort "$tmp.err" | uniq -c | head -5; fi
    rm -f "$sdb" "$sdb-wal" "$sdb-shm" "$sdb.lock" "$tmp.body" "$tmp.err"
fi

rm -f "$tmp" "$tmp.db"
echo "passed: $pass  failed: $fail"
[ "$fail" -eq 0 ]
