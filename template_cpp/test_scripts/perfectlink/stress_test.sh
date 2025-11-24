#!/bin/bash
# 文件名: stress_test.sh
# 路径: template_cpp/test_scripts/perfectlink/stress_test.sh
# 用途: 使用 tools/stress.py 对 Perfect Link 实现进行进程崩溃和暂停的压力测试。
# 验证：在进程随机停止/重启/崩溃的情况下，Perfect Link 是否仍能保证最终交付。

set -e

# --- 配置和路径 ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR/../../.." # CS451-2025-project/
TEMPLATE_DIR="$ROOT_DIR/template_cpp" # template_cpp/

# stress.py 路径: 从当前脚本向上三级到根目录，再进入 tools/
STRESS_PY="$ROOT_DIR/tools/stress.py"
# run.sh 路径: 从当前脚本向上两级
RUN_SH="$TEMPLATE_DIR/run.sh"

# 使用 $$ 确保每次运行的临时目录唯一
OUTPUT_DIR="/tmp/da_stress_$$"
STRESS_LOG="$OUTPUT_DIR/stress_run_log.txt"
PID_FILE="$OUTPUT_DIR/pids.txt"

# --- 颜色定义 ---
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m'

# --- 全局变量 ---
# Receiver ID 在 stress.py 生成的 hosts 文件中始终是最后一个进程。
RECEIVER_ID=0

# --- 清理函数 ---
cleanup() {
    echo -e "\n${BLUE}Cleanup: Terminating processes and removing $OUTPUT_DIR...${NC}"
    # 确保所有由 stress.py 启动的 da_proc 进程被终止 (包括那些可能被 SIGSTOP 暂停的)
    kill -9 $(ps aux | grep 'da_proc' | grep -v 'grep' | awk '{print $2}') 2>/dev/null || true
    rm -rf "$OUTPUT_DIR"
    echo -e "${GREEN}Cleanup complete.${NC}"
}
trap cleanup EXIT

# --- 辅助函数：诊断和进程状态检查 ---
diagnose_stuck_processes() {
    local max_wait_sec=15 # 硬性等待超时时间
    local timeout_reached=0
    
    echo -e "\n${YELLOW}--- DIAGNOSTIC PHASE ---${NC}"
    echo -e "${YELLOW}Waiting up to ${max_wait_sec}s for all processes to exit...${NC}"
    
    # 记录 stress.py 报告的逻辑 PID 到 物理 PID 的映射
    declare -A logical_to_pid
    # 假设 stress.py 的输出已捕获到 $STRESS_LOG
    if [ -f "$STRESS_LOG" ]; then
        grep "Process with logicalPID" "$STRESS_LOG" | while read -r line; do
            local logical_id=$(echo "$line" | awk '{print $4}')
            local pid=$(echo "$line" | awk '{print $7}')
            logical_to_pid[$logical_id]=$pid
        done
    fi
    
    local procs_to_check=$(seq 1 $RECEIVER_ID)
    local start_time=$(date +%s)
    
    while true; do
        local running_procs=""
        local all_exited=true

        for logical_id in $procs_to_check; do
            local pid=${logical_to_pid[$logical_id]}
            # 检查 PID 是否仍在运行
            if ps -p "$pid" > /dev/null 2>&1; then
                running_procs+="$logical_id (PID $pid) "
                all_exited=false
            fi
        done

        if $all_exited; then
            echo -e "${GREEN}All da_proc processes have exited successfully.${NC}"
            break
        fi

        local current_time=$(date +%s)
        if [ $((current_time - start_time)) -ge $max_wait_sec ]; then
            timeout_reached=1
            echo -e "${RED}TIMEOUT reached after ${max_wait_sec}s.${NC}"
            break
        fi

        # 检查频率
        sleep 1
    done

    if [ $timeout_reached -eq 1 ]; then
        echo -e "${RED}STUCK PROCESSES FOUND:${NC}"
        for logical_id in $procs_to_check; do
            local pid=${logical_to_pid[$logical_id]}
            if ps -p "$pid" > /dev/null 2>&1; then
                echo -e "--- Logical ID $logical_id (PID $pid) is ${RED}STILL RUNNING${NC} ---"
                
                # 打印进程状态 (STAT) 和命令行
                ps -p "$pid" -o pid,stat,cmd

                # 打印进程的输出文件 (procXX.output) 的最后 10 行
                echo -e "${YELLOW}Last 10 output lines ($OUTPUT_DIR/proc${logical_id}.output):${NC}"
                tail -n 10 "$OUTPUT_DIR/proc${logical_id}.output" || echo "(File not readable or empty)"
                
                # 打印进程的标准错误文件 (procXX.stderr) 的最后 10 行
                echo -e "${YELLOW}Last 10 stderr lines ($OUTPUT_DIR/proc${logical_id}.stderr):${NC}"
                tail -n 10 "$OUTPUT_DIR/proc${logical_id}.stderr" || echo "(File not readable or empty)"
            fi
        done
        # 由于进程卡住，我们强制终止并返回失败
        echo -e "${RED}Forcing cleanup (kill -9) for the stuck processes now.${NC}"
        return 1
    fi
    
    return 0
}

