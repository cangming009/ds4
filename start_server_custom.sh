#!/bin/bash
set -e

# ================================
# ds4-server 启动脚本 (Custom)
# 基于 start_server.sh，覆盖 3 项默认值:
#   MAX_TOKENS=8192          — 防止单请求阻塞队列
#   KV_CACHE_MAX_ENTRIES=50  — 多轮工具调用更多 KV 槽位
#   PREFILL_CHUNK=4096       — ~24% 加速首次预填
# 同步上游 start_server.sh 后，差异集中在此文件，减少冲突
# ================================

# --- 颜色 / Colors ---
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

info()  { echo -e "${CYAN}[INFO]${NC}  $1"; }
ok()    { echo -e "${GREEN}[OK]${NC}    $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
err()   { echo -e "${RED}[ERROR]${NC} $1"; }

# --- 脚本所在目录 / Script Directory ---
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# ================================
# 默认配置 / Default Configuration
# ================================
MODEL_PATH="ds4flash.gguf"
MTP_PATH=""
HOST="127.0.0.1"
PORT=8000

# ================================
# 硬件检测 / Hardware Detection
# ================================
TOTAL_RAM_GB=$(sysctl -n hw.memsize 2>/dev/null | awk '{printf "%d", $1/1073741824}')
CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)
info "检测到硬件: $(sysctl -n hw.model 2>/dev/null) | ${CPU_CORES} 核 | ${TOTAL_RAM_GB} GB 内存"

# ================================
# 最佳默认配置 / Optimal Defaults
#   Mac Studio M3 Ultra 512GB
#   定制优化: MAX_TOKENS=8192, KV_CACHE_MAX_ENTRIES=50, PREFILL_CHUNK=4096
# ================================
# Think Max 需要 ctx >= 393216，您的机器完全可以承载
CTX_SIZE=393216
MAX_TOKENS=8192
THREADS=""
KV_DISK_DIR=""
KV_DISK_SPACE_MB=16384
KV_CACHE_MAX_ENTRIES=50
PREFILL_CHUNK=4096
TRACE_FILE=""
WARM_WEIGHTS=true
QUALITY=true
LOG_DIR="logs"
LOG_FILE=""
BUILD=false

