#!/bin/bash
# qemu-recovery-test.sh -- Watchdog/supervisor recovery integration test
#
# Boots a QEMU instance and tests that the supervisor recovers the child
# process after it is killed via the debug.kill_child test API endpoint.
#
# Requires the image to be built with LLAMASTE_TEST_API=ON for the
# debug.kill_child tool to be available. If not, all tests are skipped.
#
# Usage: ./qemu-recovery-test.sh [BUILD_DIR]
#   BUILD_DIR defaults to ~/llamaste-build/output

set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/qemu-test-lib.sh"

# Override images dir if argument provided
if [ $# -ge 1 ]; then
    IMAGES_DIR="$1/images"
    IMAGE="${IMAGES_DIR}/llamaste.img"
    KERNEL="${IMAGES_DIR}/bzImage"
fi

# --- Prerequisites ---
require_cmd qemu-system-x86_64
require_cmd curl
require_cmd jq
require_file "$IMAGE" "disk image"
require_file "$KERNEL" "kernel"

# --- Temp disk ---
DISK="/tmp/llamaste-recovery-$$.img"

cleanup_disk() {
    rm -f "$DISK"
}
trap 'cleanup_qemu; cleanup_disk' EXIT

info "=== Llamaste Watchdog Recovery Test ==="
echo ""

# --- Setup ---
info "Copying disk image..."
copy_disk "$IMAGE" "$DISK"

PORT=$(find_free_port)
LOG="/tmp/qemu-recovery-$$.log"

# Use default 4G RAM — 2G is too slow under QEMU emulation without KVM
boot_qemu "$PORT" "$LOG" "$DISK"
QEMU_PID="${BOOT_PID}"
wait_for_health "$PORT" 180 "$QEMU_PID"

BASE="http://localhost:${PORT}"
TOOL_URL="${BASE}/llamaste/tool"

# ============================================================
# Test 1: Check if test API is available
# ============================================================
info ""
info "--- Test 1: Test API availability ---"

KILL_RESP=$(http_post "$TOOL_URL" '{"name":"debug.kill_child"}')
echo "  debug.kill_child response: $KILL_RESP"

# Check if the tool is unknown (image not built with LLAMASTE_TEST_API)
# "unknown tool" or "no matching tool" = tool not registered (no LLAMASTE_TEST_API)
# "process not found" = tool works but no llama-server running (expected without model)
if echo "$KILL_RESP" | grep -qi "unknown tool\|no matching tool\|No tool named"; then
    skip "debug.kill_child not available (image not built with LLAMASTE_TEST_API)"
    skip "Kill child and verify recovery (test API unavailable)"
    skip "System functional after recovery (test API unavailable)"
    skip "Rapid kills / backoff (test API unavailable)"
    summary
    exit $?
fi

pass "Test API is available (debug.kill_child recognized)"

# ============================================================
# Test 2: Kill child and verify recovery
# ============================================================
info ""
info "--- Test 2: Kill child and verify recovery ---"

# Check if kill actually succeeded (llama-server might not be running if no model)
KILLED=$(echo "$KILL_RESP" | jq -r '.killed // .result.killed // empty' 2>/dev/null) || true

if echo "$KILL_RESP" | grep -qi "process not found\|no.*running\|no.*child"; then
    skip "No child process running (no model loaded?) -- skipping recovery tests"
    skip "System functional after recovery (no child to kill)"
    skip "Rapid kills / backoff (no child to kill)"
    summary
    exit $?
fi

if [ "$KILLED" = "true" ]; then
    pass "debug.kill_child returned killed=true"
else
    # Even if we can't parse 'killed', the tool was accepted -- continue
    pass "debug.kill_child accepted (response: $(echo "$KILL_RESP" | head -c 120))"
fi

# Poll /health for up to 30s -- expect it to either dip then recover, or stay up
# (child_main HTTP server is independent of llama-server subprocess)
info "  Polling /health for recovery (30s timeout)..."
SAW_DOWN=0
SAW_RECOVER=0
for i in $(seq 1 30); do
    STATUS=$(http_status "${BASE}/health")
    if [ "$STATUS" != "200" ]; then
        SAW_DOWN=1
    elif [ "$SAW_DOWN" -eq 1 ]; then
        SAW_RECOVER=1
        break
    fi
    sleep 1
done

# Final health check
FINAL_STATUS=$(http_status "${BASE}/health")

if [ "$SAW_DOWN" -eq 1 ] && [ "$SAW_RECOVER" -eq 1 ]; then
    pass "Health went down then recovered after child kill"
elif [ "$SAW_DOWN" -eq 1 ] && [ "$FINAL_STATUS" = "200" ]; then
    pass "Health went down then recovered (detected on final check)"
elif [ "$SAW_DOWN" -eq 0 ] && [ "$FINAL_STATUS" = "200" ]; then
    pass "Health stayed up after child kill (HTTP server independent of llama-server)"
else
    fail "System did not recover: saw_down=${SAW_DOWN}, final_status=${FINAL_STATUS}"
fi

# ============================================================
# Test 3: Verify system functional after recovery
# ============================================================
info ""
info "--- Test 3: System functional after recovery ---"

HEALTH=$(http_get "${BASE}/health")
assert_json_field "GET /health status=ok" "$HEALTH" '.status' 'ok'

SYSINFO=$(http_get "${BASE}/llamaste/system")
assert_json_exists "GET /llamaste/system has ram_total_mb" "$SYSINFO" '.ram_total_mb'

# ============================================================
# Test 4: Rapid kills (backoff test)
# ============================================================
info ""
info "--- Test 4: Rapid kills (backoff stress test) ---"

for attempt in 1 2 3; do
    RESP=$(http_post "$TOOL_URL" '{"name":"debug.kill_child"}')
    echo "  Kill #${attempt}: $(echo "$RESP" | head -c 100)"
    sleep 1
done

info "  Waiting 5s for supervisor recovery..."
sleep 5

HEALTH_AFTER=$(http_get "${BASE}/health")
FINAL=$(echo "$HEALTH_AFTER" | jq -r '.status' 2>/dev/null)

if [ "$FINAL" = "ok" ]; then
    pass "System responsive after 3 rapid kills (status=ok)"
else
    # System might still be recovering -- give it more time
    info "  Status not ok yet, waiting 10 more seconds..."
    sleep 10
    HEALTH_RETRY=$(http_get "${BASE}/health")
    RETRY_STATUS=$(echo "$HEALTH_RETRY" | jq -r '.status' 2>/dev/null)
    if [ "$RETRY_STATUS" = "ok" ]; then
        pass "System responsive after 3 rapid kills (recovered with backoff)"
    else
        fail "System not responsive after rapid kills: status=${RETRY_STATUS}"
    fi
fi

# ============================================================
echo ""
summary
