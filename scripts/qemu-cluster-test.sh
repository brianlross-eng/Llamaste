#!/bin/bash
# qemu-cluster-test.sh -- Multi-node cluster integration test
#
# Boots 2 QEMU instances with different RAM/CPU configs and tests
# the cluster API endpoints on each node.
#
# Node 1: 2G RAM, 2 CPUs (coordinator — higher capacity score)
# Node 2: 1G RAM, 1 CPU  (worker — lower capacity score)
#
# Usage: ./qemu-cluster-test.sh [BUILD_DIR]
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
require_cmd qemu-img
require_cmd curl
require_cmd jq
require_file "$IMAGE" "disk image"
require_file "$KERNEL" "kernel"

# --- Disk copies (each node needs its own) ---
DISK1="/tmp/llamaste-cluster-node1-$$.img"
DISK2="/tmp/llamaste-cluster-node2-$$.img"

cleanup_disks() {
    rm -f "$DISK1" "$DISK2"
}
# Chain our disk cleanup with the library's QEMU cleanup
trap 'cleanup_qemu; cleanup_disks' EXIT

info "=== Llamaste Multi-Node Cluster Test ==="
echo ""

info "Copying disk images..."
copy_disk "$IMAGE" "$DISK1"
copy_disk "$IMAGE" "$DISK2"

# --- Ports ---
PORT1=$(find_free_port)
# Temporarily mark PORT1 as used so find_free_port picks a different one
PORT2=$(PORT1_TAKEN="$PORT1"; port=9090; while [ "$port" -lt 9200 ]; do
    if [ "$port" -eq "$PORT1_TAKEN" ]; then ((port++)); continue; fi
    if ! ss -tlnp 2>/dev/null | grep -q ":${port} " && \
       ! netstat -tlnp 2>/dev/null | grep -q ":${port} "; then
        echo "$port"; break
    fi
    ((port++))
done)

info "Node 1: port=${PORT1}, 2G RAM, 2 CPUs"
info "Node 2: port=${PORT2}, 1G RAM, 1 CPU"
echo ""

# --- Boot Node 1 (coordinator candidate: higher resources) ---
QEMU_MEM="2G" QEMU_SMP="2" boot_qemu "$PORT1" "/tmp/qemu-cluster-node1-$$.log" "$DISK1" "$KERNEL"
PID1="${_qemu_pids[-1]}"

# --- Boot Node 2 (worker candidate: lower resources) ---
QEMU_MEM="1G" QEMU_SMP="1" boot_qemu "$PORT2" "/tmp/qemu-cluster-node2-$$.log" "$DISK2" "$KERNEL"
PID2="${_qemu_pids[-1]}"

# --- Wait for both nodes ---
info "Waiting for Node 1..."
if ! wait_for_health "$PORT1" 90 "$PID1"; then
    echo "Node 1 failed to start"
    echo "--- Node 1 log (last 30 lines) ---"
    tail -30 "/tmp/qemu-cluster-node1-$$.log"
    exit 1
fi

info "Waiting for Node 2..."
if ! wait_for_health "$PORT2" 90 "$PID2"; then
    echo "Node 2 failed to start"
    echo "--- Node 2 log (last 30 lines) ---"
    tail -30 "/tmp/qemu-cluster-node2-$$.log"
    exit 1
fi

echo ""
info "=== Both nodes online. Running cluster tests ==="
echo ""

# -------------------------------------------------------
# Test 1: Both nodes start as STANDALONE
# -------------------------------------------------------
info "Test 1: Initial cluster status (both STANDALONE)"

STATUS1=$(http_get "http://localhost:${PORT1}/cluster/status")
STATUS2=$(http_get "http://localhost:${PORT2}/cluster/status")

assert_json_field "Node 1 role=STANDALONE" "$STATUS1" ".role" "STANDALONE"
assert_json_field "Node 2 role=STANDALONE" "$STATUS2" ".role" "STANDALONE"

# -------------------------------------------------------
# Test 2: Add-peer API accepts request
# -------------------------------------------------------
info "Test 2: POST /cluster/add-peer (API contract)"

# QEMU user-mode NAT means nodes can't actually reach each other,
# so we just verify the API accepts the request and returns JSON.
ADD_RESP=$(http_post "http://localhost:${PORT1}/cluster/add-peer" \
    '{"ip":"10.0.2.100","port":80}')

if echo "$ADD_RESP" | jq -e '.' >/dev/null 2>&1; then
    pass "add-peer returns valid JSON"
else
    fail "add-peer did not return valid JSON: ${ADD_RESP}"
fi

