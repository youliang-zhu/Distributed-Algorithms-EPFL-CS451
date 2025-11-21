#!/usr/bin/env bash
# pl_diagnose.sh — Perfect Link 里程碑自检脚本
# 用途：
#  1) 自动构造 hosts 与 config（m i），启动 n 个进程（n-1 发送者，1 接收者）
#  2) 等待并发送 SIGTERM，收集输出
#  3) 验证：
#     - PL1 可靠交付：接收者是否收齐每个发送者的 1..m（逐一列出缺失）
#     - PL2 不重复：是否存在重复交付 d sender seq（接收者侧）
#     - PL3 不创造：接收者 d sender seq 必须能在对应发送者 b seq 中找到
#     - 日志格式：仅允许 "b <seq>" 与 "d <sender> <seq>"（且为正整数）
#     - 信号后静默：发 SIGTERM 后文件是否仍显著增长（仅提示，不作为硬 FAIL）
#
# 依赖：bash、grep、awk、sort、uniq、wc、mktemp、date、pkill
# 约定：在 template_cpp 根目录运行；用 ./build.sh 与 ./run.sh 启动
#
# 参考要求（项目文档）：
#  - Perfect Links 性质 PL1/PL2/PL3：可靠交付 / 不重复 / 不创造
#  - CLI/日志格式/信号处理规则
#  - 单报文最多 8 条消息属于实现细节限制，本脚本不直接验证（抓不到报文）
#
# 文档出处：
#   - Project description（接口、性质、日志、信号、运行方式）:contentReference[oaicite:2]{index=2}
#   - Project introduction（里程碑与评测概览）:contentReference[oaicite:3]{index=3}

set -euo pipefail

# ===== 默认参数 =====
PROCS=4            # 进程总数 n
RECEIVER_ID=4      # 接收者 id = i
MESSAGES=100       # 每个发送者的消息数 m
WAIT_BEFORE_TERM=5 # 发送 SIGTERM 前等待秒数
ROOT_DIR="$(pwd)"
BIN="./bin/da_proc"
RUN="./run.sh"
BUILD="./build.sh"

# ===== 解析命令行 =====
usage() {
  cat <<EOF
用法: bash $0 [-p PROCS] [-r RECEIVER_ID] [-m MESSAGES] [-w WAIT_SECONDS]
示例: bash $0 -p 4 -r 3 -m 100 -w 5
  -p 进程个数 (默认 4)
  -r 接收者ID (默认 4)
  -m 每个发送者消息数 m (默认 100)
  -w SIGTERM 之前等待的秒数 (默认 5)
EOF
  exit 1
}

while getopts ":p:r:m:w:h" opt; do
  case $opt in
    p) PROCS="$OPTARG" ;;
    r) RECEIVER_ID="$OPTARG" ;;
    m) MESSAGES="$OPTARG" ;;
    w) WAIT_BEFORE_TERM="$OPTARG" ;;
    h) usage ;;
    \?) usage ;;
  esac
done

# ===== 工作目录 =====
WORKDIR="$(mktemp -d -p . pltest.XXXXXX)"
HOSTS_FILE="$WORKDIR/hosts"
CONFIG_FILE="$WORKDIR/perfect-links.config"
OUTDIR="$WORKDIR/output"
LOGDIR="$WORKDIR/logs"
mkdir -p "$OUTDIR" "$LOGDIR"

# ===== 颜色 =====
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

# ===== 小工具 =====
pass() { echo -e "${GREEN}✓ $*${NC}"; }
warn() { echo -e "${YELLOW}⚠ $*${NC}"; }
fail() { echo -e "${RED}✗ $*${NC}"; }

# ===== 构造 hosts 与 config =====
gen_hosts() {
  : > "$HOSTS_FILE"
  local base_port=11001
  for ((i=1;i<=PROCS;i++)); do
    echo "$i localhost $((base_port + i - 1))" >> "$HOSTS_FILE"
  done
}
gen_config() {
  echo "$MESSAGES $RECEIVER_ID" > "$CONFIG_FILE"
}

# ===== 构建 =====
build_bin() {
  if [[ ! -x "$BIN" ]]; then
    echo -e "${CYAN}构建项目...${NC}"
    bash "$BUILD"
  fi
  [[ -x "$BIN" ]] || { fail "可执行文件 $BIN 不存在或不可执行"; exit 1; }
}

