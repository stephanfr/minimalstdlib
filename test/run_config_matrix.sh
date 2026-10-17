#!/bin/bash
#  Build and run the correctness suites under each non-default build setting.  Defects hide in configurations the
#      default test build never compiles (MINIMAL_STD_POINTER_TOP_BITS=ONES decoded null wrongly; the TRAP allocation
#      policy skipped its fallbacks), so each setting gets its own build directory and run.
#
#    test/run_config_matrix.sh [CONFIG ...]      default: every configuration below
#
#  Extra make arguments (CPPUTEST_PATH=..., MINIMALCLIB_DIR=...) are taken from MAKE_ARGS.  Exits 1 if any run fails.

set -u

cd "$(dirname "${BASH_SOURCE[0]}")/.."

declare -A DEFINES=(
    [failure-policy-test]="-DMINIMAL_STD_FAILURE_POLICY=MINIMAL_STD_FAILURE_POLICY_TEST"
    [alloc-policy-trap]="-DMINIMAL_STD_FAILURE_POLICY=MINIMAL_STD_FAILURE_POLICY_TEST -DMINIMAL_STD_ALLOCATION_FAILURE_POLICY=MINIMAL_STD_ALLOCATION_FAILURE_POLICY_TRAP"
    [top-bits-sign-extend-47]="-DMINIMAL_STD_POINTER_TOP_BITS=MINIMAL_STD_POINTER_TOP_BITS_SIGN_EXTEND_47"
    [top-bits-ones]="-DMINIMAL_STD_POINTER_TOP_BITS=MINIMAL_STD_POINTER_TOP_BITS_ONES"
    [clang]=""
)

#  Configurations built with another compiler (CC and LD); the default settings otherwise.  clang is skipped from the
#      default list when it is not installed.
CLANG_CXX="${CLANG_CXX:-clang++}"
declare -A COMPILER=(
    [clang]="$CLANG_CXX"
)

#  Host addresses lie in the lower half, so under ONES only the codec's own tests are meaningful: every structure
#      that stores a real pointer in a tagged_ptr would decode it into the upper half.
declare -A GROUPS_ONLY=(
    [top-bits-ones]="-g TaggedPtrTests"
)

CONFIGS=("$@")
if [ ${#CONFIGS[@]} -eq 0 ]; then
    CONFIGS=(failure-policy-test alloc-policy-trap top-bits-sign-extend-47 top-bits-ones)
    if command -v "$CLANG_CXX" > /dev/null; then
        CONFIGS+=(clang)
    else
        echo "($CLANG_CXX not found: skipping the clang configuration)"
    fi
fi

status=0

for config in "${CONFIGS[@]}"; do
    if [[ ! -v DEFINES[$config] ]]; then
        echo "unknown configuration: $config"
        exit 2
    fi

    defines="${DEFINES[$config]}"
    compiler="${COMPILER[$config]:-}"
    compiler_args=()
    [ -n "$compiler" ] && compiler_args=(CC="$compiler" LD="$compiler")

    dir="test/build-config/$config"
    mkdir -p "$dir"
    echo "=== $config (${compiler:+$compiler }$defines)"

    if ! make -f Makefile.test.mk -j4 ${MAKE_ARGS:-} "${compiler_args[@]}" \
            CDEFINES="-D__MINIMAL_STD_TEST__ -DMINIMAL_STD_CONFIG_MATRIX $defines" \
            CORRECTNESS_OBJ_DIR="$dir/correctness" LIB_OBJ_DIR="$dir/lib" \
            "$dir/correctness/cpputest_correctness.exe" "$dir/correctness/cpputest_lockfree_correctness.exe" > "$dir.build.log" 2>&1; then
        echo "FAILED to build: see $dir.build.log"
        grep -m5 "error" "$dir.build.log"
        status=1
        continue
    fi

    groups="${GROUPS_ONLY[$config]:-}"

    for exe in cpputest_correctness.exe cpputest_lockfree_correctness.exe; do
        [ -n "$groups" ] && [ "$exe" = cpputest_lockfree_correctness.exe ] && continue

        result=$("./$dir/correctness/$exe" $groups 2>&1 | grep -E "^(OK|Errors) \(|Failure in" | head -5)
        echo "$exe: $result"
        [[ "$result" == OK* ]] || status=1
    done
done

exit $status
