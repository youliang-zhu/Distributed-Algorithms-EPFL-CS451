#!/bin/bash
# 优化测试脚本 - 专注于大消息量和 SIGTERM 后的处理

# 注意：不使用 set -e，因为我们需要测试即使失败也继续运行
# set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
MAGENTA='\033[0;35m'
BLUE='\033[0;34m'
NC='\033[0m'

HOSTS_FILE="files/hosts"
CONFIG_FILE="files/configs/perfect-links.config"
OUTPUT_DIR="files/output"
LOG_DIR="optimized_test_logs"

mkdir -p "$OUTPUT_DIR"
mkdir -p "$LOG_DIR"

# 强制清理所有进程
force_cleanup() {
    pkill -9 -f "da_proc" 2>/dev/null || true
    sleep 1
}

# 打印分隔线
print_separator() {
    echo -e "${BLUE}═══════════════════════════════════════════════════════════${NC}"
}

# 启动日志监控
start_log_monitoring() {
    local test_name=$1
    echo -e "\n${BLUE}════ Live Process Output ════${NC}"
    
    # 创建空日志文件
    touch "$LOG_DIR/${test_name}_receiver.log"
    touch "$LOG_DIR/${test_name}_sender1.log"
    touch "$LOG_DIR/${test_name}_sender2.log"
    
    # 启动 tail 进程来监控日志
    tail -f "$LOG_DIR/${test_name}_receiver.log" 2>/dev/null | sed "s/^/  [R] /" &
    TAIL_R_PID=$!
    
    tail -f "$LOG_DIR/${test_name}_sender1.log" 2>/dev/null | sed "s/^/  [S1] /" &
    TAIL_S1_PID=$!
    
    tail -f "$LOG_DIR/${test_name}_sender2.log" 2>/dev/null | sed "s/^/  [S2] /" &
    TAIL_S2_PID=$!
    
    sleep 0.5
}

# 停止日志监控
stop_log_monitoring() {
    echo -e "${BLUE}════ End Live Output ════${NC}\n"
    # 使用 || true 确保即使进程不存在也不会报错
    kill $TAIL_R_PID 2>/dev/null || true
    kill $TAIL_S1_PID 2>/dev/null || true
    kill $TAIL_S2_PID 2>/dev/null || true
    sleep 0.5
}