# ===== 启动进程 =====
PIDS=()
start_procs() {
  echo -e "${CYAN}启动 $PROCS 个进程（接收者=$RECEIVER_ID，发送者=${PROCS}-1）...${NC}"
  for ((i=1;i<=PROCS;i++)); do
    local of="$OUTDIR/${i}.output"
    local lf="$LOGDIR/${i}.log"
    bash "$RUN" --id "$i" --hosts "$HOSTS_FILE" --output "$of" "$CONFIG_FILE" \
      >"$lf" 2>&1 &
    PIDS+=($!)
  done
}

# ===== 清理 =====
cleanup() {
  pkill -f "da_proc" 2>/dev/null || true
}
trap cleanup EXIT

# ===== 校验：日志格式 =====
check_format() {
  local ok=1
  for ((i=1;i<=PROCS;i++)); do
    local of="$OUTDIR/${i}.output"
    if [[ ! -f "$of" ]]; then
      fail "进程 $i 未生成输出文件"
      ok=0
      continue
    fi
    # 只允许两种行： b <seq>  或  d <sender> <seq>；均为正整数
    if grep -v -E '^(b [1-9][0-9]*|d [1-9][0-9]* [1-9][0-9]*)$' "$of" >/dev/null 2>&1; then
      fail "进程 $i 的输出含非法行（格式不合规）"
      ok=0
    fi
  done
  [[ $ok -eq 1 ]] && pass "日志格式检查通过（仅有 b/d 且数字为正整数）"
  return $((ok==1?0:1))
}

# ===== 校验：发送者 b 1..m 完整且无重复 =====
check_senders_b() {
  local ok=1
  for ((i=1;i<=PROCS;i++)); do
    [[ $i -eq $RECEIVER_ID ]] && continue
    local of="$OUTDIR/${i}.output"
    local count=$(grep -E '^b ' "$of" | wc -l || echo 0)
    if [[ "$count" -ne "$MESSAGES" ]]; then
      fail "Sender $i：b 行数 $count != $MESSAGES"
      ok=0
    fi
    # 是否正好是 1..m
    local miss=$(comm -23 <(seq 1 "$MESSAGES") <(grep -E '^b ' "$of" | awk '{print $2}' | sort -n) | head -20)
    if [[ -n "$miss" ]]; then
      fail "Sender $i：缺失 b 序号（前20）: $(echo "$miss" | xargs)"
      ok=0
    fi
    # 重复
    if grep -E '^b ' "$of" | awk '{print $2}' | sort -n | uniq -d | grep . >/dev/null; then
      fail "Sender $i：存在重复 b 序号"
      ok=0
    fi
  done
  [[ $ok -eq 1 ]] && pass "所有发送者 b 1..$MESSAGES 完整且无重复"
  return $((ok==1?0:1))
}

# ===== 校验：接收者交付（PL1/PL2） =====
check_receiver_delivery() {
  local of="$OUTDIR/${RECEIVER_ID}.output"
  if [[ ! -f "$of" ]]; then fail "接收者未生成输出文件"; return 1; fi
  local ok=1

  # PL2 不重复：同一 sender/seq 不能交付多次
  if awk '/^d /{print $2" "$3}' "$of" | sort | uniq -d | grep . >/dev/null; then
    fail "接收者存在重复交付（违反 PL2）"
    ok=0
  else
    pass "PL2 不重复：通过"
  fi

  # PL1 可靠交付：统计每个发送者缺失
  local any_miss=0
  for ((s=1;s<=PROCS;s++)); do
    [[ $s -eq $RECEIVER_ID ]] && continue
    local delivered=$(awk -v sid="$s" '$1=="d" && $2==sid {print $3}' "$of" | sort -n)
    local count=$(echo "$delivered" | wc -l || echo 0)
    if [[ "$count" -ne "$MESSAGES" ]]; then
      any_miss=1
      local miss=$(comm -23 <(seq 1 "$MESSAGES") <(echo "$delivered") | head -20)
      fail "PL1 可靠交付：Sender $s 缺失 $(($MESSAGES - $count)) 条（前20缺失: $(echo "$miss" | xargs) ...）"
    else
      pass "PL1 可靠交付：Sender $s 收齐 $MESSAGES 条"
    fi
  done
  [[ $any_miss -eq 0 ]] || ok=0

  return $((ok==1?0:1))
}

