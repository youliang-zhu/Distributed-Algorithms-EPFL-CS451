#!/bin/bash

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
BIN_DIR="$ROOT_DIR/bin"
OUTPUT_DIR="/tmp/da_diagnostic_$$"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=== FIFO Broadcast 诊断测试 ===${NC}"
echo "目标：诊断老师测试系统中出现的两个问题"
echo "1. Process 3 delivered nothing from sender 1"
echo "2. Violation of uniform agreement"
echo ""

mkdir -p "$OUTPUT_DIR"

# ============================================================================
# 测试 1: 模拟 "Process 3 delivered nothing from sender 1"
# 可能原因：
# - 启动延迟不足，P3 启动太晚错过 P1 的消息
# - P3 的接收线程有问题
# - P1->P3 的网络路径有问题
# ============================================================================

echo -e "${YELLOW}[测试 1] 诊断 'Process 3 delivered nothing from sender 1'${NC}"
echo "场景：快速启动进程，模拟启动延迟不足的情况"

TEST1_DIR="$OUTPUT_DIR/test1"
mkdir -p "$TEST1_DIR"

cat > "$TEST1_DIR/hosts" << EOF
1 127.0.0.1 11001
2 127.0.0.1 11002
3 127.0.0.1 11003
EOF

cat > "$TEST1_DIR/config" << EOF
10
EOF

# 测试 1a: P1 立即启动，P3 延迟 2 秒启动
echo "  [1a] P1 先启动，P3 延迟 2 秒..."
"$BIN_DIR/da_proc" --id 1 --hosts "$TEST1_DIR/hosts" --output "$TEST1_DIR/proc1.output" "$TEST1_DIR/config" > "$TEST1_DIR/proc1.log" 2>&1 &
PID1=$!

"$BIN_DIR/da_proc" --id 2 --hosts "$TEST1_DIR/hosts" --output "$TEST1_DIR/proc2.output" "$TEST1_DIR/config" > "$TEST1_DIR/proc2.log" 2>&1 &
PID2=$!

sleep 2

"$BIN_DIR/da_proc" --id 3 --hosts "$TEST1_DIR/hosts" --output "$TEST1_DIR/proc3.output" "$TEST1_DIR/config" > "$TEST1_DIR/proc3.log" 2>&1 &
PID3=$!

sleep 15

kill -SIGTERM $PID1 $PID2 $PID3 2>/dev/null || true
sleep 2
kill -9 $PID1 $PID2 $PID3 2>/dev/null || true

# 检查 P3 是否收到了 P1 的消息
P3_FROM_P1=$(grep -c "^d 1 " "$TEST1_DIR/proc3.output" 2>/dev/null || echo 0)
echo "  结果: P3 从 P1 交付了 $P3_FROM_P1 条消息"

if [ "$P3_FROM_P1" -eq 0 ]; then
    echo -e "  ${RED}✗ 复现问题: P3 没有收到 P1 的任何消息！${NC}"
    echo "  可能原因："
    echo "    - 启动延迟 (1000ms) 不足以覆盖最慢进程的启动时间"
    echo "    - P1 的消息在 P3 socket 绑定前就发送完毕并丢失"
    echo "  建议："
    echo "    - 增加启动延迟到 2000-3000ms"
    echo "    - 或实现进程间同步机制（如 barrier）"
elif [ "$P3_FROM_P1" -lt 10 ]; then
    echo -e "  ${YELLOW}⚠ 部分丢失: P3 只收到部分消息 ($P3_FROM_P1/10)${NC}"
else
    echo -e "  ${GREEN}✓ P3 完整收到 P1 的消息${NC}"
fi

# 检查日志中是否有错误
if grep -q "error\|Error\|ERROR\|failed\|Failed" "$TEST1_DIR/proc3.log" 2>/dev/null; then
    echo -e "  ${RED}P3 日志中发现错误:${NC}"
    grep "error\|Error\|ERROR\|failed\|Failed" "$TEST1_DIR/proc3.log" | head -5
fi

echo ""

# 测试 1b: 所有进程同时快速启动（无延迟）
echo "  [1b] 所有进程同时快速启动（模拟最坏情况）..."

