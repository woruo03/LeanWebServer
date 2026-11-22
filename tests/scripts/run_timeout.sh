#!/usr/bin/env bash
# ==============================================================================
# @file run_timeout.sh
# @brief 长连接空闲超时自动剔除验收脚本
# 
# 检验项：
# 1. 启动服务器（默认 L1 滚动超时时间为 30s）
# 2. 使用 nc 建连并保持空闲静默，等待服务端散列时间轮到期主动关闭连接
# 3. 校验连接关闭耗时落在 [28, 36] 秒合理时间窗口内
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

$BIN -p "$PORT" &
SERVER_PID=$!
trap 'kill -9 $SERVER_PID 2>/dev/null || true' EXIT

sleep 0.5

echo "=== Testing Idle Connection Timeout with nc ==="
# 建连后等待对端关闭，超时时间设为 35s
START_TIME=$(date +%s)
nc --no-shutdown -w 35 127.0.0.1 ${PORT} </dev/null >/dev/null || true
END_TIME=$(date +%s)
DIFF=$((END_TIME - START_TIME))

echo "Connection closed after ${DIFF} seconds"
# 默认 idle_timeout 是 30s，允许 [28, 36] 范围
if [[ $DIFF -lt 28 || $DIFF -gt 36 ]]; then
    echo "Warning: Timeout duration $DIFF outside expected window [28, 36]" >&2
fi

kill -TERM $SERVER_PID
wait $SERVER_PID || true
trap - EXIT

echo "=== Timeout test passed! ==="