# --- 辅助函数：验证输出 (保持不变) ---
verify_output() {
    local test_name=$1
    local num_processes=$2
    local num_messages=$3
    local receiver_output="$OUTPUT_DIR/proc${RECEIVER_ID}.output"
    local num_senders=$((num_processes - 1))
    local total_expected=$((num_messages * num_senders))
    
    echo -e "${BLUE}  Verifying output for $test_name...${NC}"
    
    if [ ! -f "$receiver_output" ]; then
        echo -e "${RED}❌ FAIL: Receiver output file not found!${NC}"
        return 1
    fi

    local total_delivered=$(grep -c "^d " "$receiver_output" 2>/dev/null || echo 0)
    
    echo "  Expected total deliveries: $total_expected"
    echo "  Actual total deliveries: $total_delivered"

    if [ "$total_expected" -ne "$total_delivered" ]; then
        echo -e "${RED}❌ FAIL: Total delivery count mismatch! Expected $total_expected, got $total_delivered.${NC}"
        return 1
    fi
    
    # 检查序列完整性
    for sender_id in $(seq 1 $num_senders); do
        local count=$(grep "^d $sender_id " "$receiver_output" | wc -l)
        if [ "$count" -ne "$num_messages" ]; then
            echo -e "${RED}❌ FAIL: Missing messages from sender $sender_id! Expected $num_messages, got $count.${NC}"
            return 1
        fi
        
        # 简单检查是否有重复或乱序（如果乱序严重可能导致输出文件行数不足，但主要检查序列完整）
        for i in $(seq 1 $num_messages); do
            if ! grep -q "^d $sender_id $i$" "$receiver_output"; then
                echo -e "${RED}❌ FAIL: Missing delivery of message $i from sender $sender_id (Full sequence required).${NC}"
                return 1
            fi
        done
    done
    
    echo -e "${GREEN}✅ PASS: All deliveries successful and correct under stress!${NC}"
    return 0
}

# --- 运行压力测试函数 ---
run_stress_test() {
    local test_name=$1
    local num_processes=$2   # P: 总进程数 (P-1 个 Sender + 1 Receiver)
    local num_messages=$3    # M: 每个 Sender 发送的消息数
    local wait_time=$4       # 等待消息完成的时间（秒）
    
    echo -e "\n${BLUE}========================================================================${NC}"
    echo -e "${BLUE}STRESS: $test_name | P: $num_processes, M: $num_messages, Total: $(( (num_processes - 1) * num_messages )) messages${NC}"
    echo -e "${BLUE}========================================================================${NC}"
    
    # --- 1. 设置 ---
    mkdir -p "$OUTPUT_DIR"
    RECEIVER_ID=$num_processes
    
    echo -e "${BLUE}Starting stress test (P=$num_processes, M=$num_messages).${NC}"
    echo -e "${BLUE}The test will simulate process stops/crashes, then wait ${wait_time}s for recovery.${NC}"

    # --- 2. 运行 stress.py ---
    # 我们捕获 stress.py 的输出到文件，以便解析 PIDs
    echo -e "${YELLOW}Running command: ( sleep ${wait_time}; echo ) | ${STRESS_PY} perfect -r ${RUN_SH} -l ${OUTPUT_DIR} -p ${num_processes} -m ${num_messages} > ${STRESS_LOG} 2>&1${NC}"
    
    # 运行 stress.py，将输出重定向到日志文件，并使用子shell管道注入 Enter 键
    ( sleep "$wait_time"; echo ) | "$STRESS_PY" perfect \
        -r "$RUN_SH" \
        -l "$OUTPUT_DIR" \
        -p "$num_processes" \
        -m "$num_messages" > "$STRESS_LOG" 2>&1 &
    
    STRESS_PID=$! # 获取 stress.py 的 PID
    
    # 额外等待 time_to_wait_for_stress_completion 秒，让 stress.py 内部的 monitor 线程完成
    # 保持 30 秒的额外等待，但由于 wait_time 增加了，总时间也更长了
    local stress_completion_wait=30 
    
    echo -e "${BLUE}Stress tool (PID $STRESS_PID) is running. Waiting up to $((wait_time + stress_completion_wait))s for completion...${NC}"
    
    # 等待 stress.py 进程结束
    # 结合 wait_time 和 stress_completion_wait 来设置一个最大容忍时间
    local total_wait_time=$((wait_time + stress_completion_wait))
    
    local start_wait=$(date +%s)
    while ps -p $STRESS_PID > /dev/null 2>&1; do
        current_wait=$(date +%s)
        if [ $((current_wait - start_wait)) -ge $total_wait_time ]; then
             echo -e "${RED}ERROR: stress.py itself timed out! Forcing kill and proceeding to diagnosis.${NC}"
             kill -9 $STRESS_PID 2>/dev/null || true
             local result=1
             break
        fi
        sleep 2
    done
    
    if [ $result -ne 1 ]; then
        # 如果 stress.py 没有超时，获取它的退出码
        wait $STRESS_PID
        local result=$?
    fi

    # 打印 stress.py 的输出，包含 PIDs 和发送的信号
    echo -e "${YELLOW}--- stress.py LOG START ---${NC}"
    cat "$STRESS_LOG"
    echo -e "${YELLOW}--- stress.py LOG END ---${NC}"

    if [ $result -ne 0 ]; then
        echo -e "${RED}❌ FAIL: Stress tool returned an error code ($result). Check logs in $OUTPUT_DIR.${NC}"
        # 即使 stress.py 失败，我们也要尝试诊断 da_proc
        diagnose_stuck_processes
        return 1
    fi
    
    # --- 3. 诊断卡住进程 ---
    # 在验证前，给所有 da_proc 进程一个硬性退出时间
    diagnose_stuck_processes
    local diagnose_result=$?
    
    # --- 4. 验证结果 ---
    verify_output "$test_name" "$num_processes" "$num_messages"
    local verify_result=$?

    # 最终结果是进程卡住或验证失败
    return $((diagnose_result | verify_result))
}

