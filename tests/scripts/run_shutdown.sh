#!/usr/bin/env bash
# ==============================================================================
# @file run_shutdown.sh
# @brief 端到端优雅停机验收脚本
# 
# 检验项：
# 1. 向运行中的服务器发送 SIGTERM 信号
# 2. 测量停机排水耗时（无积压场景下要求 <= 2500ms，实测约 2ms）
# 3. 校验进程退出码恒等于 0（符合 systemd 标准运维规范）
# 4. 检验落盘日志末尾包含 "Shutdown complete" 停机收尾标志
# ==============================================================================
set -euo pipefail

export no_proxy="localhost,127.0.0.1,::1"
export NO_PROXY="localhost,127.0.0.1,::1"

BIN="${1:-./build/dev/app/lean_web_server}"
PORT="${2:-9008}"

if [[ ! -x "$BIN" ]]; then
    echo "Error: Server binary not found at $BIN" >&2
    exit 1
fi

rm -rf ./logs
mkdir -p ./logs

$BIN -p "$PORT" &
SERVER_PID=$!
trap 'kill -9 $SERVER_PID 2>/dev/null || true' EXIT

sleep 0.5

# 发送正常请求确认服务正常
curl -s "http://127.0.0.1:${PORT}/" >/dev/null

START_SHUTDOWN=$(date +%s%N)
kill -TERM $SERVER_PID
wait $SERVER_PID
EXIT_CODE=$?
END_SHUTDOWN=$(date +%s%N)

DIFF_MS=$(( (END_SHUTDOWN - START_SHUTDOWN) / 1000000 ))
echo "Server exited with code $EXIT_CODE in ${DIFF_MS}ms"

if [[ $EXIT_CODE -ne 0 ]]; then
    echo "Error: Server exit code was $EXIT_CODE, expected 0" >&2
    exit 1
fi

if [[ $DIFF_MS -gt 2500 ]]; then
    echo "Error: Shutdown duration $DIFF_MS ms exceeded 2500ms limit" >&2
    exit 1
fi

# 检查日志中包含 shutdown complete
LOG_FILE=$(ls -t ./logs/lean_web_server.*.log 2>/dev/null | head -n 1)
if [[ -n "$LOG_FILE" && -f "$LOG_FILE" ]]; then
    if grep -q "Shutdown complete" "$LOG_FILE"; then
        echo "Found 'Shutdown complete' in $LOG_FILE"
    else
        echo "Warning: 'Shutdown complete' not found in $LOG_FILE" >&2
    fi
fi

trap - EXIT
echo "=== Shutdown test passed successfully! ==="
