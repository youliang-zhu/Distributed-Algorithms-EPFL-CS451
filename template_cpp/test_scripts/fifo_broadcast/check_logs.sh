#!/bin/bash

# 快速检查最近的诊断测试输出
LATEST_DIR=$(ls -td /tmp/da_diagnostic_* 2>/dev/null | head -1)

if [ -z "$LATEST_DIR" ]; then
    echo "未找到诊断测试输出目录"
    exit 1
fi

echo "检查目录: $LATEST_DIR"
echo ""

# 测试 3 的详细分析
echo "=== 测试 3 详细分析（高负载丢失问题）==="
TEST3_DIR="$LATEST_DIR/test3"

if [ -d "$TEST3_DIR" ]; then
    echo "每个进程的广播和交付情况："
    for i in 1 2 3 4 5; do
        if [ -f "$TEST3_DIR/proc$i.output" ]; then
            BC=$(grep "^b " "$TEST3_DIR/proc$i.output" | wc -l)
            DC=$(grep "^d " "$TEST3_DIR/proc$i.output" | wc -l)
            echo "P$i: 广播 $BC 条, 交付 $DC 条"
            
            # 详细统计从每个发送者收到的消息数
            echo "  交付详情:"
            for sender in 1 2 3 4 5; do
                COUNT=$(grep "^d $sender " "$TEST3_DIR/proc$i.output" | wc -l)
                echo "    从 P$sender: $COUNT/50"
            done
            
            # 检查是否有重复
            UNIQUE=$(grep "^d " "$TEST3_DIR/proc$i.output" | sort -u | wc -l)
            TOTAL=$(grep "^d " "$TEST3_DIR/proc$i.output" | wc -l)
            if [ "$UNIQUE" -ne "$TOTAL" ]; then
                echo "  ⚠ 发现重复: $((TOTAL - UNIQUE)) 条"
            fi
            
            # 检查 FIFO 违规
            for sender in 1 2 3 4 5; do
                SEQS=$(grep "^d $sender " "$TEST3_DIR/proc$i.output" | awk '{print $3}')
                if [ -n "$SEQS" ]; then
                    SORTED=$(echo "$SEQS" | sort -n)
                    if [ "$SEQS" != "$SORTED" ]; then
                        echo "  ⚠ FIFO 违规: 来自 P$sender 的消息顺序错误"
                        echo "    实际: $(echo $SEQS | head -20)"
                        echo "    期望: $(echo $SORTED | head -20)"
                    fi
                fi
            done
            
            echo ""
        fi
    done
    
    # 检查日志是否有错误
    echo "检查进程日志错误:"
    for i in 1 2 3 4 5; do
        if [ -f "$TEST3_DIR/proc$i.log" ]; then
            ERRORS=$(grep -i "error\|segmentation\|fault\|exception" "$TEST3_DIR/proc$i.log" 2>/dev/null || true)
            if [ -n "$ERRORS" ]; then
                echo "P$i 日志有错误:"
                echo "$ERRORS"
            fi
        fi
    done
fi

echo ""
echo "=== 测试 1b 分析（快速启动）==="
TEST1B_DIR="$LATEST_DIR/test1b"

if [ -d "$TEST1B_DIR" ]; then
    for i in 1 2 3; do
        if [ -f "$TEST1B_DIR/proc$i.output" ]; then
            echo "P$i 交付详情:"
            for sender in 1 2 3; do
                COUNT=$(grep "^d $sender " "$TEST1B_DIR/proc$i.output" | wc -l)
                SEQS=$(grep "^d $sender " "$TEST1B_DIR/proc$i.output" | awk '{print $3}' | tr '\n' ' ')
                echo "  从 P$sender: $COUNT/10 - 序列: $SEQS"
            done
            echo ""
        fi
    done
fi

echo ""
echo "=== 建议检查项 ==="
echo "1. 查看是否有进程提前退出:"
ls -lh "$TEST3_DIR"/*.log 2>/dev/null
echo ""
echo "2. 查看完整输出文件大小:"
ls -lh "$TEST3_DIR"/*.output 2>/dev/null
echo ""
echo "3. 要查看某个进程的完整日志，运行:"
echo "   cat $TEST3_DIR/proc1.log"
echo "   cat $TEST3_DIR/proc1.output"
