#!/bin/bash
# 文件名: performance_test.sh
# 用途: Perfect Links 并发和性能测试套件
# 测试程序在不同并发和负载下的表现。

set -e

# --- 配置和路径 ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../.."
BIN_DIR="$ROOT_DIR/bin"
OUTPUT_DIR="/tmp/da_perf_test_$$"
LOG_DIR="$OUTPUT_DIR/logs"

# --- 颜色定义 ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# --- 全局变量 ---
# 接收者 ID 始终是最后一个进程
RECEIVER_ID=0
BASE_PORT=11000

# --- 清理函数 ---
cleanup() {
    echo -e "\n${YELLOW}Cleaning up processes and temporary directory $OUTPUT_DIR...${NC}"
    # 尝试使用 SIGTERM 优雅关闭
    kill -SIGTERM $(jobs -p) 2>/dev/null || true
    sleep 1
    # 强制杀死残留进程
    kill -9 $(jobs -p) 2>/dev/null || true
    rm -rf "$OUTPUT_DIR"
    echo -e "${GREEN}Cleanup complete.${NC}"
}
# 注册清理函数，确保测试中断时也能执行
trap cleanup EXIT

# --- 辅助函数：创建 hosts 文件 ---
create_hosts_file() {
    local num_processes=$1
    local hosts_file="$OUTPUT_DIR/hosts"
    
    echo -e "${YELLOW}Generating hosts file for ${num_processes} processes...${NC}"
    rm -f "$hosts_file"
    for i in $(seq 1 $num_processes); do
        local port=$((BASE_PORT + i))
        echo "$i 127.0.0.1 $port" >> "$hosts_file"
    done
    RECEIVER_ID=$num_processes
}

# --- 验证输出函数 ---
verify_output() {
    local test_name=$1
    local num_processes=$2
    local num_messages=$3
    local receiver_output="$OUTPUT_DIR/proc${RECEIVER_ID}.output"
    
    local num_senders=$((num_processes - 1))
    local total_expected=$((num_messages * num_senders))
    local total_delivered=$(grep -c "^d " "$receiver_output" 2>/dev/null || echo 0)
    
    echo -e "${BLUE}  Verifying output for $test_name...${NC}"
    echo "  Expected total deliveries: $total_expected"
    echo "  Actual total deliveries: $total_delivered"
    
    local success=0

    if [ "$total_expected" -ne "$total_delivered" ]; then
        echo -e "${RED}❌ FAIL: Total delivery count mismatch! Expected $total_expected, got $total_delivered.${NC}"
        success=1
    fi
    
    # 检查每个发送者的消息数量和序列完整性
    for sender_id in $(seq 1 $num_senders); do
        local count=$(grep "^d $sender_id " "$receiver_output" | wc -l)
        
        if [ "$count" -ne "$num_messages" ]; then
            echo -e "${RED}❌ FAIL: Missing/Duplicate messages from sender $sender_id! Expected $num_messages, got $count.${NC}"
            success=1
            continue
        fi
        
        # 检查序列完整性和无重复（通过排序和唯一化）
        local unique_count=$(grep "^d $sender_id " "$receiver_output" | sort -n -k3 | uniq -c | awk '{print $1}' | sort -n | tail -1)
        
        if [ "$unique_count" -gt 1 ]; then
            echo -e "${RED}❌ FAIL: Duplicate messages found from sender $sender_id! Max count $unique_count.${NC}"
            success=1
            continue
        fi
    done
    
    if [ $success -eq 0 ]; then
        echo -e "${GREEN}✅ PASS: All deliveries successful and correct!${NC}"
        return 0
    else
        echo -e "${RED}❌ FAIL: Test $test_name failed validation.${NC}"
        return 1
    fi
}