TEST1B_DIR="$OUTPUT_DIR/test1b"
mkdir -p "$TEST1B_DIR"
cp "$TEST1_DIR/hosts" "$TEST1B_DIR/hosts"
cp "$TEST1_DIR/config" "$TEST1B_DIR/config"

for i in 1 2 3; do
    "$BIN_DIR/da_proc" --id $i --hosts "$TEST1B_DIR/hosts" --output "$TEST1B_DIR/proc$i.output" "$TEST1B_DIR/config" > "$TEST1B_DIR/proc$i.log" 2>&1 &
done

PIDS=($(jobs -p))

sleep 15

for pid in "${PIDS[@]}"; do
    kill -SIGTERM $pid 2>/dev/null || true
done
sleep 2
for pid in "${PIDS[@]}"; do
    kill -9 $pid 2>/dev/null || true
done

# 统计每个进程的交付情况
echo "  交付统计:"
for i in 1 2 3; do
    TOTAL=$(grep -c "^d " "$TEST1B_DIR/proc$i.output" 2>/dev/null || echo 0)
    echo -n "    P$i: $TOTAL 条总计 | "
    for sender in 1 2 3; do
        COUNT=$(grep -c "^d $sender " "$TEST1B_DIR/proc$i.output" 2>/dev/null || echo 0)
        echo -n "从P$sender: $COUNT 条 "
    done
    echo ""
done

echo ""

# ============================================================================
# 测试 2: 模拟 "Violation of uniform agreement"
# Uniform Agreement 语义：如果一个正确进程交付了消息 m，则所有正确进程最终都必须交付 m
# 可能原因：
# - 进程崩溃前已交付消息，但其他进程未收到足够 ACK 无法交付
# - URB 实现有 bug，某些进程交付了但没有正确转发
# - 消息转发机制不完整
# ============================================================================

echo -e "${YELLOW}[测试 2] 诊断 'Violation of uniform agreement'${NC}"
echo "场景：一个进程提前终止，检查其他进程能否交付它已交付的消息"

TEST2_DIR="$OUTPUT_DIR/test2"
mkdir -p "$TEST2_DIR"

cat > "$TEST2_DIR/hosts" << EOF
1 127.0.0.1 11001
2 127.0.0.1 11002
3 127.0.0.1 11003
4 127.0.0.1 11004
EOF

cat > "$TEST2_DIR/config" << EOF
20
EOF

echo "  启动 4 个进程..."
for i in 1 2 3 4; do
    "$BIN_DIR/da_proc" --id $i --hosts "$TEST2_DIR/hosts" --output "$TEST2_DIR/proc$i.output" "$TEST2_DIR/config" > "$TEST2_DIR/proc$i.log" 2>&1 &
    eval "PID$i=$!"
done

sleep 3

# P1 在广播完成后立即终止（模拟崩溃）
echo "  [3秒后] 强制终止 P1 (模拟崩溃)..."
kill -9 $PID1 2>/dev/null || true

sleep 12

# 终止其他进程
echo "  终止其他进程..."
kill -SIGTERM $PID2 $PID3 $PID4 2>/dev/null || true
sleep 2
kill -9 $PID2 $PID3 $PID4 2>/dev/null || true

# 分析 uniform agreement
echo "  分析 uniform agreement..."

# 找出 P1 交付的消息
P1_DELIVERED=$(grep "^d " "$TEST2_DIR/proc1.output" 2>/dev/null || true)
P1_COUNT=$(echo "$P1_DELIVERED" | grep -c "^d " || echo 0)
echo "  P1 崩溃前交付了 $P1_COUNT 条消息"

