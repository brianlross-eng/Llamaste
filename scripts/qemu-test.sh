#!/bin/bash
# qemu-test.sh -- Boot Llamaste in QEMU and run end-to-end integration tests
#
# Usage: ./scripts/qemu-test.sh [images-dir]
#
# This script:
#   1. Boots the Llamaste image in QEMU with virtio networking (port forwarding)
#   2. Waits for the system to come up (polls /health)
#   3. Runs a series of HTTP tests against the running system
#   4. Reports results and exits QEMU
#
# Prerequisites:
#   - qemu-system-x86_64 installed
#   - Built Llamaste image (llamaste.img + bzImage) in images dir
#
# Exit code = number of failed tests (0 = all passed)

set -e

IMAGES_DIR="${1:-$HOME/llamaste-build/output/images}"
IMAGE="${IMAGES_DIR}/llamaste.img"
KERNEL="${IMAGES_DIR}/bzImage"
PORT="${PORT:-8080}"
TIMEOUT="${TIMEOUT:-60}"
LOG="/tmp/qemu-llamaste-$$.log"

# --- Colors ---
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

passed=0
failed=0

pass() { echo -e "  ${GREEN}PASS${NC}: $1"; ((passed++)); }
fail() { echo -e "  ${RED}FAIL${NC}: $1"; ((failed++)); }
info() { echo -e "${BOLD}$1${NC}"; }

# --- Validate prerequisites ---
if ! command -v qemu-system-x86_64 &>/dev/null; then
    echo "ERROR: qemu-system-x86_64 not found. Install QEMU first."
    exit 1
fi

if ! command -v curl &>/dev/null; then
    echo "ERROR: curl not found."
    exit 1
fi

if [ ! -f "${IMAGE}" ]; then
    echo "ERROR: Image not found: ${IMAGE}"
    echo "Build the image first, or specify the images directory:"
    echo "  ./scripts/qemu-test.sh /path/to/images"
    exit 1
fi

if [ ! -f "${KERNEL}" ]; then
    echo "ERROR: Kernel not found: ${KERNEL}"
    echo "Build the image first, or specify the images directory:"
    echo "  ./scripts/qemu-test.sh /path/to/images"
    exit 1
fi

# --- Check if port is available ---
if command -v ss &>/dev/null; then
    if ss -tlnp 2>/dev/null | grep -q ":${PORT} "; then
        echo "ERROR: Port ${PORT} is already in use."
        echo "Set a different port: PORT=9090 ./scripts/qemu-test.sh"
        exit 1
    fi
elif command -v netstat &>/dev/null; then
    if netstat -tlnp 2>/dev/null | grep -q ":${PORT} "; then
        echo "ERROR: Port ${PORT} is already in use."
        echo "Set a different port: PORT=9090 ./scripts/qemu-test.sh"
        exit 1
    fi
fi

info "Llamaste QEMU E2E Test Suite"
echo "============================="
echo "Image:   ${IMAGE}"
echo "Kernel:  ${KERNEL}"
echo "Port:    ${PORT}"
echo "Timeout: ${TIMEOUT}s"
echo "Log:     ${LOG}"
echo ""

# --- Start QEMU in background ---
info "Starting QEMU..."
qemu-system-x86_64 \
    -m 4G -smp 4 \
    -kernel "${KERNEL}" \
    -append "root=/dev/vda3 rootfstype=squashfs ro console=ttyS0 init=/opt/llamaste/llamaste llamaste.mode=server ip=dhcp" \
    -drive file="${IMAGE}",format=raw,if=virtio \
    -netdev user,id=net0,hostfwd=tcp::${PORT}-:80 \
    -device virtio-net-pci,netdev=net0 \
    -nographic \
    -serial mon:stdio \
    &>"${LOG}" &

QEMU_PID=$!

