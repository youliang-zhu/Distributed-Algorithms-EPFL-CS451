#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
BIN_DIR="$ROOT_DIR/bin"
OUTPUT_DIR="/tmp/da_fifo_test_$$"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

echo "=== FIFO Broadcast Basic Test ==="

mkdir -p "$OUTPUT_DIR"
# trap "rm -rf $OUTPUT_DIR" EXIT

cat > "$OUTPUT_DIR/hosts" << EOF
1 127.0.0.1 11001
2 127.0.0.1 11002
3 127.0.0.1 11003
EOF

cat > "$OUTPUT_DIR/config" << EOF
5
EOF

echo "Starting 3 processes..."

for i in 1 2 3; do
    "$BIN_DIR/da_proc" --id $i --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc$i.output" "$OUTPUT_DIR/config" &
    eval "PID$i=$!"
done

sleep 5

for i in 1 2 3; do
    eval "kill -SIGTERM \$PID$i 2>/dev/null || true"
done
sleep 5

echo "Validating output..."

for i in 1 2 3; do
    if [ ! -f "$OUTPUT_DIR/proc$i.output" ]; then
        echo -e "${RED}FAIL: proc$i.output not created${NC}"
        exit 1
    fi
done

for i in 1 2 3; do
    BROADCAST_COUNT=$(grep -c "^b " "$OUTPUT_DIR/proc$i.output" 2>/dev/null || echo 0)
    if [ "$BROADCAST_COUNT" -ne 5 ]; then
        echo -e "${RED}FAIL: Process $i expected 5 broadcasts, got $BROADCAST_COUNT${NC}"
        exit 1
    fi
done

for i in 1 2 3; do
    DELIVERY_COUNT=$(grep -c "^d " "$OUTPUT_DIR/proc$i.output" 2>/dev/null || echo 0)
    if [ "$DELIVERY_COUNT" -ne 15 ]; then
        echo -e "${RED}FAIL: Process $i expected 15 deliveries, got $DELIVERY_COUNT${NC}"
        cat "$OUTPUT_DIR/proc$i.output"
        exit 1
    fi
done

for proc in 1 2 3; do
    for sender in 1 2 3; do
        for seq in {1..5}; do
            if ! grep -q "^d $sender $seq$" "$OUTPUT_DIR/proc$proc.output"; then
                echo -e "${RED}FAIL: Process $proc missing delivery d $sender $seq${NC}"
                cat "$OUTPUT_DIR/proc$proc.output"
                exit 1
            fi
        done
    done
done

for proc in 1 2 3; do
    for sender in 1 2 3; do
        POSITIONS=$(grep "^d $sender " "$OUTPUT_DIR/proc$proc.output" | awk '{print $3}')
        SORTED=$(echo "$POSITIONS" | sort -n)
        if [ "$POSITIONS" != "$SORTED" ]; then
            echo -e "${RED}FAIL: Process $proc FIFO violation for sender $sender${NC}"
            echo "Got: $POSITIONS"
            echo "Expected: $SORTED"
            exit 1
        fi
    done
done

for proc in 1 2 3; do
    UNIQUE_COUNT=$(sort "$OUTPUT_DIR/proc$proc.output" | uniq | wc -l)
    TOTAL_COUNT=$(wc -l < "$OUTPUT_DIR/proc$proc.output")
    if [ "$UNIQUE_COUNT" -ne "$TOTAL_COUNT" ]; then
        echo -e "${RED}FAIL: Process $proc has duplicates${NC}"
        exit 1
    fi
done

echo -e "${GREEN}PASS: All checks passed${NC}"
echo "✓ Each process broadcast 5 messages"
echo "✓ Each process delivered 15 messages (3×5)"
echo "✓ FIFO order maintained for all senders"
echo "✓ No duplicates detected"