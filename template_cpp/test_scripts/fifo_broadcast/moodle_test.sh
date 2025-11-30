#!/bin/bash

set -e

# --- 配置 ---
ROOT_DIR="../.."  # 根据你的实际目录结构调整
BIN_DIR="$ROOT_DIR/bin"
OUTPUT_DIR="/tmp/da_debug_grading_$$"

# --- 1. 编译 ---
echo "Building project..."
cd "$ROOT_DIR"
./build.sh > /dev/null

# --- 2. 准备环境 ---
mkdir -p "$OUTPUT_DIR"
# 注意：我不删除目录，方便你事后检查日志
echo "Debug output directory: $OUTPUT_DIR"

# 生成 hosts (模拟 Test 0 通常是小规模)
cat > "$OUTPUT_DIR/hosts" << EOF
1 127.0.0.1 11001
2 127.0.0.1 11002
EOF

# 生成 config (发送 10 条消息)
echo "10" > "$OUTPUT_DIR/config"

# --- 3. 启动进程 ---
echo "Starting Process 1 and 2..."

# 启动 P1 (记录 stdout 到 proc1.log)
"$BIN_DIR/da_proc" --id 1 --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc1.output" "$OUTPUT_DIR/config" > "$OUTPUT_DIR/proc1.log" 2>&1 &
PID1=$!

# 启动 P2 (记录 stdout 到 proc2.log)
"$BIN_DIR/da_proc" --id 2 --hosts "$OUTPUT_DIR/hosts" --output "$OUTPUT_DIR/proc2.output" "$OUTPUT_DIR/config" > "$OUTPUT_DIR/proc2.log" 2>&1 &
PID2=$!

echo "Processes started. P1=$PID1, P2=$PID2"

# --- 4. 等待 ---
# 给足时间，排除时间不够的因素
sleep 8

# --- 5. 终止 ---
echo "Stopping processes..."
kill -SIGTERM $PID1 $PID2 2>/dev/null || true
sleep 2 # 等待 flush
kill -9 $PID1 $PID2 2>/dev/null || true

# --- 6. 自动化诊断分析 ---
echo "================ ANALYSIS ================"

# 检查 P1 是否收到了 P2 的消息
DELIVERED_FROM_2=$(grep -c "^d 2 " "$OUTPUT_DIR/proc1.output" 2>/dev/null || echo 0)
echo "P1 delivered from P2: $DELIVERED_FROM_2 / 10"

if [ "$DELIVERED_FROM_2" -eq 0 ]; then
    echo "❌ 故障复现！P1 没有交付 P2 的消息。"
    
    echo "--- 检查 P2 是否尝试发送 (Check P2 Log) ---"
    # 查找 P2 是否向 P1 的端口 (11001) 发送过数据
    grep "SENT.*to.*11001" "$OUTPUT_DIR/proc2.log" | head -n 5
    if [ $? -ne 0 ]; then
        echo "  -> P2 从未向 P1 (11001) 发送过数据。P2 可能启动失败或卡住。"
    else
        echo "  -> P2 确实尝试发送了。"
    fi

    echo "--- 检查 P1 是否收到物理包 (Check P1 Log) ---"
    # 查找 P1 是否从 P2 的端口 (11002) 收到过数据
    grep "RECV.*from.*11002" "$OUTPUT_DIR/proc1.log" | head -n 5
    if [ $? -ne 0 ]; then
        echo "  -> P1 物理层从未收到来自 P2 (11002) 的包。网络丢包或绑定端口错误。"
    else
        echo "  -> P1 物理层收到了包，但在应用层被丢弃了。"
        echo "--- 检查 P1 应用层处理逻辑 ---"
        grep "processing DATA" "$OUTPUT_DIR/proc1.log" | grep "PacketSenderID=2" | head -n 5
    fi

else
    echo "✅ 在本地无法复现故障，P1 成功收到了 P2 的消息。"
    echo "这说明可能是评分环境的机器非常慢，你需要增加启动延迟。"
fi

echo "日志文件位置: $OUTPUT_DIR"