#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
TOOLS_DIR="$(cd "$ROOT_DIR/.." && pwd)/tools"
OUTPUT_DIR="/tmp/da_network_$$"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=== Perfect Links Network Delay Test ==="
echo ""

echo "Building..."
cd "$ROOT_DIR"
./build.sh > /dev/null 2>&1 || { echo -e "${RED}Build failed${NC}"; exit 1; }

mkdir -p "$OUTPUT_DIR"
trap "echo ''; echo 'Output directory: $OUTPUT_DIR (not removed for debugging)'" EXIT

echo -e "${YELLOW}Starting network emulation (delay/loss/reorder)...${NC}"
echo "This will add network conditions to loopback interface"
echo ""

# Run tc.py in background, auto-press Enter after test completes
(sleep 120; echo) | python3 "$TOOLS_DIR/tc.py" &
TC_PID=$!
sleep 3
echo "Network conditions applied"
echo ""

echo "Running test: 3 processes, 100 messages"
cd "$ROOT_DIR"

# Create config file
cat > "$OUTPUT_DIR/config" << EOF
100 1
EOF

# Create hosts file
cat > "$OUTPUT_DIR/hosts" << EOF
1 localhost 11001
2 localhost 11002
3 localhost 11003
EOF

# Start receiver (process 1)
./bin/da_proc --id 1 --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc01.output" "$OUTPUT_DIR/config" > "$OUTPUT_DIR/proc01.stdout" 2>&1 &
PID1=$!

# Start senders (process 2, 3)
./bin/da_proc --id 2 --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc02.output" "$OUTPUT_DIR/config" > "$OUTPUT_DIR/proc02.stdout" 2>&1 &
PID2=$!

./bin/da_proc --id 3 --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc03.output" "$OUTPUT_DIR/config" > "$OUTPUT_DIR/proc03.stdout" 2>&1 &
PID3=$!

echo "Processes started: PID1=$PID1, PID2=$PID2, PID3=$PID3"
echo "Waiting for senders to complete (with network delays)..."
echo "Checking process status every 5 seconds..."

# Wait for senders with timeout and status checks
WAIT_COUNT=0
MAX_WAIT=60  # 5 minutes max
while kill -0 $PID2 2>/dev/null || kill -0 $PID3 2>/dev/null; do
    sleep 5
    WAIT_COUNT=$((WAIT_COUNT + 1))
    
    # Check process status
    if kill -0 $PID2 2>/dev/null; then
        PROC2_STATUS="running"
    else
        PROC2_STATUS="exited"
    fi
    
    if kill -0 $PID3 2>/dev/null; then
        PROC3_STATUS="running"
    else
        PROC3_STATUS="exited"
    fi
    
    echo "  [$((WAIT_COUNT * 5))s] Process 2: $PROC2_STATUS, Process 3: $PROC3_STATUS"
    
    # Check stdout for debug info
    if [ -f "$OUTPUT_DIR/proc02.stdout" ]; then
        LAST_LINE=$(tail -1 "$OUTPUT_DIR/proc02.stdout" 2>/dev/null || echo "")
        if [ ! -z "$LAST_LINE" ]; then
            echo "      Process 2 last log: $LAST_LINE"
        fi
    fi
    
    if [ $WAIT_COUNT -ge $MAX_WAIT ]; then
        echo -e "${RED}TIMEOUT: Senders did not complete within 5 minutes${NC}"
        echo "Killing processes and examining logs..."
        kill -TERM $PID2 $PID3 $PID1 2>/dev/null
        
        echo ""
        echo "=== Process 2 stdout (last 30 lines) ==="
        tail -30 "$OUTPUT_DIR/proc02.stdout"
        echo ""
        echo "=== Process 3 stdout (last 30 lines) ==="
        tail -30 "$OUTPUT_DIR/proc03.stdout"
        
        kill -TERM $TC_PID 2>/dev/null
        wait $TC_PID 2>/dev/null
        exit 1
    fi
done

echo "Senders completed, waiting 2 seconds for final deliveries..."
sleep 2

# Stop receiver
echo "Stopping receiver (process 1)..."
kill -TERM $PID1 2>/dev/null
wait $PID1 2>/dev/null
echo "Receiver stopped"

# Stop tc.py - send Enter to stdin and kill if needed
echo "Stopping network emulation..."
echo "" > /proc/$TC_PID/fd/0 2>/dev/null || true
sleep 1
kill -TERM $TC_PID 2>/dev/null || true
sleep 1
kill -KILL $TC_PID 2>/dev/null || true
echo "Network emulation stopped"

echo ""
echo "Validating outputs..."

# Validate broadcasts
BROADCAST_COUNT=$(grep -c "^b " "$OUTPUT_DIR/proc02.output" 2>/dev/null || echo 0)
if [ "$BROADCAST_COUNT" -ne 100 ]; then
    echo -e "${RED}FAIL: Process 2 expected 100 broadcasts, got $BROADCAST_COUNT${NC}"
    exit 1
fi

BROADCAST_COUNT=$(grep -c "^b " "$OUTPUT_DIR/proc03.output" 2>/dev/null || echo 0)
if [ "$BROADCAST_COUNT" -ne 100 ]; then
    echo -e "${RED}FAIL: Process 3 expected 100 broadcasts, got $BROADCAST_COUNT${NC}"
    exit 1
fi

# Validate deliveries
DELIVERY_COUNT_2=$(grep -c "^d 2 " "$OUTPUT_DIR/proc01.output" 2>/dev/null || echo 0)
DELIVERY_COUNT_3=$(grep -c "^d 3 " "$OUTPUT_DIR/proc01.output" 2>/dev/null || echo 0)

if [ "$DELIVERY_COUNT_2" -ne 100 ]; then
    echo -e "${RED}FAIL: Process 1 expected 100 deliveries from process 2, got $DELIVERY_COUNT_2${NC}"
    exit 1
fi

if [ "$DELIVERY_COUNT_3" -ne 100 ]; then
    echo -e "${RED}FAIL: Process 1 expected 100 deliveries from process 3, got $DELIVERY_COUNT_3${NC}"
    exit 1
fi

echo "Process 2: 100 broadcasts"
echo "Process 3: 100 broadcasts"
echo "Process 1: $DELIVERY_COUNT_2 deliveries from process 2"
echo "Process 1: $DELIVERY_COUNT_3 deliveries from process 3"

echo -e "${GREEN}PASS: Network delay test completed successfully${NC}"
echo "Network conditions: delay=200ms±50ms, loss=10%±25%, reorder=25%±50%"
