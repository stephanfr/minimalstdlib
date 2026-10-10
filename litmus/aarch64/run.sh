#!/bin/bash
#  Run every litmus test here with herd7 and the ARMv8 (AArch64) memory model.
#
#    HERD=/path/to/herd7 ./run.sh            (default: herd7 on PATH)
#    HERD_ARGS="-set-libdir <herdtools7>/herd/libdir" ./run.sh   (for an uninstalled build)
#
#  herd7 is part of herdtools7 (https://github.com/herd/herdtools7; `opam install herdtools7`, or build it with
#  dune).  Each line shows whether the outcome in the test's `exists` clause can happen: Never or Sometimes.

HERD="${HERD:-herd7}"
cd "$(dirname "$0")"

for t in *.litmus; do
    printf '%-36s %s\n' "${t%.litmus}" "$("$HERD" $HERD_ARGS -model aarch64.cat "$t" | awk '/^Observation/ {print $3}')"
done
