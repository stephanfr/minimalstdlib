#!/bin/bash
#  Run TLC on every model configuration under tlaplus/ and print one summary line per configuration.
#
#    ./run_all.sh                 every configuration except the slow, non-exhaustive ones listed below
#    ALL=1 ./run_all.sh           include them too
#    ./run_all.sh skiplist        only configurations whose path contains "skiplist"
#
#  Environment: TLC_WORKERS (default 2), TLC_TIMEOUT seconds per configuration (default 3600), TLA_JAR (default
#  tlaplus/tla2tools.jar, downloaded if missing).  Each line is checked against the expected result: the *bug*,
#  *current* (except slot_blocks/current_one), *_genwrap, steal_resets_tag and tag_wraps configurations model known
#  defects or mutations and are expected to fail; every other configuration is expected to pass.  A line that
#  differs (or times out) is marked UNEXPECTED and the script exits 1.
#
#  A spec is chosen per configuration: the only .tla file in its directory, or else the .tla whose name is the
#  longest prefix of the configuration's name.  The committed .tla files carry their PlusCal translation.

TLA_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TLA_JAR="${TLA_JAR:-$TLA_DIR/tla2tools.jar}"
FILTER="${1:-}"
SLOW="fixed_irq2.cfg fixed_irq2_small.cfg BitblockSet.cfg BitblockSet_strict.cfg BitblockSet_stress.cfg LockfreeSkiplist_safety_2t.cfg"

if [ ! -f "$TLA_JAR" ]; then
    wget -q "https://github.com/tlaplus/tlaplus/releases/download/v1.8.0/tla2tools.jar" -O "$TLA_JAR" || exit 1
fi

expected_to_fail() {
    case "$1" in
        current_one) return 1 ;;  #  the single-list baseline passes (slot_blocks/README.md)
        *bug*|*current*|*_genwrap|steal_resets_tag|tag_wraps) return 0 ;;
        *) return 1 ;;
    esac
}

status=0

for cfg in $(cd "$TLA_DIR" && find . -name '*.cfg' | sort); do
    name="$(basename "$cfg" .cfg)"
    dir="$TLA_DIR/$(dirname "$cfg")"

    [ -n "$FILTER" ] && [[ "$cfg" != *"$FILTER"* ]] && continue
    [ -z "$ALL" ] && [[ " $SLOW " == *" $name.cfg "* ]] && continue

    spec=""
    for tla in "$dir"/*.tla; do
        t="$(basename "$tla" .tla)"
        if [ "$(ls "$dir"/*.tla | wc -l)" -eq 1 ] || { [[ "$name" == "$t"* ]] && [ ${#t} -gt ${#spec} ]; }; then
            spec="$t"
        fi
    done

    meta="$(mktemp -d)"
    log="$(mktemp)"
    start=$(date +%s)
    ( cd "$dir" && timeout "${TLC_TIMEOUT:-3600}" java -Xmx4g -XX:+UseParallelGC -cp "$TLA_JAR" tlc2.TLC \
        -workers "${TLC_WORKERS:-2}" -metadir "$meta" -config "$name.cfg" "$spec.tla" > "$log" 2>&1 )
    rc=$?
    verdict=$(grep -m1 -E "Model checking completed. No error|is violated|Temporal properties were violated|Deadlock reached|evaluated to FALSE|Error:" "$log")
    [ $rc -eq 124 ] && verdict="TIMEOUT"
    states=$(grep "distinct states found" "$log" | tail -1 | sed 's/.*generated, //')
    outcome="ok"
    if [ "$verdict" = "TIMEOUT" ]; then
        outcome="UNEXPECTED"
    elif expected_to_fail "$name"; then
        [[ "$verdict" == *"No error"* ]] && outcome="UNEXPECTED"
    else
        [[ "$verdict" == *"No error"* ]] || outcome="UNEXPECTED"
    fi
    [ "$outcome" = "UNEXPECTED" ] && status=1

    printf '%-10s %-60s %5ss  %s | %s\n' "$outcome" "${cfg#./}" "$(( $(date +%s) - start ))" "$verdict" "$states"
    rm -rf "$meta" "$log"
    find "$dir" -name '*_TTrace_*' -delete
done

exit $status
