#!/bin/bash
#  Build the fuzz targets with ASan + UBSan and run each for a time budget.
#
#    test/fuzz/run_fuzzers.sh [SECONDS_PER_TARGET] [TARGET ...]     default: 60 seconds, every fuzz_*.cpp
#
#  The targets use libFuzzer's entry point, but minstd does not compile with clang today (its <compare> relies on
#  GCC accepting builtin <=> without std::strong_ordering, and <tuple> uses pack expansions clang rejects), so they
#  are linked with standalone_driver.cpp and built with g++: random and mutational inputs without coverage feedback.
#  MINIMALCLIB_DIR points at the minimalclib checkout (the formatter uses strnstr/strlcpy from it).  A failing input
#  is left in test/build-fuzz/<target>.last_input.bin; replay it with test/build-fuzz/<target> FILE.  The script
#  exits 1 if any target fails.

set -u

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

SECONDS_PER_TARGET="${1:-60}"
shift || true
TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=($(cd test/fuzz && ls fuzz_*.cpp | sed 's/\.cpp$//'))

MINIMALCLIB_DIR="${MINIMALCLIB_DIR:-../minimalclib}"
OUT=test/build-fuzz
mkdir -p "$OUT"

FLAGS=(-std=c++20 -O1 -g -fno-exceptions -fno-rtti -D__MINIMAL_STD_TEST__
       -fsanitize=address,undefined -fno-sanitize-recover=undefined
       -I. -Iinclude -Itest/shared -idirafter "$MINIMALCLIB_DIR/include")
LIB_SOURCES=(src/format.cpp src/format_formatters.cpp)

status=0

for target in "${TARGETS[@]}"; do
    exe="$OUT/$target"

    if ! g++ "${FLAGS[@]}" "test/fuzz/$target.cpp" test/fuzz/standalone_driver.cpp "${LIB_SOURCES[@]}" src/new.cpp -L"$MINIMALCLIB_DIR/lib/x64" -lminimalclib -o "$exe" > "$exe.build.log" 2>&1; then
        echo "FAILED to build $target: see $exe.build.log"
        grep -m5 "error" "$exe.build.log"
        status=1
        continue
    fi

    ( cd "$OUT" && mkdir -p "$target.work" && cd "$target.work" && ASAN_OPTIONS=detect_leaks=0 "../$target" "$SECONDS_PER_TARGET" 1 > "../$target.log" 2>&1 )
    rc=$?
    cp "$OUT/$target.work/last_input.bin" "$OUT/$target.last_input.bin" 2>/dev/null
    runs=$(grep -m1 "stat::number_of_executed_units" "$OUT/$target.log" | awk '{print $2}')

    if [ $rc -eq 0 ]; then
        echo "ok      $target: ${runs:-?} executions in ${SECONDS_PER_TARGET}s"
    else
        echo "FAILED  $target (rc $rc): see $OUT/$target.log"
        grep -m3 -E "ERROR|SUMMARY|runtime error|deadly signal" "$OUT/$target.log"
        status=1
    fi
done

exit $status