# ===== 校验：PL3 不创造 =====
check_no_creation() {
  local ofr="$OUTDIR/${RECEIVER_ID}.output"
  local ok=1
  # 为每个 sender 建索引集合
  for ((s=1;s<=PROCS;s++)); do
    [[ $s -eq $RECEIVER_ID ]] && continue
    awk '$1=="b"{print $2}' "$OUTDIR/${s}.output" | sort -n | uniq > "$WORKDIR/b_sender_${s}.txt"
  done
  # 检查每条 d s seq 是否出现在对应 sender 的 b seq
  while read -r _ s seq; do
    if ! grep -qx "$seq" "$WORKDIR/b_sender_${s}.txt"; then
      fail "PL3 不创造：发现 d $s $seq 但 Sender $s 未记录 b $seq"
      ok=0
      break
    fi
  done < <(awk '$1=="d"{print}' "$ofr")
  [[ $ok -eq 1 ]] && pass "PL3 不创造：通过"
  return $((ok==1?0:1))
}

# ===== 软检测：SIGTERM 后静默性（仅提示） =====
check_post_term_quiet() {
  local before_sizes="$WORKDIR/sizes.before"
  local after_sizes="$WORKDIR/sizes.after"
  : > "$before_sizes"
  : > "$after_sizes"
  for ((i=1;i<=PROCS;i++)); do
    local of="$OUTDIR/${i}.output"
    [[ -f "$of" ]] && echo "$i $(wc -l < "$of")" >> "$before_sizes" || echo "$i 0" >> "$before_sizes"
  done
  # 发送 SIGTERM
  echo -e "${CYAN}发送 SIGTERM，等待 2 秒收尾...${NC}"
  pkill -TERM -f "da_proc" || true
  sleep 2
  for ((i=1;i<=PROCS;i++)); do
    local of="$OUTDIR/${i}.output"
    [[ -f "$of" ]] && echo "$i $(wc -l < "$of")" >> "$after_sizes" || echo "$i 0" >> "$after_sizes"
  done
  echo -e "${CYAN}SIGTERM 前后行数变化（仅提示）：${NC}"
  join -j 1 <(sort -k1,1 "$before_sizes") <(sort -k1,1 "$after_sizes") \
    | awk '{d=$3-$2; printf("  进程 %-3s：%s -> %s (Δ=%s)\n",$1,$2,$3,d)}'
  warn "根据规范，信号后只允许写文件，不允许再收发网络包（此项仅作提示，非硬性判定）。"
}

# ===== 主流程 =====
echo -e "${CYAN}====== Perfect Link 自检开始 ======${NC}"
echo "参数：PROCS=$PROCS, RECEIVER_ID=$RECEIVER_ID, MESSAGES=$MESSAGES, WAIT=$WAIT_BEFORE_TERM s"
gen_hosts
gen_config
build_bin
start_procs

echo -e "${CYAN}运行中，等待 ${WAIT_BEFORE_TERM}s ...${NC}"
sleep "$WAIT_BEFORE_TERM"

check_post_term_quiet

echo -e "${CYAN}开始验证输出...${NC}"
RET=0
check_format           || RET=1
check_senders_b        || RET=1
check_receiver_delivery|| RET=1
check_no_creation      || RET=1

echo -e "${CYAN}====== 自检总结 ======${NC}"
if [[ $RET -eq 0 ]]; then
  pass "🎉 关键属性与格式全部通过。可进一步用 tc/netem 与 stress 脚本做鲁棒性/性能测试。"
else
  fail "存在未通过项。上面已列出具体问题与缺失序号，按项修复后重跑本脚本。"
fi

echo -e "${CYAN}产物目录：${WORKDIR}${NC}"
echo -e "${CYAN}- hosts:     ${HOSTS_FILE}\n- config:    ${CONFIG_FILE}\n- 输出:      ${OUTDIR}\n- 运行日志:  ${LOGDIR}${NC}"
