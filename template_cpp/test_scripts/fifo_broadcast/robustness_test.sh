#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
BIN_DIR="$ROOT_DIR/bin"
OUTPUT_DIR="/tmp/da_robust_test_$$"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=== FIFO Broadcast 容错性测试 ==="

cd "$ROOT_DIR"
./build.sh > /dev/null 2>&1 || { echo -e "${RED}Build failed${NC}"; exit 1; }

mkdir -p "$OUTPUT_DIR"
trap "rm -rf $OUTPUT_DIR" EXIT

N=5
M=100
EXPECTED=$((N * M))

cat /dev/null > "$OUTPUT_DIR/hosts"
for i in $(seq 1 $N); do
    echo "$i 127.0.0.1 $((11000 + i))" >> "$OUTPUT_DIR/hosts"
done

cat > "$OUTPUT_DIR/config" << EOF
$M
EOF

echo "启动 $N 个进程..."

PIDS=()
for i in $(seq 1 $N); do
    "$BIN_DIR/da_proc" --id $i --hosts "$OUTPUT_DIR/hosts" \
        --output "$OUTPUT_DIR/proc$i.output" "$OUTPUT_DIR/config" \
        > "$OUTPUT_DIR/proc$i.stdout" 2>&1 &
    PIDS+=($!)
done

sleep 2

echo -e "${YELLOW}开始故障注入...${NC}"

# 模拟stress.py的干扰
for attempt in {1..10}; do
    sleep 1
    
    # 随机选择一个进程
    TARGET_IDX=$((RANDOM % N))
    TARGET_PID=${PIDS[$TARGET_IDX]}
    
    ACTION=$((RANDOM % 100))
    
    if [ $ACTION -lt 48 ]; then
        echo "  [干扰$attempt] 暂停进程 $((TARGET_IDX + 1))"
        kill -SIGSTOP $TARGET_PID 2>/dev/null || true
    elif [ $ACTION -lt 96 ]; then
        echo "  [干扰$attempt] 恢复进程 $((TARGET_IDX + 1))"
        kill -SIGCONT $TARGET_PID 2>/dev/null || true
    fi
done

echo -e "${YELLOW}恢复所有暂停的进程...${NC}"
for pid in "${PIDS[@]}"; do
    kill -SIGCONT $pid 2>/dev/null || true
done

echo "等待 30 秒进行消息传播..."
sleep 30

echo "终止进程..."
for pid in "${PIDS[@]}"; do
    kill -SIGTERM $pid 2>/dev/null || true
done
sleep 5

for pid in "${PIDS[@]}"; do
    kill -9 $pid 2>/dev/null || true
done

echo "验证输出..."

PASS=true

# 检查文件存在
for i in $(seq 1 $N); do
    if [ ! -f "$OUTPUT_DIR/proc$i.output" ]; then
        echo -e "${RED}FAIL: proc$i.output not created${NC}"
        PASS=false
    fi
done

if [ "$PASS" == true ]; then
    # 检查广播数量
    for i in $(seq 1 $N); do
        BC=$(grep -c "^b " "$OUTPUT_DIR/proc$i.output" 2>/dev/null || echo 0)
        if [ "$BC" -ne "$M" ]; then
            echo -e "${RED}FAIL: Process $i broadcasted $BC (expected $M)${NC}"
            PASS=false
        fi
    done
fi

if [ "$PASS" == true ]; then
    # 检查交付数量（允许部分失败）
    MIN_DELIVERY=$((EXPECTED * 8 / 10))  # 至少80%
    
    for i in $(seq 1 $N); do
        DC=$(grep -c "^d " "$OUTPUT_DIR/proc$i.output" 2>/dev/null || echo 0)
        if [ "$DC" -lt "$MIN_DELIVERY" ]; then
            echo -e "${YELLOW}WARN: Process $i only delivered $DC/$EXPECTED (< 80%)${NC}"
        fi
        if [ "$DC" -eq "$EXPECTED" ]; then
            echo "  Process $i: 完整交付 $EXPECTED 条消息 ✓"
        fi
    done
fi

if [ "$PASS" == true ]; then
    # 检查FIFO顺序
    for proc in $(seq 1 $N); do
        for sender in $(seq 1 $N); do
            POSITIONS=$(grep "^d $sender " "$OUTPUT_DIR/proc$proc.output" 2>/dev/null | awk '{print $3}')
            if [ -z "$POSITIONS" ]; then continue; fi
            
            SORTED=$(echo "$POSITIONS" | sort -n)
            if [ "$POSITIONS" != "$SORTED" ]; then
                echo -e "${RED}FAIL: Process $proc FIFO violation for sender $sender${NC}"
                PASS=false
                break 2
            fi
        done
    done
fi

if [ "$PASS" == true ]; then
    # 检查去重
    for proc in $(seq 1 $N); do
        UNIQUE=$(grep "^d " "$OUTPUT_DIR/proc$proc.output" 2>/dev/null | sort | uniq | wc -l)
        TOTAL=$(grep -c "^d " "$OUTPUT_DIR/proc$proc.output" 2>/dev/null || echo 0)
        if [ "$UNIQUE" -ne "$TOTAL" ]; then
            echo -e "${RED}FAIL: Process $proc has duplicates${NC}"
            PASS=false
            break
        fi
    done
fi

if [ "$PASS" == true ]; then
    echo -e "${GREEN}PASS: 容错性测试通过！${NC}"
    echo "✓ 所有进程在干扰下完成广播"
    echo "✓ FIFO顺序正确"
    echo "✓ 无重复交付"
else
    echo -e "${RED}FAIL: 测试失败${NC}"
    exit 1
fi