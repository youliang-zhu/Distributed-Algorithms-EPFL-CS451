#!/bin/bash
# 文件名: diagnostic_test.sh
# 用途: 诊断 Perfect Links 的问题

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置参数
NUM_PROCESSES=4
RECEIVER_ID=4
HOSTS_FILE="files/hosts"
CONFIG_FILE="files/configs/perfect-links.config"
OUTPUT_DIR="files/output"
LOG_DIR="test_logs"

# 创建必要的目录
mkdir -p "$OUTPUT_DIR"
mkdir -p "$LOG_DIR"

# 清理函数
cleanup() {
    echo -e "${YELLOW}Cleaning up processes...${NC}"
    pkill -f "da_proc" 2>/dev/null || true
    sleep 1
}

# 验证输出函数
verify_output() {
    local test_name=$1
    local num_messages=$2
    local num_senders=$((NUM_PROCESSES - 1))
    
    echo -e "${BLUE}Verifying output for $test_name...${NC}"
    
    # 检查接收者的输出
    local receiver_output="$OUTPUT_DIR/${RECEIVER_ID}.output"
    
    if [ ! -f "$receiver_output" ]; then
        echo -e "${RED}❌ Receiver output file not found!${NC}"
        return 1
    fi
    
    local total_expected=$((num_messages * num_senders))
    local total_delivered=$(wc -l < "$receiver_output")
    
    echo "  Expected deliveries: $total_expected"
    echo "  Actual deliveries: $total_delivered"
    
    # 检查每个发送者的消息
    for sender_id in $(seq 1 $((NUM_PROCESSES - 1))); do
        local count=$(grep "^d $sender_id " "$receiver_output" | wc -l)
        echo "  Messages from sender $sender_id: $count / $num_messages"
        
        if [ "$count" -ne "$num_messages" ]; then
            echo -e "${RED}❌ Missing messages from sender $sender_id!${NC}"
            
            # 显示缺失的消息
            echo "  Missing sequence numbers:"
            for seq in $(seq 1 "$num_messages"); do
                if ! grep -q "^d $sender_id $seq$" "$receiver_output"; then
                    echo -n "$seq "
                fi
            done
            echo ""
            return 1
        fi
    done
    
    if [ "$total_delivered" -eq "$total_expected" ]; then
        echo -e "${GREEN}✅ All messages delivered correctly!${NC}"
        return 0
    else
        echo -e "${RED}❌ Delivery count mismatch!${NC}"
        return 1
    fi
}