# 测试1: 大消息量测试（50条）
test_large_messages_50() {
    local num_messages=50
    local wait_time=4
    
    echo -e "\n${MAGENTA}╔════════════════════════════════════════════════════════╗${NC}"
    echo -e "${MAGENTA}║  TEST 1: Large Messages (50 messages)                 ║${NC}"
    echo -e "${MAGENTA}╚════════════════════════════════════════════════════════╝${NC}"
    
    force_cleanup
    rm -f "$OUTPUT_DIR"/*.output
    
    echo "$num_messages 3" > "$CONFIG_FILE"
    
    # 启动日志监控
    start_log_monitoring "test1"
    
    echo -e "${CYAN}Starting receiver (process 3)...${NC}"
    ./bin/da_proc --id 3 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/3.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test1_receiver.log" 2>&1 &
    local receiver_pid=$!
    
    sleep 1
    
    echo -e "${CYAN}Starting sender 1 (process 1)...${NC}"
    ./bin/da_proc --id 1 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/1.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test1_sender1.log" 2>&1 &
    local sender1_pid=$!
    
    echo -e "${CYAN}Starting sender 2 (process 2)...${NC}"
    ./bin/da_proc --id 2 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/2.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test1_sender2.log" 2>&1 &
    local sender2_pid=$!
    
    echo -e "${YELLOW}Waiting ${wait_time} seconds for message exchange...${NC}"
    sleep "$wait_time"
    
    # 检查进程状态
    echo -e "${CYAN}Process status before SIGTERM:${NC}"
    ps -p $receiver_pid > /dev/null 2>&1 && echo "  Receiver: alive" || echo "  Receiver: dead"
    ps -p $sender1_pid > /dev/null 2>&1 && echo "  Sender 1: alive" || echo "  Sender 1: dead"
    ps -p $sender2_pid > /dev/null 2>&1 && echo "  Sender 2: alive" || echo "  Sender 2: dead"
    
    # 检查是否有完成消息
    echo -e "${CYAN}Checking for completion messages:${NC}"
    grep -q "Sent $num_messages messages" "$LOG_DIR/test1_sender1.log" 2>/dev/null && \
        echo -e "  ${GREEN}✓ Sender 1 completed${NC}" || echo -e "  ${RED}✗ Sender 1 not completed${NC}"
    grep -q "Sent $num_messages messages" "$LOG_DIR/test1_sender2.log" 2>/dev/null && \
        echo -e "  ${GREEN}✓ Sender 2 completed${NC}" || echo -e "  ${RED}✗ Sender 2 not completed${NC}"
    
    echo -e "\n${YELLOW}Sending SIGTERM to all processes...${NC}"
    kill -TERM $receiver_pid $sender1_pid $sender2_pid 2>/dev/null || true
    
    echo -e "${YELLOW}Waiting 2 seconds for graceful shutdown...${NC}"
    sleep 2
    
    # 停止日志监控
    stop_log_monitoring
    
    # 检查进程是否退出
    echo -e "${CYAN}Process status after SIGTERM:${NC}"
    if ps -p $receiver_pid > /dev/null 2>&1; then
        echo -e "  ${RED}Receiver still running (killing forcefully)${NC}"
        kill -9 $receiver_pid 2>/dev/null || true
    else
        echo -e "  ${GREEN}Receiver terminated${NC}"
    fi
    
    if ps -p $sender1_pid > /dev/null 2>&1; then
        echo -e "  ${RED}Sender 1 still running (killing forcefully)${NC}"
        kill -9 $sender1_pid 2>/dev/null || true
    else
        echo -e "  ${GREEN}Sender 1 terminated${NC}"
    fi
    
    if ps -p $sender2_pid > /dev/null 2>&1; then
        echo -e "  ${RED}Sender 2 still running (killing forcefully)${NC}"
        kill -9 $sender2_pid 2>/dev/null || true
    else
        echo -e "  ${GREEN}Sender 2 terminated${NC}"
    fi
    
    # 分析结果
    echo -e "\n${CYAN}Results Analysis:${NC}"
    analyze_results 3 1 2 "$num_messages"
}

# 测试2: 大消息量测试（100条）
test_large_messages_100() {
    local num_messages=100
    local wait_time=5
    
    echo -e "\n${MAGENTA}╔════════════════════════════════════════════════════════╗${NC}"
    echo -e "${MAGENTA}║  TEST 2: Large Messages (100 messages)                ║${NC}"
    echo -e "${MAGENTA}╚════════════════════════════════════════════════════════╝${NC}"
    
    force_cleanup
    rm -f "$OUTPUT_DIR"/*.output
    
    echo "$num_messages 3" > "$CONFIG_FILE"
    
    # 启动日志监控
    start_log_monitoring "test2"
    
    echo -e "${CYAN}Starting receiver (process 3)...${NC}"
    ./bin/da_proc --id 3 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/3.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test2_receiver.log" 2>&1 &
    local receiver_pid=$!
    
    sleep 1
    
    echo -e "${CYAN}Starting sender 1 (process 1)...${NC}"
    ./bin/da_proc --id 1 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/1.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test2_sender1.log" 2>&1 &
    local sender1_pid=$!
    
    echo -e "${CYAN}Starting sender 2 (process 2)...${NC}"
    ./bin/da_proc --id 2 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/2.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test2_sender2.log" 2>&1 &
    local sender2_pid=$!
    
    echo -e "${YELLOW}Waiting ${wait_time} seconds for message exchange...${NC}"
    sleep "$wait_time"
    
    # 检查进程状态
    echo -e "${CYAN}Process status before SIGTERM:${NC}"
    ps -p $receiver_pid > /dev/null 2>&1 && echo "  Receiver: alive" || echo "  Receiver: dead"
    ps -p $sender1_pid > /dev/null 2>&1 && echo "  Sender 1: alive" || echo "  Sender 1: dead"
    ps -p $sender2_pid > /dev/null 2>&1 && echo "  Sender 2: alive" || echo "  Sender 2: dead"
    
    # 检查是否有完成消息
    echo -e "${CYAN}Checking for completion messages:${NC}"
    grep -q "Sent $num_messages messages" "$LOG_DIR/test2_sender1.log" 2>/dev/null && \
        echo -e "  ${GREEN}✓ Sender 1 completed${NC}" || echo -e "  ${RED}✗ Sender 1 not completed${NC}"
    grep -q "Sent $num_messages messages" "$LOG_DIR/test2_sender2.log" 2>/dev/null && \
        echo -e "  ${GREEN}✓ Sender 2 completed${NC}" || echo -e "  ${RED}✗ Sender 2 not completed${NC}"
    
    echo -e "\n${YELLOW}Sending SIGTERM to all processes...${NC}"
    kill -TERM $receiver_pid $sender1_pid $sender2_pid 2>/dev/null || true
    
    echo -e "${YELLOW}Waiting 2 seconds for graceful shutdown...${NC}"
    sleep 2
    
    # 停止日志监控
    stop_log_monitoring
    
    # 检查进程是否退出
    echo -e "${CYAN}Process status after SIGTERM:${NC}"
    if ps -p $receiver_pid > /dev/null 2>&1; then
        echo -e "  ${RED}Receiver still running (killing forcefully)${NC}"
        kill -9 $receiver_pid 2>/dev/null || true
    else
        echo -e "  ${GREEN}Receiver terminated${NC}"
    fi
    
    if ps -p $sender1_pid > /dev/null 2>&1; then
        echo -e "  ${RED}Sender 1 still running (killing forcefully)${NC}"
        kill -9 $sender1_pid 2>/dev/null || true
    else
        echo -e "  ${GREEN}Sender 1 terminated${NC}"
    fi
    
    if ps -p $sender2_pid > /dev/null 2>&1; then
        echo -e "  ${RED}Sender 2 still running (killing forcefully)${NC}"
        kill -9 $sender2_pid 2>/dev/null || true
    else
        echo -e "  ${GREEN}Sender 2 terminated${NC}"
    fi
    
    # 分析结果
    echo -e "\n${CYAN}Results Analysis:${NC}"
    analyze_results 3 1 2 "$num_messages"
}

# 测试3: SIGTERM 后 receiveLoop 处理测试
test_sigterm_processing() {
    local num_messages=80
    local wait_time=3  # 故意减少等待时间，让SIGTERM在传输过程中到来
    
    echo -e "\n${MAGENTA}╔════════════════════════════════════════════════════════╗${NC}"
    echo -e "${MAGENTA}║  TEST 3: SIGTERM During Transmission (80 messages)    ║${NC}"
    echo -e "${MAGENTA}║  Purpose: Test if receiveLoop processes remaining     ║${NC}"
    echo -e "${MAGENTA}║           packets after SIGTERM                        ║${NC}"
    echo -e "${MAGENTA}╚════════════════════════════════════════════════════════╝${NC}"
    
    force_cleanup
    rm -f "$OUTPUT_DIR"/*.output
    
    echo "$num_messages 3" > "$CONFIG_FILE"
    
    # 启动日志监控
    start_log_monitoring "test3"
    
    echo -e "${CYAN}Starting receiver (process 3)...${NC}"
    ./bin/da_proc --id 3 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/3.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test3_receiver.log" 2>&1 &
    local receiver_pid=$!
    
    sleep 1
    
    echo -e "${CYAN}Starting sender 1 (process 1)...${NC}"
    ./bin/da_proc --id 1 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/1.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test3_sender1.log" 2>&1 &
    local sender1_pid=$!
    
    echo -e "${CYAN}Starting sender 2 (process 2)...${NC}"
    ./bin/da_proc --id 2 --hosts "$HOSTS_FILE" \
        --output "$OUTPUT_DIR/2.output" "$CONFIG_FILE" \
        > "$LOG_DIR/test3_sender2.log" 2>&1 &
    local sender2_pid=$!
    
    echo -e "${YELLOW}Waiting ${wait_time} seconds (intentionally short)...${NC}"
    sleep "$wait_time"
    
    # 检查完成状态
    echo -e "${CYAN}Checking completion status before SIGTERM:${NC}"
    local sender1_done=$(grep -c "Sent $num_messages messages" "$LOG_DIR/test3_sender1.log" 2>/dev/null || echo "0")
    local sender2_done=$(grep -c "Sent $num_messages messages" "$LOG_DIR/test3_sender2.log" 2>/dev/null || echo "0")
    
    if [ "$sender1_done" = "1" ] && [ "$sender2_done" = "1" ]; then
        echo -e "  ${GREEN}Both senders completed before SIGTERM${NC}"
    else
        echo -e "  ${YELLOW}Senders still transmitting (good for this test)${NC}"
    fi
    
    # 记录SIGTERM前的接收量
    local deliveries_before=0
    if [ -f "$OUTPUT_DIR/3.output" ]; then
        deliveries_before=$(grep "^d " "$OUTPUT_DIR/3.output" | wc -l)
    fi
    echo -e "  Deliveries before SIGTERM: ${CYAN}$deliveries_before${NC}"
    
    echo -e "\n${YELLOW}Sending SIGTERM NOW...${NC}"
    local sigterm_time=$(date +%s.%N)
    kill -TERM $receiver_pid $sender1_pid $sender2_pid 2>/dev/null || true
    
    echo -e "${YELLOW}Waiting 2 seconds for shutdown processing...${NC}"
    sleep 2
    
    # 记录SIGTERM后的接收量
    local deliveries_after=0
    if [ -f "$OUTPUT_DIR/3.output" ]; then
        deliveries_after=$(grep "^d " "$OUTPUT_DIR/3.output" | wc -l)
    fi
    echo -e "  Deliveries after SIGTERM: ${CYAN}$deliveries_after${NC}"
    
    local processed_during_shutdown=$((deliveries_after - deliveries_before))
    echo -e "\n${MAGENTA}Messages processed DURING shutdown: ${CYAN}$processed_during_shutdown${NC}"
    
    if [ $processed_during_shutdown -gt 0 ]; then
        echo -e "${GREEN}✓ receiveLoop IS processing packets after SIGTERM${NC}"
    else
        echo -e "${YELLOW}⚠ receiveLoop processed 0 packets after SIGTERM${NC}"
        echo -e "${YELLOW}  (This might be OK if all messages were already delivered)${NC}"
    fi
    
    # 停止日志监控
    stop_log_monitoring
    
    # 强制清理
    kill -9 $receiver_pid $sender1_pid $sender2_pid 2>/dev/null || true
    
    # 分析结果
    echo -e "\n${CYAN}Final Results Analysis:${NC}"
    analyze_results 3 1 2 "$num_messages"
}

# 分析结果的通用函数
analyze_results() {
    local receiver_id=$1
    local sender1_id=$2
    local sender2_id=$3
    local expected=$4
    
    local receiver_output="$OUTPUT_DIR/${receiver_id}.output"
    
    if [ ! -f "$receiver_output" ]; then
        echo -e "${RED}✗ Receiver output file not found!${NC}"
        return 1
    fi
    
    # 统计总接收量
    local total_delivered=$(grep "^d " "$receiver_output" | wc -l)
    local total_expected=$((expected * 2))
    local percentage=$((total_delivered * 100 / total_expected))
    
    echo "  Total deliveries: $total_delivered / $total_expected (${percentage}%)"
    
    # 分析每个发送者
    for sender_id in $sender1_id $sender2_id; do
        local delivered=$(grep "^d $sender_id " "$receiver_output" | awk '{print $3}' | sort -n)
        local count=$(echo "$delivered" | wc -w)
        local sender_percentage=$((count * 100 / expected))
        
        echo ""
        if [ $count -eq $expected ]; then
            echo -e "  ${GREEN}✓ Sender $sender_id: $count / $expected (100%)${NC}"
        else
            echo -e "  ${YELLOW}⚠ Sender $sender_id: $count / $expected (${sender_percentage}%)${NC}"
            
            # 显示范围
            local first=$(echo "$delivered" | head -1)
            local last=$(echo "$delivered" | tail -1)
            echo "    Range: $first - $last"
            
            # 显示缺失的序号（前20个）
            echo -n "    Missing (first 20): "
            local missing_count=0
            for seq in $(seq 1 $expected); do
                if ! echo "$delivered" | grep -qw "$seq"; then
                    echo -n "$seq "
                    missing_count=$((missing_count + 1))
                    if [ $missing_count -ge 20 ]; then
                        echo "..."
                        break
                    fi
                fi
            done
            [ $missing_count -eq 0 ] && echo "(none)"
            [ $missing_count -gt 0 ] && [ $missing_count -lt 20 ] && echo ""
            
            # 判断缺失模式
            local first_missing=$(seq 1 $expected | while read seq; do
                if ! echo "$delivered" | grep -qw "$seq"; then
                    echo $seq
                    break
                fi
            done)
            
            if [ -n "$first_missing" ]; then
                if [ $first_missing -gt $((expected * 3 / 4)) ]; then
                    echo -e "    ${RED}Pattern: Missing at END → receiveLoop stopped too early${NC}"
                elif [ $first_missing -lt $((expected / 4)) ]; then
                    echo -e "    ${YELLOW}Pattern: Missing at BEGINNING → startup issue${NC}"
                else
                    echo -e "    ${YELLOW}Pattern: Missing in MIDDLE → ACK/retransmission issue${NC}"
                fi
            fi
        fi
    done
    
    echo ""
    if [ $total_delivered -eq $total_expected ]; then
        echo -e "${GREEN}═══ TEST PASSED: All messages delivered ═══${NC}"
        return 0
    else
        echo -e "${RED}═══ TEST FAILED: Missing $((total_expected - total_delivered)) messages ═══${NC}"
        return 1
    fi
}

# 主函数
main() {
    echo -e "${MAGENTA}"
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║                                                            ║"
    echo "║     Optimized Perfect Link Test Suite                     ║"
    echo "║     Focus: Large Messages & SIGTERM Processing            ║"
    echo "║                                                            ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"
    
    # 预清理
    echo -e "${CYAN}Pre-test cleanup...${NC}"
    force_cleanup
    
    # 检查编译
    if [ ! -f "bin/da_proc" ]; then
        echo -e "${YELLOW}Building project...${NC}"
        if ! ./build.sh; then
            echo -e "${RED}Build failed!${NC}"
            exit 1
        fi
    fi
    
    local tests_passed=0
    local tests_total=3
    
    运行测试 - 使用 || true 确保即使测试失败也继续运行
    echo -e "\n${CYAN}Running Test 1...${NC}"
    if test_large_messages_50; then
        ((tests_passed++)) || true
    fi
    
    echo -e "\n${CYAN}Running Test 2...${NC}"
    if test_large_messages_100; then
        ((tests_passed++)) || true
    fi
    
    echo -e "\n${CYAN}Running Test 3...${NC}"
    if test_sigterm_processing; then
        ((tests_passed++)) || true
    fi
    
    # 最终清理
    echo -e "\n${CYAN}Final cleanup...${NC}"
    force_cleanup
    
    # 总结
    print_separator
    echo -e "${MAGENTA}Test Summary:${NC}"
    echo -e "  Tests passed: ${GREEN}$tests_passed${NC} / $tests_total"
    
    if [ $tests_passed -eq $tests_total ]; then
        echo -e "\n${GREEN}✓ All tests passed!${NC}"
    else
        echo -e "\n${RED}✗ Some tests failed${NC}"
        echo -e "${YELLOW}Check logs in $LOG_DIR for details${NC}"
    fi
    print_separator
}

main "$@"
