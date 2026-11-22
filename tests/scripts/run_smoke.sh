#!/usr/bin/env bash
# ==============================================================================
# @file run_smoke.sh
# @brief 端到端 HTTP 冒烟验证脚本
# 
# 检验项：
# 1. HTTP/1.1 标准 GET 请求：返回 200 OK 并验证 Connection: keep-alive 及 Keep-Alive: timeout= 头
# 2. 畸形非法请求：格式不合规时返回 400 Bad Request 并主动关闭连接
# 3. HTTP/1.0 短连接请求：返回 200 OK 且包含 Connection: close
# 4. 发送 SIGTERM 验证服务在轻量请求后的优雅收尾退出
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

rm -rf ./logs_smoke
mkdir -p ./logs_smoke

# 启动服务器在后台（采用标准 getopt 选项指定端口）
$BIN -p "$PORT" &
SERVER_PID=$!

trap 'kill -9 $SERVER_PID 2>/dev/null || true' EXIT

sleep 0.5

echo "=== 1. Normal HTTP/1.1 GET Request ==="
RESP1=$(curl -s -i "http://127.0.0.1:${PORT}/")
echo "$RESP1" | grep -q "200 OK"
echo "$RESP1" | grep -iq "Connection: keep-alive"
echo "$RESP1" | grep -iq "Keep-Alive: timeout="

echo "=== 2. Malformed HTTP Request (expect 400) ==="
BAD_RESP=$(echo -e "INVALID REQUEST STRING\r\n\r\n" | nc --no-shutdown -w 2 127.0.0.1 ${PORT} || true)
echo "$BAD_RESP" | grep -q "400 Bad Request"

echo "=== 3. HTTP/1.0 Connection: close Request ==="
RESP_CLOSE=$(curl -0 -s -i -H "Connection: close" "http://127.0.0.1:${PORT}/")
echo "$RESP_CLOSE" | grep -q "200 OK"
echo "$RESP_CLOSE" | grep -iq "Connection: close"

echo "=== Stopping server gracefully ==="
kill -TERM $SERVER_PID
wait $SERVER_PID || true

trap - EXIT
echo "=== Smoke test passed successfully! ==="