# -------------------------------------------------------
# Test 3: Cluster status has role and self fields
# -------------------------------------------------------
info "Test 3: Cluster status structure"

assert_json_exists "Node 1 has .role"  "$STATUS1" ".role"
assert_json_exists "Node 1 has .self"  "$STATUS1" ".self"
assert_json_exists "Node 2 has .role"  "$STATUS2" ".role"
assert_json_exists "Node 2 has .self"  "$STATUS2" ".self"

# -------------------------------------------------------
# Test 4: GET /cluster/capacity
# -------------------------------------------------------
info "Test 4: GET /cluster/capacity"

CAP1=$(http_get "http://localhost:${PORT1}/cluster/capacity")
CAP2=$(http_get "http://localhost:${PORT2}/cluster/capacity")

assert_json_exists "Node 1 total_ram_mb"  "$CAP1" ".total_ram_mb"
assert_json_exists "Node 1 usable_ram_mb" "$CAP1" ".usable_ram_mb"
assert_json_exists "Node 1 total_cores"   "$CAP1" ".total_cores"
assert_json_exists "Node 1 node_count"    "$CAP1" ".node_count"

assert_json_exists "Node 2 total_ram_mb"  "$CAP2" ".total_ram_mb"
assert_json_exists "Node 2 usable_ram_mb" "$CAP2" ".usable_ram_mb"
assert_json_exists "Node 2 total_cores"   "$CAP2" ".total_cores"
assert_json_exists "Node 2 node_count"    "$CAP2" ".node_count"

# Node 1 (2G) should report more RAM than Node 2 (1G)
assert_json_gt "Node 1 RAM > 1500MB" "$CAP1" ".total_ram_mb" 1500
assert_json_gt "Node 2 RAM > 500MB"  "$CAP2" ".total_ram_mb" 500

# -------------------------------------------------------
# Test 5: GET /cluster/models
# -------------------------------------------------------
info "Test 5: GET /cluster/models"

MODELS1=$(http_get "http://localhost:${PORT1}/cluster/models")
MODELS2=$(http_get "http://localhost:${PORT2}/cluster/models")

assert_json_exists "Node 1 usable_ram_mb" "$MODELS1" ".usable_ram_mb"
assert_json_exists "Node 2 usable_ram_mb" "$MODELS2" ".usable_ram_mb"

# Verify tiers array exists and has entries
TIERS1=$(echo "$MODELS1" | jq '.tiers | length' 2>/dev/null || echo "0")
if [ "$TIERS1" -gt 0 ]; then
    pass "Node 1 tiers array has ${TIERS1} entries"
else
    fail "Node 1 tiers array is empty or missing"
fi

TIERS2=$(echo "$MODELS2" | jq '.tiers | length' 2>/dev/null || echo "0")
if [ "$TIERS2" -gt 0 ]; then
    pass "Node 2 tiers array has ${TIERS2} entries"
else
    fail "Node 2 tiers array is empty or missing"
fi

# -------------------------------------------------------
# Test 6: GET /cluster/peers returns JSON array
# -------------------------------------------------------
info "Test 6: GET /cluster/peers"

PEERS1=$(http_get "http://localhost:${PORT1}/cluster/peers")
PEERS2=$(http_get "http://localhost:${PORT2}/cluster/peers")

if echo "$PEERS1" | jq -e 'type == "array"' >/dev/null 2>&1; then
    pass "Node 1 /cluster/peers returns array"
else
    fail "Node 1 /cluster/peers not an array: ${PEERS1}"
fi

if echo "$PEERS2" | jq -e 'type == "array"' >/dev/null 2>&1; then
    pass "Node 2 /cluster/peers returns array"
else
    fail "Node 2 /cluster/peers not an array: ${PEERS2}"
fi

# -------------------------------------------------------
# Test 7: POST /cluster/reload returns new_role
# -------------------------------------------------------
info "Test 7: POST /cluster/reload"

RELOAD1=$(http_post "http://localhost:${PORT1}/cluster/reload" '{}')
RELOAD2=$(http_post "http://localhost:${PORT2}/cluster/reload" '{}')

assert_json_exists "Node 1 reload has new_role" "$RELOAD1" ".new_role"
assert_json_exists "Node 2 reload has new_role" "$RELOAD2" ".new_role"

# -------------------------------------------------------
# Summary
# -------------------------------------------------------
echo ""
info "Cleaning up QEMU instances..."
# cleanup_qemu runs via EXIT trap, but we call it explicitly for clean output
cleanup_qemu
cleanup_disks
# Prevent double-cleanup in trap
trap - EXIT

summary
