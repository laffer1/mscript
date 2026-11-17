#!/bin/sh
# mscript test runner: runs each tests/*.vbs and diffs output against tests/<name>.expected
# neg_* tests are expected to fail (nonzero exit); their .expected holds the stderr text.
cd "$(dirname "$0")/.." || exit 1
BIN=./mscript
pass=0
fail=0
tmp=$(mktemp)
for t in tests/*.vbs; do
    name=$(basename "$t" .vbs)
    exp="tests/$name.expected"
    [ -f "$exp" ] || continue
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