# ================================
# 参数解析 / Parse Arguments
# ================================
show_help() {
    cat <<'EOF'
用法 / Usage:
  ./start_server_custom.sh [选项/options]

基本选项 / Basic:
  -m, --model FILE      模型路径 (默认: ds4flash.gguf)
  --mtp FILE            MTP 草稿模型路径 (自动检测 gguf/*MTP*)
  --host HOST           绑定地址 (默认: 127.0.0.1)
  --port PORT           绑定端口 (默认: 8000)
  -c, --ctx N           上下文大小 (默认: 393216，开启 Think Max)
  -n, --tokens N        最大输出 tokens (默认: 8192，客户端 max_tokens 可覆盖)
  -t, --threads N       CPU helper 线程数
  --prefill-chunk N     Prefill 批处理大小 (默认: 4096, 实时设置: DS4_METAL_PREFILL_CHUNK)
  --trace FILE          写入详细 trace 日志到文件
  --warm-weights        启动时预热模型权重 (默认: 开启)
  --quality             使用精确内核 (默认: 开启)
  --no-warm-weights     禁用权重预热
  --no-quality          禁用精确内核 (使用近似路径)
  --build               启动前重新编译

磁盘 KV 缓存 / Disk KV Cache:
  --kv-disk-dir DIR     磁盘 KV 缓存目录 (默认启用: ./kv-cache)
  --kv-disk-space-mb N  磁盘缓存预算 (默认: 16384 MB)
  --kv-cache-max-entries N  最大缓存文件数 (LRU, 0=不限, 默认: 50)
  --no-kv-cache         禁用磁盘 KV 缓存

日志 / Logging:
  --log-dir DIR         日志输出目录 (默认: logs)
  --log-file FILE       日志文件路径 (覆盖 --log-dir)

其他:
  -h, --help            显示此帮助

示例 / Examples:
  # 默认最佳启动 (Think Max + KV缓存 + 权重预热 + chunk=4096)
  ./start_server_custom.sh

  # 自定义模型路径
  ./start_server_custom.sh -m /path/to/model.gguf

  # 禁用缓存，使用近似内核
  ./start_server_custom.sh --no-kv-cache --no-quality

  # 修改端口，暴露到局域网
  ./start_server_custom.sh --port 8080 --host 0.0.0.0
EOF
    exit 0
}

BUILD_ARGS=""
SERVER_ARGS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--help) show_help ;;
        -m|--model)
            MODEL_PATH="$2"; shift 2 ;;
        --mtp)
            MTP_PATH="$2"; shift 2 ;;
        --prefill-chunk)
            PREFILL_CHUNK="$2"; shift 2 ;;
        --host)
            HOST="$2"; shift 2 ;;
        --port)
            PORT="$2"; shift 2 ;;
        -c|--ctx)
            CTX_SIZE="$2"; shift 2 ;;
        -n|--tokens)
            MAX_TOKENS="$2"; shift 2 ;;
        -t|--threads)
            THREADS="$2"; shift 2 ;;
        --kv-disk-dir)
            KV_DISK_DIR="$2"; shift 2 ;;
        --kv-disk-space-mb)
            KV_DISK_SPACE_MB="$2"; shift 2 ;;
        --kv-cache-max-entries)
            KV_CACHE_MAX_ENTRIES="$2"; shift 2 ;;
        --no-kv-cache)
            KV_DISK_DIR="SKIP"; shift ;;
        --trace)
            TRACE_FILE="$2"; shift 2 ;;
        --warm-weights)
            WARM_WEIGHTS=true; shift ;;
        --no-warm-weights)
            WARM_WEIGHTS=false; shift ;;
        --quality)
            QUALITY=true; shift ;;
        --no-quality)
            QUALITY=false; shift ;;
        --build)
            BUILD=true; shift ;;
        --log-dir)
            LOG_DIR="$2"; shift 2 ;;
        --log-file)
            LOG_FILE="$2"; shift 2 ;;
        *)
            err "未知选项 / Unknown option: $1"
            echo "使用 / Use: ./start_server_custom.sh --help"
            exit 1 ;;
    esac
done

# ================================
# 清理旧进程 / Cleanup Old Process
# ================================
cleanup_old_process() {
    local pids
    pids=$(pgrep -f "ds4-server" 2>/dev/null || true)
    if [[ -n "$pids" ]]; then
        warn "检测到正在运行的 ds4-server (PID: $(echo $pids | tr '\n' ' '))"
        warn "正在停止旧进程..."
        kill $pids 2>/dev/null || true
        sleep 1
        # 强制终止
        pids=$(pgrep -f "ds4-server" 2>/dev/null || true)
        if [[ -n "$pids" ]]; then
            warn "进程未响应，强制终止..."
            kill -9 $pids 2>/dev/null || true
            sleep 0.5
        fi
        ok "旧进程已清理"
    else
        info "没有检测到正在运行的 ds4-server"
    fi
}

# ================================
# 检查模型文件 / Check Model File
# ================================
check_model() {
    if [[ ! -f "$MODEL_PATH" ]]; then
        if [[ -L "$MODEL_PATH" ]]; then
            local target
            target=$(readlink "$MODEL_PATH")
            if [[ ! -f "$target" ]]; then
                err "模型文件符号链接指向不存在: $target"
                err "请检查模型文件是否正确"
                exit 1
            fi
        else
            err "模型文件不存在: $MODEL_PATH"
            err "请确认模型文件路径，或使用 -m 指定"
            exit 1
        fi
    fi
    ok "模型文件: $MODEL_PATH"
}

# ================================
# 自动检测 MTP 模型 / Auto-detect MTP
# ================================
detect_mtp() {
    if [[ -z "$MTP_PATH" ]]; then
        local mtp_file
        mtp_file=$(ls gguf/*MTP*.gguf 2>/dev/null | head -1)
        if [[ -n "$mtp_file" ]]; then
            MTP_PATH="$mtp_file"
            ok "自动检测到 MTP 模型: $MTP_PATH"
        fi
    fi
    if [[ -n "$MTP_PATH" ]]; then
        if [[ ! -f "$MTP_PATH" ]]; then
            warn "MTP 模型文件不存在: $MTP_PATH，跳过"
            MTP_PATH=""
        else
            ok "MTP 模型: $MTP_PATH"
        fi
    fi
}

# ================================
# 设置默认 KV 缓存 / Setup KV Cache
# ================================
setup_kv_cache() {
    if [[ "$KV_DISK_DIR" == "SKIP" ]]; then
        KV_DISK_DIR=""
        info "磁盘 KV 缓存已禁用"
    elif [[ -z "$KV_DISK_DIR" ]]; then
        KV_DISK_DIR="./kv-cache"
        info "磁盘 KV 缓存: $KV_DISK_DIR (${KV_DISK_SPACE_MB} MB)"
    fi
}

# ================================
# 构建 / Build
# ================================
do_build() {
    info "正在编译 ds4-server..."
    if make ds4-server 2>&1; then
        ok "编译成功"
    else
        err "编译失败，请检查错误信息"
        exit 1
    fi
}

# ================================
# 设置日志 / Setup Logging
# ================================
setup_logging() {
    if [[ -z "$LOG_FILE" ]]; then
        mkdir -p "$LOG_DIR"
        LOG_FILE="${LOG_DIR}/ds4-server_$(date +%Y%m%d_%H%M%S).log"
    fi
    info "日志文件: $LOG_FILE"
}

# ================================
# 构建启动命令 / Build Server Command
# ================================
build_command() {
    local cmd="env DS4_METAL_PREFILL_CHUNK=$PREFILL_CHUNK ./ds4-server"
    cmd+=" -m $MODEL_PATH"
    cmd+=" -c $CTX_SIZE"
    cmd+=" -n $MAX_TOKENS"
    cmd+=" --host $HOST"
    cmd+=" --port $PORT"

    [[ -n "$MTP_PATH" ]]        && cmd+=" --mtp $MTP_PATH"
    [[ -n "$THREADS" ]]         && cmd+=" -t $THREADS"
    [[ "$WARM_WEIGHTS" == true ]] && cmd+=" --warm-weights"
    [[ "$QUALITY" == true ]]       && cmd+=" --quality"
    [[ -n "$KV_DISK_DIR" ]]     && cmd+=" --kv-disk-dir $KV_DISK_DIR --kv-disk-space-mb $KV_DISK_SPACE_MB"
    [[ -n "$KV_CACHE_MAX_ENTRIES" ]] && cmd+=" --kv-cache-max-entries $KV_CACHE_MAX_ENTRIES"
    [[ -n "$TRACE_FILE" ]]      && cmd+=" --trace $TRACE_FILE"

    echo "$cmd"
}

# ================================
# 打印启动信息 / Print Startup Info
# ================================
print_summary() {
    echo ""
    echo "=========================================="
    echo -e "  ${CYAN}ds4-server 配置总结${NC}"
    echo "=========================================="
    echo -e "  模型 / Model:       ${YELLOW}$MODEL_PATH${NC}"
    [[ -n "$MTP_PATH" ]] && \
        echo -e "  MTP 草稿模型:      ${YELLOW}$MTP_PATH${NC}"
    echo -e "  Prefill Chunk:      ${YELLOW}$PREFILL_CHUNK${NC}"
    echo -e "  地址 / Host:        ${YELLOW}$HOST${NC}"
    echo -e "  端口 / Port:        ${YELLOW}$PORT${NC}"
    echo -e "  上下文 / Context:   ${YELLOW}$CTX_SIZE${NC}"
    echo -e "  最大输出 Tokens:    ${YELLOW}$MAX_TOKENS${NC}"
    [[ -n "$THREADS" ]] && \
        echo -e "  线程 / Threads:     ${YELLOW}$THREADS${NC}"
    [[ "$WARM_WEIGHTS" == true ]] && \
        echo -e "  预热权重:           ${YELLOW}是${NC}"
    [[ "$QUALITY" == true ]] && \
        echo -e "  精确内核:           ${YELLOW}是${NC}"
    [[ -n "$KV_DISK_DIR" ]] && \
        echo -e "  KV 缓存目录:        ${YELLOW}$KV_DISK_DIR ($KV_DISK_SPACE_MB MB)${NC}"
    [[ -n "$TRACE_FILE" ]] && \
        echo -e "  Trace 日志:         ${YELLOW}$TRACE_FILE${NC}"
    echo -e "  日志文件:           ${YELLOW}$LOG_FILE${NC}"
    echo "=========================================="
    echo ""
    echo -e "  API 地址: ${GREEN}http://$HOST:$PORT${NC}"
    echo -e "  OpenAI 兼容: ${CYAN}http://$HOST:$PORT/v1/chat/completions${NC}"
    echo -e "  Anthropic 兼容: ${CYAN}http://$HOST:$PORT/v1/messages${NC}"
    echo ""
    echo -e "  停止服务: ${YELLOW}Ctrl+C${NC} 或 ${YELLOW}kill \$(pgrep ds4-server)${NC}"
    echo ""
}

# ================================
# 启动服务器 / Start Server
# ================================
start_server() {
    local cmd
    cmd=$(build_command)
    echo -e "${GREEN}启动命令 / Command:${NC} $cmd"
    echo ""

    # 使用 tee 同时输出到终端和日志文件
    echo "==========================================" >> "$LOG_FILE"
    echo "ds4-server started at $(date)" >> "$LOG_FILE"
    echo "Command: $cmd" >> "$LOG_FILE"
    echo "==========================================" >> "$LOG_FILE"

    # 启动服务器，stdout/stderr 都输出到终端和日志文件
    $cmd 2>&1 | tee -a "$LOG_FILE"
    local exit_code=${PIPESTATUS[0]}

    echo "" >> "$LOG_FILE"
    echo "ds4-server exited at $(date) with code $exit_code" >> "$LOG_FILE"

    if [[ $exit_code -ne 0 ]]; then
        err "ds4-server 异常退出 (exit code: $exit_code)"
        err "请检查日志文件: $LOG_FILE"
        if [[ -n "$TRACE_FILE" ]]; then
            err "Trace 日志: $TRACE_FILE"
        fi
    fi
    return $exit_code
}

# ================================
# Main
# ================================
echo ""
echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  ds4-server 启动脚本 / Start Script${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

cleanup_old_process
check_model
detect_mtp
setup_kv_cache
[[ "$BUILD" == true ]] && do_build
setup_logging
print_summary

echo -e "${GREEN}正在启动 ds4-server...${NC}"
start_server
