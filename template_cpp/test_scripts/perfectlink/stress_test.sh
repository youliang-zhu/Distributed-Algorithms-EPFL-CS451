#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
TOOLS_DIR="$(cd "$ROOT_DIR/.." && pwd)/tools"
OUTPUT_DIR="/tmp/da_stress_$$"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== Perfect Links Stress Test ==="

echo "Building..."
cd "$ROOT_DIR"
./build.sh > /dev/null 2>&1 || { echo -e "${RED}Build failed${NC}"; exit 1; }

mkdir -p "$OUTPUT_DIR"
trap "echo ''; echo 'Output directory: $OUTPUT_DIR (not removed for debugging)'" EXIT

echo "Running stress test: 5 processes, 1000 messages"
echo "Debug output directory: $OUTPUT_DIR"
python3 "$TOOLS_DIR/stress.py" perfect \
    -r "$ROOT_DIR/run.sh" \
    -l "$OUTPUT_DIR" \
    -p 5 \
    -m 1000

echo ""
echo "=== Checking debug logs ==="
echo "Listing files in $OUTPUT_DIR:"
ls -lh "$OUTPUT_DIR"
echo ""
for i in {1..5}; do
    STDERR_FILE="$OUTPUT_DIR/proc0${i}.stderr"
    echo "--- Process $i stderr ---"
    if [ -f "$STDERR_FILE" ]; then
        if [ -s "$STDERR_FILE" ]; then
            tail -100 "$STDERR_FILE"
        else
            echo "(empty)"
        fi
    else
        echo "(file not found)"
    fi
    echo ""
done

echo ""
echo "Validating outputs..."

EXPECTED_BROADCASTS=1000
EXPECTED_DELIVERIES=1000

for i in {1..5}; do
    OUTPUT_FILE="$OUTPUT_DIR/proc0${i}.output"
    
    if [ ! -f "$OUTPUT_FILE" ]; then
        echo -e "${RED}FAIL: proc0${i}.output not found${NC}"
        exit 1
    fi
    
    if [ $i -eq 1 ]; then
        BROADCAST_COUNT=$(grep -c "^b " "$OUTPUT_FILE" 2>/dev/null || echo 0)
        if [ "$BROADCAST_COUNT" -lt "$EXPECTED_BROADCASTS" ]; then
            echo -e "${RED}FAIL: Process 1 expected at least $EXPECTED_BROADCASTS broadcasts, got $BROADCAST_COUNT${NC}"
            exit 1
        fi
        echo "Process 1: $BROADCAST_COUNT broadcasts"
    else
        DELIVERY_COUNT=$(grep -c "^d 1 " "$OUTPUT_FILE" 2>/dev/null || echo 0)
        if [ "$DELIVERY_COUNT" -lt "$EXPECTED_DELIVERIES" ]; then
            echo -e "${RED}FAIL: Process $i expected $EXPECTED_DELIVERIES deliveries, got $DELIVERY_COUNT${NC}"
            exit 1
        fi
        echo "Process $i: $DELIVERY_COUNT deliveries"
    fi
done

echo -e "${GREEN}PASS: Stress test completed successfully${NC}"