# 运行测试函数
run_test() {
    local test_name=$1
    local num_messages=$2
    local delay_before_send=$3
    local use_stdout=$4  # "yes" 或 "no"
    
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}TEST: $test_name${NC}"
    echo -e "${BLUE}Messages: $num_messages, Delay: ${delay_before_send}ms, StdOut: $use_stdout${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    cleanup
    rm -f "$OUTPUT_DIR"/*.output
    
    # 创建配置文件
    echo "$num_messages $RECEIVER_ID" > "$CONFIG_FILE"
    
    # 启动接收者
    echo -e "${YELLOW}Starting receiver (process $RECEIVER_ID)...${NC}"
    ./bin/da_proc --id $RECEIVER_ID --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/${RECEIVER_ID}.output" "$CONFIG_FILE" \
        > "$LOG_DIR/${test_name}_receiver.log" 2>&1 &
    local receiver_pid=$!
    
    # 等待接收者启动
    sleep 1
    
    # 启动发送者
    for sender_id in $(seq 1 $((NUM_PROCESSES - 1))); do
        echo -e "${YELLOW}Starting sender (process $sender_id)...${NC}"
        ./bin/da_proc --id $sender_id --hosts "$HOSTS_FILE" \
            --output "$OUTPUT_DIR/${sender_id}.output" "$CONFIG_FILE" \
            > "$LOG_DIR/${test_name}_sender${sender_id}.log" 2>&1 &
    done
    
    # 等待消息传输完成
    local wait_time=$((num_messages / 100 + 5))  # 根据消息数量动态调整
    echo -e "${YELLOW}Waiting ${wait_time} seconds for message transmission...${NC}"
    sleep "$wait_time"
    
    # 发送 SIGTERM 终止所有进程
    echo -e "${YELLOW}Sending SIGTERM to all processes...${NC}"
    pkill -TERM -f "da_proc"
    
    # 等待进程写入日志
    sleep 2
    
    # 验证结果
    if verify_output "$test_name" "$num_messages"; then
        echo -e "${GREEN}✅ TEST PASSED: $test_name${NC}" | tee -a "$LOG_DIR/summary.txt"
        return 0
    else
        echo -e "${RED}❌ TEST FAILED: $test_name${NC}" | tee -a "$LOG_DIR/summary.txt"
        
        # 保存失败的日志
        cp "$OUTPUT_DIR/${RECEIVER_ID}.output" "$LOG_DIR/${test_name}_receiver_output.txt" 2>/dev/null || true
        
        return 1
    fi
}

# 检查编译
check_build() {
    echo -e "${BLUE}Checking if project is built...${NC}"
    if [ ! -f "bin/da_proc" ]; then
        echo -e "${YELLOW}Building project...${NC}"
        ./build.sh
    else
        echo -e "${GREEN}Project already built.${NC}"
    fi
}

# 主测试流程
main() {
    echo -e "${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║  Perfect Links Diagnostic Test Suite  ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    
    check_build
    
    # 清空之前的测试日志
    rm -f "$LOG_DIR/summary.txt"
    
    # 测试1: 基线测试 - 小规模无延迟
    echo -e "\n${YELLOW}═══ Test 1: Baseline (20 messages, no delay) ═══${NC}"
    run_test "test1_baseline" 20 0 "no"
    test1_result=$?
    
    # 测试2: 中等规模无延迟
    echo -e "\n${YELLOW}═══ Test 2: Medium scale (100 messages, no delay) ═══${NC}"
    run_test "test2_medium" 100 0 "no"
    test2_result=$?
    
    # 测试3: 大规模无延迟（模拟老师的测试）
    echo -e "\n${YELLOW}═══ Test 3: Large scale (1000 messages, no delay) ═══${NC}"
    run_test "test3_large" 1000 0 "no"
    test3_result=$?
    
    # 测试4: 基线测试但有启动延迟
    echo -e "\n${YELLOW}═══ Test 4: With startup delay (20 messages, 2s delay) ═══${NC}"
    # 手动测试需要修改代码添加延迟
    echo -e "${YELLOW}Note: This test requires code modification to add 2s delay in run()${NC}"
    echo -e "${YELLOW}Skipping for now. Please run manually after adding delay.${NC}"
    test4_result=0
    
    # 测试5: 压力测试 - 检查UDP缓冲区溢出
    echo -e "\n${YELLOW}═══ Test 5: Stress test (5000 messages) ═══${NC}"
    run_test "test5_stress" 5000 0 "no"
    test5_result=$?
    
    # 汇总结果
    echo -e "\n${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║           Test Summary                 ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    
    [ $test1_result -eq 0 ] && echo -e "Test 1 (Baseline):       ${GREEN}✅ PASS${NC}" || echo -e "Test 1 (Baseline):       ${RED}❌ FAIL${NC}"
    [ $test2_result -eq 0 ] && echo -e "Test 2 (Medium):         ${GREEN}✅ PASS${NC}" || echo -e "Test 2 (Medium):         ${RED}❌ FAIL${NC}"
    [ $test3_result -eq 0 ] && echo -e "Test 3 (Large):          ${GREEN}✅ PASS${NC}" || echo -e "Test 3 (Large):          ${RED}❌ FAIL${NC}"
    echo -e "Test 4 (With delay):     ${YELLOW}⊘ SKIPPED${NC}"
    [ $test5_result -eq 0 ] && echo -e "Test 5 (Stress):         ${GREEN}✅ PASS${NC}" || echo -e "Test 5 (Stress):         ${RED}❌ FAIL${NC}"
    
    echo -e "\n${BLUE}Detailed logs saved to: $LOG_DIR/${NC}"
    
    # 诊断建议
    echo -e "\n${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║         Diagnostic Analysis            ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    
    if [ $test1_result -eq 0 ] && [ $test2_result -ne 0 ]; then
        echo -e "${YELLOW}📊 Pattern: Small tests pass, medium tests fail${NC}"
        echo -e "${YELLOW}→ Likely cause: Logger FLUSH_THRESHOLD too small (issue #2)${NC}"
        echo -e "${YELLOW}→ Solution: Increase FLUSH_THRESHOLD from 5 to 1000${NC}"
    fi
    
    if [ $test1_result -ne 0 ]; then
        echo -e "${YELLOW}📊 Pattern: Even small tests fail${NC}"
        echo -e "${YELLOW}→ Likely cause: Race condition in startup (issue #1)${NC}"
        echo -e "${YELLOW}→ Solution: Add 1000ms delay before sender starts${NC}"
    fi
    
    if [ $test1_result -eq 0 ] && [ $test2_result -eq 0 ] && [ $test3_result -ne 0 ]; then
        echo -e "${YELLOW}📊 Pattern: Small/medium pass, large fails${NC}"
        echo -e "${YELLOW}→ Likely cause: UDP buffer overflow (issue #4)${NC}"
        echo -e "${YELLOW}→ Solution: Increase UDP receive buffer size to 8MB${NC}"
    fi
    
    if [ $test5_result -ne 0 ]; then
        echo -e "${YELLOW}📊 Pattern: Stress test fails${NC}"
        echo -e "${YELLOW}→ Likely cause: Memory or performance issues${NC}"
        echo -e "${YELLOW}→ Solution: Optimize data structures, check memory usage${NC}"
    fi
    
    cleanup
}

# 运行主程序
main "$@"