# --- 运行测试函数 ---
run_performance_test() {
    local test_name=$1
    local num_processes=$2       # P: 总进程数 (P-1 个 Sender + 1 Receiver)
    local num_messages=$3        # M: 每个 Sender 发送的消息数
    local wait_time=$4           # 动态等待时间 (秒)
    
    echo -e "\n${BLUE}========================================================================${NC}"
    echo -e "${BLUE}TEST: $test_name | P: $num_processes, M: $num_messages, Total: $((num_processes - 1)) * $num_messages = $(( (num_processes - 1) * num_messages ))${NC}"
    echo -e "${BLUE}========================================================================${NC}"
    
    # --- 1. 设置 ---
    create_hosts_file $num_processes
    
    # 创建配置文件: <num_messages> <receiver_id>
    cat > "$OUTPUT_DIR/config" << EOF
$num_messages $RECEIVER_ID
EOF
    
    # 清理旧的 output 文件
    rm -f "$OUTPUT_DIR"/proc*.output
    
    # --- 2. 启动接收者 ---
    echo -e "${YELLOW}Starting receiver (ID $RECEIVER_ID) on port $((BASE_PORT + RECEIVER_ID))...${NC}"
    "$BIN_DIR/da_proc" --id $RECEIVER_ID --hosts "$OUTPUT_DIR/hosts" \
        --output "$OUTPUT_DIR/proc${RECEIVER_ID}.output" "$OUTPUT_DIR/config" \
        > "$LOG_DIR/${test_name}_receiver.log" 2>&1 &
    
    local receiver_pid=$!
    sleep 1 # 确保接收者先启动并绑定端口
    
    # --- 3. 启动发送者 ---
    local num_senders=$((num_processes - 1))
    echo -e "${YELLOW}Starting ${num_senders} senders (ID 1 to $num_senders)...${NC}"
    for sender_id in $(seq 1 $num_senders); do
        "$BIN_DIR/da_proc" --id $sender_id --hosts "$OUTPUT_DIR/hosts" \
            --output "$OUTPUT_DIR/proc${sender_id}.output" "$OUTPUT_DIR/config" \
            > "$LOG_DIR/${test_name}_sender${sender_id}.log" 2>&1 &
    done
    
    # --- 4. 等待传输完成 ---
    echo -e "${YELLOW}Waiting ${wait_time} seconds for message transmission...${NC}"
    sleep "$wait_time"
    
    # --- 5. 终止进程 ---
    echo -e "${YELLOW}Sending SIGTERM to all processes...${NC}"
    pkill -TERM -f "da_proc"
    sleep 2
    
    # --- 6. 验证结果 ---
    if [ ! -f "$OUTPUT_DIR/proc${RECEIVER_ID}.output" ]; then
        echo -e "${RED}❌ FAIL: Receiver output file not found!${NC}"
        RESULT=1
    else
        verify_output "$test_name" "$num_processes" "$num_messages"
        RESULT=$?
    fi
    
    # 记录结果
    if [ $RESULT -eq 0 ]; then
        echo -e "${GREEN}PASS: Test $test_name passed.${NC}" | tee -a "$LOG_DIR/summary.txt"
    else
        echo -e "${RED}FAIL: Test $test_name failed.${NC}" | tee -a "$LOG_DIR/summary.txt"
    fi
    
    return $RESULT
}

# --- 主测试流程 ---
main() {
    echo -e "${BLUE}╔══════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║  Perfect Links Concurrency & Performance Test  ║${NC}"
    echo -e "${BLUE}╚══════════════════════════════════════════════════╝${NC}"
    
    # --- 初始化 ---
    mkdir -p "$OUTPUT_DIR"
    mkdir -p "$LOG_DIR"
    rm -f "$LOG_DIR/summary.txt"
    
    # 检查编译
    echo -e "${BLUE}Checking build status...${NC}"
    if [ ! -f "$BIN_DIR/da_proc" ]; then
        echo -e "${YELLOW}Building project...${NC}"
        cd "$ROOT_DIR"
        ./build.sh > /dev/null 2>&1 || { echo -e "${RED}Build failed${NC}"; exit 1; }
        cd "$SCRIPT_DIR" # 回到脚本目录
    else
        echo -e "${GREEN}Project already built.${NC}"
    fi

    local overall_status=0

    # --- 难度等级 1: 低并发，中等负载 ---
    # 目标：测试基本重传和 ACK 机制在多进程下的稳定性。
    # run_performance_test "Level1_LowConcurrency" 5 100 5
    # level1_result=$?
    # ((overall_status |= level1_result))

    # --- 难度等级 2: 中等并发，高负载 ---
    # 目标：测试线程池/线程处理大量并发消息的能力，以及 ACK/DATA 冲突解决。
    # run_performance_test "Level2_MediumLoad" 10 500 10
    # level2_result=$?
    # ((overall_status |= level2_result))

    # --- 难度等级 3: 高并发，压力测试 ---
    # # 目标：测试在大规模并发和数据量下的鲁棒性，以及 Socket 缓冲区和线程同步极限。
    run_performance_test "Level3_StressTest" 20 1000 20
    level3_result=$?
    ((overall_status |= level3_result))
    
    # --- 汇总结果 ---
    echo -e "\n${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║            性能测试汇总                 ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    
    [ $level1_result -eq 0 ] && echo -e "Level 1 (Low Concurrency):   ${GREEN}✅ PASS${NC}" || echo -e "Level 1 (Low Concurrency):   ${RED}❌ FAIL${NC}"
    [ $level2_result -eq 0 ] && echo -e "Level 2 (Medium Load):       ${GREEN}✅ PASS${NC}" || echo -e "Level 2 (Medium Load):       ${RED}❌ FAIL${NC}"
    [ $level3_result -eq 0 ] && echo -e "Level 3 (Stress Test):       ${GREEN}✅ PASS${NC}" || echo -e "Level 3 (Stress Test):       ${RED}❌ FAIL${NC}"
    
    if [ $overall_status -eq 0 ]; then
        echo -e "\n${GREEN}🎉 所有性能测试通过! Perfect Link 实现非常健壮。${NC}"
    else
        echo -e "\n${RED}⚠️ 发现性能问题! 请检查 $LOG_DIR/summary.txt 中的详细失败日志。${NC}"
    fi
    
    return $overall_status
}

# 运行主程序
main "$@"