if [ "$P1_COUNT" -gt 0 ]; then
    # 检查其他正确进程是否也交付了这些消息
    echo "  检查其他进程是否交付了相同的消息..."
    
    VIOLATIONS=0
    while read -r line; do
        if [ -z "$line" ]; then continue; fi
        
        SENDER=$(echo "$line" | awk '{print $2}')
        SEQ=$(echo "$line" | awk '{print $3}')
        
        # 检查 P2, P3, P4
        for proc in 2 3 4; do
            if ! grep -q "^d $SENDER $SEQ$" "$TEST2_DIR/proc$proc.output" 2>/dev/null; then
                echo -e "    ${RED}✗ P$proc 未交付 {$SENDER,$SEQ} (但 P1 已交付)${NC}"
                VIOLATIONS=$((VIOLATIONS + 1))
            fi
        done
    done <<< "$P1_DELIVERED"
    
    if [ "$VIOLATIONS" -gt 0 ]; then
        echo -e "  ${RED}发现 $VIOLATIONS 处 uniform agreement 违规！${NC}"
        echo "  可能原因："
        echo "    - P1 交付消息后未继续转发或转发丢失"
        echo "    - 其他进程的 ACK 计数不足（minority）"
        echo "    - URB 实现中 forwarding 逻辑有问题"
    else
        echo -e "  ${GREEN}✓ 未发现 uniform agreement 违规${NC}"
    fi
fi

# 额外检查：反向检查 P2/P3/P4 是否都交付了相同的消息
echo ""
echo "  交付一致性检查:"
for sender in 1 2 3 4; do
    echo -n "    来自 P$sender 的消息: "
    C2=$(grep -c "^d $sender " "$TEST2_DIR/proc2.output" 2>/dev/null || echo 0)
    C3=$(grep -c "^d $sender " "$TEST2_DIR/proc3.output" 2>/dev/null || echo 0)
    C4=$(grep -c "^d $sender " "$TEST2_DIR/proc4.output" 2>/dev/null || echo 0)
    echo "P2=$C2, P3=$C3, P4=$C4"
    
    if [ "$C2" -ne "$C3" ] || [ "$C3" -ne "$C4" ]; then
        echo -e "      ${RED}✗ 不一致！可能违反 uniform agreement${NC}"
    fi
done

echo ""

# ============================================================================
# 测试 3: 压力测试 - 多进程高负载
# ============================================================================

echo -e "${YELLOW}[测试 3] 压力测试 - 检测高负载下的问题${NC}"

TEST3_DIR="$OUTPUT_DIR/test3"
mkdir -p "$TEST3_DIR"

N=5
M=50

cat /dev/null > "$TEST3_DIR/hosts"
for i in $(seq 1 $N); do
    echo "$i 127.0.0.1 $((11000 + i))" >> "$TEST3_DIR/hosts"
done

cat > "$TEST3_DIR/config" << EOF
$M
EOF

echo "  启动 $N 个进程，每个广播 $M 条消息..."

for i in $(seq 1 $N); do
    "$BIN_DIR/da_proc" --id $i --hosts "$TEST3_DIR/hosts" --output "$TEST3_DIR/proc$i.output" "$TEST3_DIR/config" > "$TEST3_DIR/proc$i.log" 2>&1 &
done

PIDS=($(jobs -p))

sleep 20

for pid in "${PIDS[@]}"; do
    kill -SIGTERM $pid 2>/dev/null || true
done
sleep 3
for pid in "${PIDS[@]}"; do
    kill -9 $pid 2>/dev/null || true
done

echo "  统计交付情况:"
EXPECTED=$((N * M))
ALL_COMPLETE=true

for i in $(seq 1 $N); do
    TOTAL=$(grep -c "^d " "$TEST3_DIR/proc$i.output" 2>/dev/null || echo 0)
    if [ "$TOTAL" -eq "$EXPECTED" ]; then
        echo -e "    P$i: ${GREEN}$TOTAL/$EXPECTED ✓${NC}"
    else
        echo -e "    P$i: ${RED}$TOTAL/$EXPECTED ✗${NC}"
        ALL_COMPLETE=false
        
        # 详细分析哪些发送者的消息丢失
        echo -n "      详情: "
        for sender in $(seq 1 $N); do
            COUNT=$(grep -c "^d $sender " "$TEST3_DIR/proc$i.output" 2>/dev/null || echo 0)
            if [ "$COUNT" -lt "$M" ]; then
                echo -n "P$sender: $COUNT/$M "
            fi
        done
        echo ""
    fi
