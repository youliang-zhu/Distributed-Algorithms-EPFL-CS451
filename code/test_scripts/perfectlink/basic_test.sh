#!/bin/bash

# Basic correctness test for Perfect Links
# Tests: 2 processes, small message count, simple validation

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
BIN_DIR="$ROOT_DIR/bin"
OUTPUT_DIR="/tmp/da_test_$$"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=== Perfect Links Basic Test ==="

# Build
echo "Building..."
cd "$ROOT_DIR"
./build.sh > /dev/null 2>&1 || { echo -e "${RED}Build failed${NC}"; exit 1; }

# Setup
mkdir -p "$OUTPUT_DIR"
trap "rm -rf $OUTPUT_DIR" EXIT

# Test config: 2 processes, 10 messages
cat > "$OUTPUT_DIR/hosts" << EOF
1 127.0.0.1 11001
2 127.0.0.1 11002
EOF

cat > "$OUTPUT_DIR/config" << EOF
10 2
EOF

echo "Starting processes..."

# Start receiver (process 2)
"$BIN_DIR/da_proc" --id 2 --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc2.output" "$OUTPUT_DIR/config" &
PID2=$!
sleep 0.5

# Start sender (process 1)
"$BIN_DIR/da_proc" --id 1 --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc1.output" "$OUTPUT_DIR/config" &
PID1=$!

# Wait
sleep 3

# Stop processes
kill -SIGTERM $PID1 $PID2 2>/dev/null || true
sleep 1
kill -9 $PID1 $PID2 2>/dev/null || true

# Validate
echo "Validating output..."

if [ ! -f "$OUTPUT_DIR/proc1.output" ] || [ ! -f "$OUTPUT_DIR/proc2.output" ]; then
    echo -e "${RED}FAIL: Output files not created${NC}"
    exit 1
fi

# Check sender broadcast 10 messages
BROADCAST_COUNT=$(grep -c "^b " "$OUTPUT_DIR/proc1.output" 2>/dev/null || echo 0)
if [ "$BROADCAST_COUNT" -ne 10 ]; then
    echo -e "${RED}FAIL: Expected 10 broadcasts, got $BROADCAST_COUNT${NC}"
    exit 1
fi

# Check receiver delivered 10 messages
DELIVERY_COUNT=$(grep -c "^d " "$OUTPUT_DIR/proc2.output" 2>/dev/null || echo 0)
if [ "$DELIVERY_COUNT" -ne 10 ]; then
    echo -e "${RED}FAIL: Expected 10 deliveries, got $DELIVERY_COUNT${NC}"
    cat "$OUTPUT_DIR/proc2.output"
    exit 1
fi

# Check sequence numbers 1-10
for i in {1..10}; do
    if ! grep -q "^d 1 $i$" "$OUTPUT_DIR/proc2.output"; then
        echo -e "${RED}FAIL: Missing delivery of message $i${NC}"
        cat "$OUTPUT_DIR/proc2.output"
        exit 1
    fi
done

echo -e "${GREEN}PASS: All checks passed${NC}"
echo "Sender broadcasts: $BROADCAST_COUNT"
echo "Receiver deliveries: $DELIVERY_COUNT"
