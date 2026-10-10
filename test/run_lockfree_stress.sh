#!/usr/bin/env bash
#
#  Repeatedly runs selected lock-free single-arena tests, under the normal build and under TSan.
#
#  Usage:
#      test/run_lockfree_stress.sh [NORMAL_ITERATIONS] [TSAN_ITERATIONS] [TEST ...]
#
#      NORMAL_ITERATIONS   passes of each test in the normal build (default 200, 0 to skip)
#      TSAN_ITERATIONS     passes of each test under TSan (default 200, 0 to skip)
#      TEST ...            test names in LockfreeSingleArenaMemoryResourceTests
#                          (default: ConcurrentAllocDeallocChurn ConcurrentChurnFullBlockIntegrity FrontierReclaimRace)
#
#  Environment:
#      FULL_TSAN=0         skip the single TSan run of the whole lock-free binary (default 1)
#      GROUP=...           CppUTest group (default LockfreeSingleArenaMemoryResourceTests)
#      EXE=..., TSAN_EXE=...  override the binaries (default: found under test/)
#
#  Run from the repository root.  Exits non-zero if anything failed; the log of each first failure is kept
#  in ./stress-logs/.

set -u

NORMAL_ITERATIONS="${1:-200}"
TSAN_ITERATIONS="${2:-200}"
shift $(( $# > 2 ? 2 : $# ))

if [ "$#" -gt 0 ]; then
    TESTS=("$@")
else
    TESTS=(ConcurrentAllocDeallocChurn ConcurrentChurnFullBlockIntegrity FrontierReclaimRace)
fi

GROUP="${GROUP:-LockfreeSingleArenaMemoryResourceTests}"
FULL_TSAN="${FULL_TSAN:-1}"

for n in "$NORMAL_ITERATIONS" "$TSAN_ITERATIONS"; do
    if ! [[ "$n" =~ ^[0-9]+$ ]]; then
        echo "Iteration counts must be non-negative integers (got '$n')" >&2
        exit 2
    fi
done

EXE="${EXE:-$(find test -name cpputest_lockfree_correctness.exe -not -path '*tsan*' 2>/dev/null | head -1)}"
TSAN_EXE="${TSAN_EXE:-$(find test -name cpputest_lockfree_correctness_tsan.exe 2>/dev/null | head -1)}"

#  TSan: stop at the first race so the loop sees a failing exit code.
export TSAN_OPTIONS="${TSAN_OPTIONS:-halt_on_error=1 exitcode=66 second_deadlock_stack=1}"

#  Run TSan binaries without ASLR when the container allows it (GCC 13's runtime can otherwise fail to map its
#  shadow memory); fall back to running them directly.
NO_ASLR=()
if setarch "$(uname -m)" -R true 2>/dev/null; then
    NO_ASLR=(setarch "$(uname -m)" -R)
else
    echo "note: setarch -R not permitted here; running TSan binaries with ASLR (startup may fail intermittently)"
fi

LOG_DIR="stress-logs"
mkdir -p "$LOG_DIR"

failures=0

#  run_loop LABEL ITERATIONS COMMAND...   (the test name is appended as -sn <name>)
run_loop()
{
    local label="$1" iterations="$2"
    shift 2

    for t in "${TESTS[@]}"; do
        local log="$LOG_DIR/${label}-${t}.log"
        local i

        for (( i = 1; i <= iterations; i++ )); do
            if ! "$@" -sg "$GROUP" -sn "$t" > "$log" 2>&1; then
                echo "FAIL [$label] $t on run $i of $iterations (log: $log)"
                tail -n 20 "$log"
                failures=$(( failures + 1 ))
                continue 2
            fi
        done

        echo "ok   [$label] $t: $iterations passes"
        rm -f "$log"
    done
}

if [ "$NORMAL_ITERATIONS" -gt 0 ]; then
    if [ -z "$EXE" ] || [ ! -x "$EXE" ]; then
        echo "Normal lock-free binary not found; build it first (make test-correctness) or set EXE=" >&2
        exit 2
    fi
    echo "=== Normal build: $EXE"
    run_loop normal "$NORMAL_ITERATIONS" "$EXE"
fi

if [ "$TSAN_ITERATIONS" -gt 0 ] || [ "$FULL_TSAN" = "1" ]; then
    if [ -z "$TSAN_EXE" ] || [ ! -x "$TSAN_EXE" ]; then
        echo "TSan lock-free binary not found; build it first (make test-correctness-tsan) or set TSAN_EXE=" >&2
        exit 2
    fi

    if [ "$FULL_TSAN" = "1" ]; then
        echo "=== TSan, whole lock-free binary: $TSAN_EXE"
        log="$LOG_DIR/tsan-full.log"
        if "${NO_ASLR[@]}" "$TSAN_EXE" > "$log" 2>&1; then
            echo "ok   [tsan-full] $(grep -E '^OK \(' "$log" | tail -1)"
            rm -f "$log"
        else
            echo "FAIL [tsan-full] exit $? (log: $log); first report:"
            grep -m1 -A12 "WARNING: ThreadSanitizer" "$log" || tail -n 20 "$log"
            failures=$(( failures + 1 ))
        fi
    fi

    if [ "$TSAN_ITERATIONS" -gt 0 ]; then
        echo "=== TSan, per test: $TSAN_EXE"
        run_loop tsan "$TSAN_ITERATIONS" "${NO_ASLR[@]}" "$TSAN_EXE"
    fi
fi

if [ "$failures" -eq 0 ]; then
    echo "=== All passed"
    rmdir "$LOG_DIR" 2>/dev/null
    exit 0
fi

echo "=== $failures failure(s); logs in $LOG_DIR/"
exit 1
