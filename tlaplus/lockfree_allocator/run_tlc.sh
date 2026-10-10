#!/bin/bash
# Translate the PlusCal in LockfreeSingleArena.tla and run TLC on one configuration.
#
#   ./run_tlc.sh [config] [extra TLC args...]
#   ./run_tlc.sh fixed_small.cfg
#   ./run_tlc.sh bug_n3only_stale.cfg -simulate -depth 2000

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TLA_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TLA_JAR="$TLA_DIR/tla2tools.jar"
TLA_VERSION="1.8.0"
SPEC="LockfreeSingleArena.tla"

CONFIG="${1:-fixed_small.cfg}"
shift || true

if [ ! -f "$TLA_JAR" ]; then
    echo "Downloading TLA+ tools v${TLA_VERSION}..."
    wget -q "https://github.com/tlaplus/tlaplus/releases/download/v${TLA_VERSION}/tla2tools.jar" -O "$TLA_JAR"
fi

if ! command -v java &> /dev/null; then
    echo "Error: Java is required but not found. Install with: sudo apt install default-jre"
    exit 1
fi

TMP_DIR="$(mktemp -d /tmp/tlc-XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

cd "$SCRIPT_DIR"

echo "=== Translating PlusCal ==="
java -cp "$TLA_JAR" pcal.trans -nocfg "$SPEC"

echo "=== Running TLC: $SPEC with $CONFIG ==="
java -Xmx4g -XX:+UseParallelGC \
    -cp "$TLA_JAR" tlc2.TLC \
    -config "$CONFIG" \
    -metadir "$TMP_DIR" \
    -workers auto \
    "$@" \
    "$SPEC"