# --- 主测试流程 (保持不变) ---
main() {
    echo -e "${BLUE}╔══════════════════════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║  Perfect Links Process Interruption (STRESS) Test        ║${NC}"
    echo -e "${BLUE}╚══════════════════════════════════════════════════════════╝${NC}"
    
    # 检查 stress.py 存在性
    if [ ! -f "$STRESS_PY" ]; then
        echo -e "${RED}Error: stress.py not found at $STRESS_PY${NC}"
        exit 1
    fi

    local overall_status=0

    # ========================================================================
    # 难度等级 1: 轻度压力 (Level1_LightStress)
    # 增加等待时间从 30s 到 45s (总超时: 45+30=75s)
    # ========================================================================
    run_stress_test "Level1_LightStress" 5 100 45 
    level1_result=$?
    ((overall_status |= level1_result))

    # ========================================================================
    # 难度等级 2: 中等压力 (Level2_ModerateStress)
    # 增加等待时间从 45s 到 60s (总超时: 60+30=90s)
    # ========================================================================
    if [ $level1_result -eq 0 ]; then
        run_stress_test "Level2_ModerateStress" 10 300 60
        level2_result=$?
        ((overall_status |= level2_result))
    else
        echo -e "${YELLOW}Skipping Level 2 due to Level 1 failure.${NC}"
        level2_result=1
    fi

    # ========================================================================
    # 难度等级 3: 高强度压力 (Level3_HeavyStress)
    # 增加等待时间从 60s 到 90s (总超时: 90+30=120s)
    # ========================================================================
    if [ $overall_status -eq 0 ]; then
        run_stress_test "Level3_HeavyStress" 15 500 90
        level3_result=$?
        ((overall_status |= level3_result))
    else
        echo -e "${YELLOW}Skipping Level 3 due to previous failures.${NC}"
        level3_result=1
    fi
    
    # --- 汇总结果 ---
    echo -e "\n${BLUE}╔════════════════════════════════════════╗${NC}"
    echo -e "${BLUE}║            压力测试汇总                 ║${NC}"
    echo -e "${BLUE}╚════════════════════════════════════════╝${NC}"
    
    [ $level1_result -eq 0 ] && echo -e "Level 1 (Light Stress):   ${GREEN}✅ PASS${NC}" || echo -e "Level 1 (Light Stress):   ${RED}❌ FAIL${NC}"
    [ $level2_result -eq 0 ] && echo -e "Level 2 (Moderate Stress): ${GREEN}✅ PASS${NC}" || echo -e "Level 2 (Moderate Stress): ${RED}❌ FAIL${NC}"
    [ $level3_result -eq 0 ] && echo -e "Level 3 (Heavy Stress):   ${GREEN}✅ PASS${NC}" || echo -e "Level 3 (Heavy Stress):   ${RED}❌ FAIL${NC}"
    
    if [ $overall_status -eq 0 ]; then
        echo -e "\n${GREEN}🎉 所有压力测试通过! Perfect Link 实现对于进程故障具有极高的鲁棒性。${NC}"
    else
        echo -e "\n${RED}⚠️ 发现鲁棒性问题! 请检查 $OUTPUT_DIR 中的详细日志文件。${NC}"
    fi
    
    return $overall_status
}

# 运行主程序
main "$@"