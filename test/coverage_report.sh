#!/bin/bash
#  Line and function coverage of include/ and src/ by the correctness suites, per file, lowest first.
#
#    test/coverage_report.sh [FILE ...]      FILEs (e.g. include/vector) also list their uncovered lines
#
#  Needs gcov (from GCC) and python3; CPPUTEST_PATH / MINIMALCLIB_DIR as for the other targets (via MAKE_ARGS).
#  Header templates count only where they are instantiated.  Tests that run in a forked child and leave with
#  _exit() write no coverage data, so their lines show as uncovered.

set -eu

cd "$(dirname "${BASH_SOURCE[0]}")/.."

EXE=test/coverage/correctness/cpputest_correctness_coverage.exe
JSON=test/coverage/json

find test/coverage -name '*.gcda' -delete 2>/dev/null || true

make -f Makefile.test.mk -j4 ${MAKE_ARGS:-} "$EXE" > test/coverage/build.log 2>&1
"./$EXE" > test/coverage/run.log 2>&1 || { echo "suite failed: see test/coverage/run.log"; exit 1; }

rm -rf "$JSON" && mkdir -p "$JSON"
( cd "$JSON" && for gcda in ../correctness/*.gcda ../lib/*.gcda; do gcov -j -o "$(dirname "$gcda")" "$gcda" > /dev/null 2>&1 || true; done )

python3 -I test/coverage_merge.py "$PWD" "$JSON" "$@"
