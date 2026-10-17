#!/bin/bash
#  Build the fuzz targets with ASan + UBSan and run each for a time budget.
#
#    test/fuzz/run_fuzzers.sh [SECONDS_PER_TARGET] [TARGET ...]     default: 60 seconds, every fuzz_*.cpp
#
#  With clang (CLANG_CXX, default clang++) the targets are built with libFuzzer (-fsanitize=fuzzer): coverage-guided,
#  with a corpus kept in test/build-fuzz/<target>.corpus across runs.  Without clang, or with FUZZ_ENGINE=standalone,
#  they are linked with standalone_driver.cpp and built with g++: random and mutational inputs without coverage
#  feedback.  MINIMALCLIB_DIR points at the minimalclib checkout (the formatter uses strnstr/strlcpy from it).  A
#  failing input is left in test/build-fuzz/<target>.last_input.bin; replay it with test/build-fuzz/<target> FILE.
#  The script exits 1 if any target fails.

set -u

cd "$(dirname "${BASH_SOURCE[0]}")/../.."

SECONDS_PER_TARGET="${1:-60}"
shift || true
TARGETS=("$@")
[ ${#TARGETS[@]} -eq 0 ] && TARGETS=($(cd test/fuzz && ls fuzz_*.cpp | sed 's/\.cpp$//'))

MINIMALCLIB_DIR="${MINIMALCLIB_DIR:-../minimalclib}"
OUT=test/build-fuzz
mkdir -p "$OUT"

CLANG_CXX="${CLANG_CXX:-clang++}"
if [ -z "${FUZZ_ENGINE:-}" ]; then
    command -v "$CLANG_CXX" > /dev/null && FUZZ_ENGINE=libfuzzer || FUZZ_ENGINE=standalone
fi

FLAGS=(-std=c++20 -O1 -g -fno-exceptions -fno-rtti -D__MINIMAL_STD_TEST__
       -fno-sanitize-recover=undefined
       -I. -Iinclude -Itest/shared -idirafter "$MINIMALCLIB_DIR/include")
LIB_SOURCES=(src/format.cpp src/format_formatters.cpp)

if [ "$FUZZ_ENGINE" = libfuzzer ]; then
    CXX="$CLANG_CXX"
    FLAGS+=(-fsanitize=fuzzer,address,undefined)
    DRIVER=()
else
    CXX=g++
    FLAGS+=(-fsanitize=address,undefined)
    DRIVER=(test/fuzz/standalone_driver.cpp)
fi

echo "(engine: $FUZZ_ENGINE, $CXX)"

status=0

for target in "${TARGETS[@]}"; do
    exe="$OUT/$target"

    if ! "$CXX" "${FLAGS[@]}" "test/fuzz/$target.cpp" "${DRIVER[@]}" "${LIB_SOURCES[@]}" src/new.cpp -L"$MINIMALCLIB_DIR/lib/x64" -lminimalclib -o "$exe" > "$exe.build.log" 2>&1; then
        echo "FAILED to build $target: see $exe.build.log"
        grep -m5 "error" "$exe.build.log"
        status=1
        continue
    fi

    if [ "$FUZZ_ENGINE" = libfuzzer ]; then
        mkdir -p "$OUT/$target.corpus" "$OUT/$target.work"
        rm -f "$OUT/$target.work"/crash-* "$OUT/$target.work"/timeout-* "$OUT/$target.work"/oom-*
        ASAN_OPTIONS=detect_leaks=0 "$exe" -max_total_time="$SECONDS_PER_TARGET" -print_final_stats=1 \
            -artifact_prefix="$OUT/$target.work/" "$OUT/$target.corpus" > "$OUT/$target.log" 2>&1
        rc=$?
        artifact=$(ls "$OUT/$target.work"/crash-* "$OUT/$target.work"/timeout-* "$OUT/$target.work"/oom-* 2>/dev/null | head -1)
        [ -n "$artifact" ] && cp "$artifact" "$OUT/$target.last_input.bin"
    else
        ( cd "$OUT" && mkdir -p "$target.work" && cd "$target.work" && ASAN_OPTIONS=detect_leaks=0 "../$target" "$SECONDS_PER_TARGET" 1 > "../$target.log" 2>&1 )
        rc=$?
        cp "$OUT/$target.work/last_input.bin" "$OUT/$target.last_input.bin" 2>/dev/null
    fi
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