# Cleanup on exit: kill QEMU and remove log
cleanup() {
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        kill "$QEMU_PID" 2>/dev/null
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# Verify QEMU started
sleep 1
if ! kill -0 "$QEMU_PID" 2>/dev/null; then
    echo "ERROR: QEMU failed to start. Log:"
    cat "${LOG}"
    exit 1
fi

# --- Wait for system to come up ---
info "Waiting for system to boot (timeout: ${TIMEOUT}s)..."
boot_start=$(date +%s)
booted=false

for i in $(seq 1 "${TIMEOUT}"); do
    if curl -s --connect-timeout 2 --max-time 3 "http://localhost:${PORT}/health" >/dev/null 2>&1; then
        boot_end=$(date +%s)
        boot_time=$((boot_end - boot_start))
        echo -e "System is up after ${GREEN}${boot_time}s${NC}"
        booted=true
        break
    fi
    # Check QEMU is still running
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "ERROR: QEMU exited prematurely. Log tail:"
        tail -30 "${LOG}"
        exit 1
    fi
    sleep 1
done

if [ "$booted" = false ]; then
    echo -e "${RED}TIMEOUT${NC}: System did not come up in ${TIMEOUT}s"
    echo ""
    echo "--- QEMU log (last 50 lines) ---"
    tail -50 "${LOG}"
    exit 1
fi

echo ""
info "=== Llamaste E2E Tests ==="
echo ""

# --- Test 1: Health check ---
response=$(curl -s "http://localhost:${PORT}/health")
if echo "$response" | grep -q '"status".*"ok"'; then
    pass "Health check returns status:ok"
else
    fail "Health check: $response"
fi

# --- Test 2: Health check has expected fields ---
if echo "$response" | grep -q '"tools_count"'; then
    pass "Health check includes tools_count"
else
    fail "Health check missing tools_count: $response"
fi

# --- Test 3: Web UI loads ---
response=$(curl -s "http://localhost:${PORT}/")
if echo "$response" | grep -qi "llamaste"; then
    pass "Web UI loads (contains 'Llamaste')"
else
    fail "Web UI: response did not contain 'Llamaste'"
fi

# --- Test 4: Web UI is valid HTML ---
if echo "$response" | grep -q "<!DOCTYPE html>"; then
    pass "Web UI returns valid HTML"
else
    fail "Web UI: response does not contain DOCTYPE"
fi

# --- Test 5: Dashboard API ---
response=$(curl -s "http://localhost:${PORT}/llamaste/system")
if echo "$response" | grep -q '"ram_total_mb"'; then
    pass "Dashboard API returns system info (ram_total_mb)"
else
    fail "Dashboard API: $response"
fi

# --- Test 6: Dashboard has expected fields ---
if echo "$response" | grep -q '"cpu_cores"' && \
   echo "$response" | grep -q '"uptime_seconds"' && \
   echo "$response" | grep -q '"disk_total_gb"'; then
    pass "Dashboard API has cpu_cores, uptime_seconds, disk_total_gb"
else
    fail "Dashboard API missing expected fields: $response"
fi

# --- Test 7: Tools API ---
response=$(curl -s "http://localhost:${PORT}/llamaste/tools")
if echo "$response" | grep -q '"function"'; then
    pass "Tools API returns tool definitions"
else
    fail "Tools API: $response"
fi

# --- Test 8: Tools API returns array ---
if echo "$response" | grep -q '^\['; then
    pass "Tools API returns JSON array"
else
    fail "Tools API: response is not a JSON array"
fi

# --- Test 9: Chat API (non-streaming) ---
response=$(curl -s -X POST "http://localhost:${PORT}/llamaste/chat" \
    -H "Content-Type: application/json" \
    -d '{"message":"Hello","stream":false}')
if echo "$response" | grep -q '"content"'; then
    pass "Chat API returns a response with content field"
else
    fail "Chat API: $response"
fi

# --- Test 10: Chat API with conversation ID ---
response=$(curl -s -X POST "http://localhost:${PORT}/llamaste/chat" \
    -H "Content-Type: application/json" \
    -d '{"message":"Test message","stream":false,"conversation_id":"e2e-test-conv"}')
if echo "$response" | grep -q '"conversation_id"'; then
    pass "Chat API returns conversation_id"
else
    fail "Chat API conversation_id: $response"
fi

# --- Test 11: Chat API rejects empty message ---
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://localhost:${PORT}/llamaste/chat" \
    -H "Content-Type: application/json" \
    -d '{"message":"","stream":false}')
if [ "$code" = "400" ]; then
    pass "Chat API rejects empty message (HTTP 400)"
else
    fail "Chat API empty message: expected 400, got ${code}"
fi

# --- Test 12: Chat API rejects invalid JSON ---
code=$(curl -s -o /dev/null -w "%{http_code}" -X POST "http://localhost:${PORT}/llamaste/chat" \
    -H "Content-Type: application/json" \
    -d 'not json at all')
if [ "$code" = "400" ]; then
    pass "Chat API rejects invalid JSON (HTTP 400)"
else
    fail "Chat API invalid JSON: expected 400, got ${code}"
fi

# --- Test 13: OpenAI-compatible API ---
response=$(curl -s -X POST "http://localhost:${PORT}/v1/chat/completions" \
    -H "Content-Type: application/json" \
    -d '{"model":"local","messages":[{"role":"user","content":"Hello"}],"max_tokens":50}')
if echo "$response" | grep -q '"choices"'; then
    pass "OpenAI API returns valid response with choices"
else
    fail "OpenAI API: $response"
fi

# --- Test 14: OpenAI API response has correct structure ---
if echo "$response" | grep -q '"message"' && echo "$response" | grep -q '"assistant"'; then
    pass "OpenAI API response has message with assistant role"
else
    fail "OpenAI API structure: $response"
fi

# --- Test 15-17: Static assets ---
for asset in style.css chat.js dashboard.js; do
    code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${PORT}/${asset}")
    if [ "$code" = "200" ]; then
        pass "Static asset: ${asset} (HTTP 200)"
    else
        fail "Static asset ${asset}: HTTP ${code}"
    fi
done

# --- Test 18: Conversations API ---
response=$(curl -s "http://localhost:${PORT}/llamaste/conversations")
if echo "$response" | grep -q '"conversations"'; then
    pass "Conversations API returns conversations field"
else
    fail "Conversations API: $response"
fi

# --- Test 19: Conversation from earlier test exists ---
if echo "$response" | grep -q "e2e-test-conv"; then
    pass "Conversations API lists e2e-test-conv"
else
    fail "Conversations API: e2e-test-conv not found"
fi

# --- Test 20: Unknown route returns 404 ---
code=$(curl -s -o /dev/null -w "%{http_code}" "http://localhost:${PORT}/nonexistent/route")
if [ "$code" = "404" ]; then
    pass "Unknown route returns HTTP 404"
else
    fail "Unknown route: expected 404, got ${code}"
fi

# --- Results ---
echo ""
echo "============================="
total=$((passed + failed))
if [ "$failed" -eq 0 ]; then
    echo -e "${GREEN}${BOLD}ALL ${total} TESTS PASSED${NC}"
else
    echo -e "${RED}${BOLD}${failed} FAILED${NC}, ${passed} passed out of ${total} total"
fi
echo "============================="

# --- Shutdown QEMU ---
info "Shutting down QEMU..."
kill "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true

exit "$failed"