done

if [ "$ALL_COMPLETE" = false ]; then
    echo -e "  ${RED}✗ 高负载下出现消息丢失${NC}"
else
    echo -e "  ${GREEN}✓ 高负载下所有消息正确交付${NC}"
fi

echo ""

# ============================================================================
# 测试 4: 网络分区恢复测试
# ============================================================================

echo -e "${YELLOW}[测试 4] 网络分区模拟（SIGSTOP/SIGCONT）${NC}"

TEST4_DIR="$OUTPUT_DIR/test4"
mkdir -p "$TEST4_DIR"

cat > "$TEST4_DIR/hosts" << EOF
1 127.0.0.1 11001
2 127.0.0.1 11002
3 127.0.0.1 11003
EOF

cat > "$TEST4_DIR/config" << EOF
30
EOF

echo "  启动 3 个进程..."
for i in 1 2 3; do
    "$BIN_DIR/da_proc" --id $i --hosts "$TEST4_DIR/hosts" --output "$TEST4_DIR/proc$i.output" "$TEST4_DIR/config" > "$TEST4_DIR/proc$i.log" 2>&1 &
    eval "PID$i=$!"
done

sleep 2

echo "  [2s] 暂停 P2..."
kill -SIGSTOP $PID2

sleep 3

echo "  [5s] 恢复 P2..."
kill -SIGCONT $PID2

sleep 3

echo "  [8s] 暂停 P1 和 P3..."
kill -SIGSTOP $PID1 $PID3

sleep 2

echo "  [10s] 恢复 P1 和 P3..."
kill -SIGCONT $PID1 $PID3

sleep 15

echo "  终止所有进程..."
kill -SIGTERM $PID1 $PID2 $PID3 2>/dev/null || true
sleep 2
kill -9 $PID1 $PID2 $PID3 2>/dev/null || true

echo "  检查最终交付情况:"
for i in 1 2 3; do
    TOTAL=$(grep -c "^d " "$TEST4_DIR/proc$i.output" 2>/dev/null || echo 0)
    echo "    P$i: $TOTAL/90 条"
    
    # 检查 FIFO 顺序
    FIFO_OK=true
    for sender in 1 2 3; do
        POSITIONS=$(grep "^d $sender " "$TEST4_DIR/proc$i.output" 2>/dev/null | awk '{print $3}')
        if [ -n "$POSITIONS" ]; then
            SORTED=$(echo "$POSITIONS" | sort -n)
            if [ "$POSITIONS" != "$SORTED" ]; then
                echo -e "      ${RED}✗ 来自 P$sender 的消息违反 FIFO 顺序${NC}"
                FIFO_OK=false
            fi
        fi
    done
    
    if [ "$FIFO_OK" = true ]; then
        echo -e "      ${GREEN}✓ FIFO 顺序正确${NC}"
    fi
done

echo ""

# ============================================================================
# 总结和建议
# ============================================================================

echo -e "${BLUE}=== 诊断总结 ===${NC}"
echo ""
echo "根据上述测试结果，请检查以下方面："
echo ""
echo "1. 如果测试 1 失败（P3 未收到 P1 消息）："
echo "   - 增加 fifo_broadcast_app.cpp 中的启动延迟 (sleep_for)"
echo "   - 当前 1000ms 可能不足，建议改为 2000-3000ms"
echo "   - 或实现进程间同步机制"
echo ""
echo "2. 如果测试 2 失败（uniform agreement 违规）："
echo "   - 检查 URB 的转发逻辑，确保消息被转发到所有进程"
echo "   - 确保进程崩溃前已交付的消息被其他进程也能交付"
echo "   - 可能需要增加 majority 阈值或改进 ACK 收集机制"
echo ""
echo "3. 如果测试 3/4 失败（高负载或网络分区）："
echo "   - 检查 Perfect Links 的重传机制"
echo "   - 确保 ACK 机制在高负载下不会丢失"
echo "   - 检查消息队列是否会溢出"
echo ""
echo "所有测试输出保存在: $OUTPUT_DIR"
echo ""
