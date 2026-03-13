#!/bin/bash
# qemu-install-test-v2.sh -- Install flow integration test with assertions
#
# Boots Llamaste ISO with a blank target disk, runs the installer via HTTP API,
# then reboots from the installed disk and verifies the system comes up.
#
# Usage: ./qemu-install-test-v2.sh [/path/to/llamaste.iso]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/qemu-test-lib.sh"

# --- Config ---
ISO="${1:-${ISO}}"
TARGET="/tmp/llamaste-install-target-$$.img"
LOG_ISO="/tmp/qemu-install-iso-$$.log"
LOG_DISK="/tmp/qemu-install-disk-$$.log"
INSTALL_TIMEOUT=240

# --- Prerequisites ---
require_cmd qemu-system-x86_64
require_cmd jq
require_file "$ISO" "ISO image"

echo ""
echo "============================================"
echo "  Llamaste Install Flow Test (v2)"
echo "============================================"
echo ""

# =============================================
# Phase 1: Boot ISO, install to target disk
# =============================================
info "=== Phase 1: Boot ISO and install ==="

# 1. Create blank target disk
info "Creating 4G target disk: ${TARGET}"
create_raw_disk "$TARGET" 4G

# 2. Boot ISO with target disk attached
PORT=$(find_free_port)
info "Using port ${PORT} for ISO boot"
ISO_PID=$(boot_qemu_iso "$PORT" "$LOG_ISO" "$ISO" "$TARGET")
info "ISO QEMU started (PID ${ISO_PID})"

# 3. Wait for health
if ! wait_for_health "$PORT" 90 "$ISO_PID"; then
    fail "ISO boot: health endpoint never responded"
    cat "$LOG_ISO" | tail -30
    summary
    exit 1
fi
pass "ISO boot: system healthy"

# 4. Detect disks
info "Detecting install target disks..."
DISKS_JSON=$(http_get "http://localhost:${PORT}/install/disks")
assert_json_exists "install/disks returns data" "$DISKS_JSON" ".disks"

# Extract the target device name (virtio disk = /dev/vda)
TARGET_DEV=$(echo "$DISKS_JSON" | jq -r '.disks[0].device // empty' 2>/dev/null)
if [ -z "$TARGET_DEV" ]; then
    # Fallback: try .devices array or default to /dev/vda
    TARGET_DEV=$(echo "$DISKS_JSON" | jq -r '.devices[0].path // .devices[0].device // empty' 2>/dev/null)
fi
if [ -z "$TARGET_DEV" ]; then
    TARGET_DEV="/dev/vda"
    info "No device found in JSON, defaulting to ${TARGET_DEV}"
fi
info "Target device: ${TARGET_DEV}"

# 5. Start installation
info "Starting installation to ${TARGET_DEV}..."
START_JSON=$(http_post "http://localhost:${PORT}/install/start" \
    "{\"device\":\"${TARGET_DEV}\",\"confirm\":true}")
assert_json_field "install/start returns started=true" "$START_JSON" ".started" "true"

# 6. Poll progress until finished
info "Polling install progress (timeout: ${INSTALL_TIMEOUT}s)..."
elapsed=0
INSTALL_SUCCESS="false"
while [ "$elapsed" -lt "$INSTALL_TIMEOUT" ]; do
    sleep 2
    elapsed=$((elapsed + 2))

    PROGRESS_JSON=$(http_get "http://localhost:${PORT}/install/progress")
    FINISHED=$(echo "$PROGRESS_JSON" | jq -r '.finished // false' 2>/dev/null)
    PCT=$(echo "$PROGRESS_JSON" | jq -r '.percent // 0' 2>/dev/null)
    STATUS_MSG=$(echo "$PROGRESS_JSON" | jq -r '.status // "unknown"' 2>/dev/null)

    echo -e "  [${elapsed}s] ${PCT}% - ${STATUS_MSG} (finished=${FINISHED})"

    if [ "$FINISHED" = "true" ]; then
        INSTALL_SUCCESS=$(echo "$PROGRESS_JSON" | jq -r '.success // false' 2>/dev/null)
        break
    fi

    # Check QEMU still alive
    if ! kill -0 "$ISO_PID" 2>/dev/null; then
        fail "QEMU exited during installation"
        break
    fi
done

if [ "$FINISHED" != "true" ]; then
    fail "Installation did not finish within ${INSTALL_TIMEOUT}s"
    summary
    exit 1
fi

# 7. Assert success
assert_json_field "install/progress finished=true" "$PROGRESS_JSON" ".finished" "true"
assert_json_field "install/progress success=true" "$PROGRESS_JSON" ".success" "true"
pass "Installation completed successfully"

# 8. Kill ISO QEMU
info "Stopping ISO QEMU..."
kill "$ISO_PID" 2>/dev/null
wait "$ISO_PID" 2>/dev/null || true

echo ""

# =============================================
# Phase 2: Boot from installed disk
# =============================================
info "=== Phase 2: Boot from installed disk ==="

PORT2=$(find_free_port)
info "Using port ${PORT2} for installed disk boot"
DISK_PID=$(boot_qemu "$PORT2" "$LOG_DISK" "$TARGET" "$KERNEL")
info "Disk QEMU started (PID ${DISK_PID})"

# 2. Wait for health
if ! wait_for_health "$PORT2" 90 "$DISK_PID"; then
    fail "Installed disk boot: health endpoint never responded"
    tail -30 "$LOG_DISK"
    summary
    exit 1
fi
pass "Installed disk boot: system healthy"

# 3. Assert health status
HEALTH_JSON=$(http_get "http://localhost:${PORT2}/health")
assert_json_field "health status=ok" "$HEALTH_JSON" ".status" "ok"

# 4. Check active slot
UPDATE_JSON=$(http_get "http://localhost:${PORT2}/update/status")
assert_json_field "update/status active_slot=A" "$UPDATE_JSON" ".active_slot" "A"

# 5. Check system info
SYSTEM_JSON=$(http_get "http://localhost:${PORT2}/llamaste/system")
assert_json_exists "system ram_total_mb exists" "$SYSTEM_JSON" ".ram_total_mb"

# 6. Cleanup
info "Cleaning up..."
kill "$DISK_PID" 2>/dev/null
wait "$DISK_PID" 2>/dev/null || true
rm -f "$TARGET" "$LOG_ISO" "$LOG_DISK"

echo ""
summary
