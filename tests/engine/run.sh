#!/bin/sh
# Engine regression tests. Needs only clang++ (or any C++11 compiler as $CXX).
#
#   tests/engine/run.sh                     run cases.tsv against the working tree engine
#   tests/engine/run.sh compare OLD NEW     run corpus.txt on two engine source dirs and
#                                           print every row whose text, backspace count
#                                           or macro key differs
#
# cases.tsv columns (tab separated):
#   id  options  keys  expected_text  expected_bs
#   options: space separated -oNAME=VALUE, or "-" for defaults (Telex, spell check on)
#   keys: as typed; ' ' = space, '<' = backspace
#   expected_bs: number of synthetic backspaces, or "-" to skip that check
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
CXX=${CXX:-clang++}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/openkey-engine-tests.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

build() { # build <engine dir> <output binary>
    "$CXX" -std=c++17 -w -I"$1" "$HERE/engine_harness.cpp" \
        "$1/Engine.cpp" "$1/Vietnamese.cpp" "$1/Macro.cpp" \
        "$1/SmartSwitchKey.cpp" "$1/ConvertTool.cpp" -o "$2"
}

run_row() { # run_row <binary> <options or -> <keys>
    if [ "$2" = "-" ]; then "$1" "$3"; else
        # shellcheck disable=SC2086  # options are intentionally word-split
        "$1" $2 "$3"
    fi
}

if [ "${1:-}" = "compare" ]; then
    [ $# -eq 3 ] || { echo "usage: $0 compare OLD_ENGINE_DIR NEW_ENGINE_DIR" >&2; exit 2; }
    build "$2" "$WORK/old"
    build "$3" "$WORK/new"
    rows=0; diffs=0
    # corpus.txt: one keystroke sequence per line, run under every option set below.
    while IFS= read -r keys || [ -n "$keys" ]; do
        case "$keys" in ''|'#'*) continue ;; esac
        for opts in "-ovInputType=1" "-ovInputType=1 -ovCheckSpelling=0" \
                    "-ovInputType=1 -ovRestoreIfWrongSpelling=1" "-ovInputType=1 -ovUseModernOrthography=0" \
                    "-ovInputType=0" "-ovInputType=0 -ovCheckSpelling=0" \
                    "-ovInputType=0 -ovRestoreIfWrongSpelling=1" "-ovInputType=0 -ovQuickTelex=1"; do
            for k in "$keys" "$keys "; do
                rows=$((rows + 1))
                a=$(run_row "$WORK/old" "$opts" "$k")
                b=$(run_row "$WORK/new" "$opts" "$k")
                if [ "$a" != "$b" ]; then
                    diffs=$((diffs + 1))
                    printf '%s\t[%s]\n    old: %s\n    new: %s\n' "$opts" "$k" "$a" "$b"
                fi
            done
        done
    done < "$HERE/corpus.txt"
    echo "rows=$rows diffs=$diffs"
    exit 0
fi

build "$ROOT/Sources/OpenKey/engine" "$WORK/h"
pass=0; fail=0
TAB=$(printf '\t')
while IFS="$TAB" read -r id opts keys want_text want_bs || [ -n "$id" ]; do
    case "$id" in ''|'#'*) continue ;; esac
    out=$(run_row "$WORK/h" "$opts" "$keys")
    got_text=${out%%"$TAB"bs=*}
    rest=${out#*"$TAB"bs=}
    got_bs=${rest%%"$TAB"*}
    if [ "$got_text" = "$want_text" ] && { [ "$want_bs" = "-" ] || [ "$got_bs" = "$want_bs" ]; }; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s  keys=[%s] opts=%s\n     want [%s] bs=%s\n     got  [%s] bs=%s\n' \
            "$id" "$keys" "$opts" "$want_text" "$want_bs" "$got_text" "$got_bs"
    fi
done < "$HERE/cases.tsv"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
