#!/bin/bash
# qemu-model-test.sh -- Model download API integration test
#
# Tests model listing, recommendation, current model, USB scan,
# and optionally triggers a model download.
#
# Usage: ./scripts/qemu-model-test.sh [images-dir] [--download]
#   images-dir  defaults to ~/llamaste-build/output/images
#   --download  enable the actual model download test (slow, up to 10min)

set -euo pipefail
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/qemu-test-lib.sh"

# --- Argument parsing ---
DO_DOWNLOAD=false

for arg in "$@"; do
    case "$arg" in
        --download)
            DO_DOWNLOAD=true
            ;;
        *)
            IMAGES_DIR="$arg"
            IMAGE="${IMAGES_DIR}/llamaste.img"
            KERNEL="${IMAGES_DIR}/bzImage"
            ;;
    esac
done

# --- Prerequisites ---
require_cmd qemu-system-x86_64
require_cmd qemu-img
require_cmd curl
require_cmd jq
require_file "$IMAGE" "disk image"
require_file "$KERNEL" "kernel"

# --- Temp disk ---
DISK="/tmp/llamaste-model-test-$$.img"

cleanup_disk() {
    rm -f "$DISK"
}
trap 'cleanup_qemu; cleanup_disk' EXIT

info "=== Llamaste Model Download API Test ==="
echo ""

# --- Setup ---
info "Copying disk image..."
copy_disk "$IMAGE" "$DISK"

PORT=$(find_free_port)
QEMU_MEM="2G"
QEMU_SMP="2"
export QEMU_MEM QEMU_SMP

LOG="/tmp/qemu-model-test-$$.log"
boot_qemu "$PORT" "$LOG" "$DISK"
QEMU_PID="${BOOT_PID}"
wait_for_health "$PORT" 120 "$QEMU_PID"

BASE="http://localhost:${PORT}"

# ===== Test 1: GET /model/list =====
info "Test 1: GET /model/list"
MODEL_LIST=$(http_get "${BASE}/llamaste/model/list")
if [ -n "$MODEL_LIST" ] && echo "$MODEL_LIST" | jq -e '.' >/dev/null 2>&1; then
    pass "/model/list responds with valid JSON"
else
    fail "/model/list did not return valid JSON"
fi

# ===== Test 2: GET /model/recommended =====
info "Test 2: GET /model/recommended"
RECOMMENDED=$(http_get "${BASE}/llamaste/model/recommended")
if [ -z "$RECOMMENDED" ] || ! echo "$RECOMMENDED" | jq -e '.' >/dev/null 2>&1; then
    fail "/model/recommended did not return valid JSON"
else
    assert_json_exists "/model/recommended has ram_available_mb" "$RECOMMENDED" ".ram_available_mb"
    assert_json_exists "/model/recommended has recommended field" "$RECOMMENDED" ".recommended"
fi

# ===== Test 3: Recommended model details =====
info "Test 3: Recommended model details"
IS_RECOMMENDED=$(echo "$RECOMMENDED" | jq -r '.recommended' 2>/dev/null)
MODEL_NAME=""
MODEL_FILENAME=""
MODEL_REPO=""

if [ "$IS_RECOMMENDED" = "true" ]; then
    MODEL_NAME=$(echo "$RECOMMENDED" | jq -r '.model_name // empty' 2>/dev/null)
    MODEL_FILENAME=$(echo "$RECOMMENDED" | jq -r '.filename // empty' 2>/dev/null)
    MODEL_REPO=$(echo "$RECOMMENDED" | jq -r '.repo_id // empty' 2>/dev/null)

    if [ -n "$MODEL_NAME" ]; then
        pass "recommended model_name present: ${MODEL_NAME}"
    else
        fail "recommended=true but model_name missing"
    fi

    if [ -n "$MODEL_FILENAME" ]; then
        pass "recommended filename present: ${MODEL_FILENAME}"
    else
        fail "recommended=true but filename missing"
    fi

    if [ -n "$MODEL_REPO" ]; then
        pass "recommended repo_id present: ${MODEL_REPO}"
    else
        fail "recommended=true but repo_id missing"
    fi

    assert_json_exists "recommended has download_url" "$RECOMMENDED" ".download_url"
    assert_json_exists "recommended has approx_download_mb" "$RECOMMENDED" ".approx_download_mb"
else
    skip "no model recommended (recommended=${IS_RECOMMENDED}), skipping detail checks"
fi

# ===== Test 4: GET /model/current =====
info "Test 4: GET /model/current"
CURRENT=$(http_get "${BASE}/llamaste/model/current")
if [ -n "$CURRENT" ] && echo "$CURRENT" | jq -e '.' >/dev/null 2>&1; then
    pass "/model/current responds with valid JSON"
else
    fail "/model/current did not return valid JSON"
fi

# ===== Test 5: GET /model/usb/scan =====
info "Test 5: GET /model/usb/scan"
USB_STATUS=$(http_status "${BASE}/llamaste/model/usb/scan")
if [ "$USB_STATUS" = "200" ]; then
    USB_SCAN=$(http_get "${BASE}/llamaste/model/usb/scan")
    if echo "$USB_SCAN" | jq -e '.' >/dev/null 2>&1; then
        pass "/model/usb/scan responds with valid JSON"
    else
        fail "/model/usb/scan returned status 200 but invalid JSON"
    fi
elif [ "$USB_STATUS" = "404" ] || [ "$USB_STATUS" = "501" ]; then
    skip "/model/usb/scan not available (HTTP ${USB_STATUS})"
else
    fail "/model/usb/scan unexpected status: ${USB_STATUS}"
fi

# ===== Test 6: Optional model download =====
info "Test 6: Model download"
if [ "$DO_DOWNLOAD" != "true" ]; then
    skip "model download (use --download flag to enable)"
elif [ "$IS_RECOMMENDED" != "true" ]; then
    skip "model download (no model recommended)"
else
    info "Triggering download of ${MODEL_FILENAME} from ${MODEL_REPO}..."
    DL_BODY=$(jq -n --arg repo "$MODEL_REPO" --arg file "$MODEL_FILENAME" \
        '{repo_id: $repo, filename: $file}')
    DL_RESP=$(http_post "${BASE}/llamaste/model/download" "$DL_BODY")

    if [ -n "$DL_RESP" ] && echo "$DL_RESP" | jq -e '.' >/dev/null 2>&1; then
        pass "POST /model/download accepted"
    else
        fail "POST /model/download did not return valid JSON"
    fi

    # Poll /model/list until a model appears (up to 600s)
    info "Polling /model/list for download completion (timeout: 600s)..."
    DL_TIMEOUT=600
    DL_START=$(date +%s)
    DL_DONE=false

    while true; do
        ELAPSED=$(( $(date +%s) - DL_START ))
        if [ "$ELAPSED" -ge "$DL_TIMEOUT" ]; then
            break
        fi

        LIST_NOW=$(http_get "${BASE}/llamaste/model/list")
        COUNT=0
        if [ -n "$LIST_NOW" ] && echo "$LIST_NOW" | jq -e '.' >/dev/null 2>&1; then
            # Handle both array and object with array field
            if echo "$LIST_NOW" | jq -e 'type == "array"' >/dev/null 2>&1; then
                COUNT=$(echo "$LIST_NOW" | jq 'length' 2>/dev/null)
            else
                COUNT=$(echo "$LIST_NOW" | jq '[.[] | arrays | length] | add // 0' 2>/dev/null)
            fi
        fi

        if [ "$COUNT" -gt 0 ]; then
            DL_DONE=true
            break
        fi

        sleep 2
    done

    if [ "$DL_DONE" = "true" ]; then
        pass "model download completed (${ELAPSED}s, ${COUNT} model(s) available)"
    else
        fail "model download timed out after ${DL_TIMEOUT}s"
    fi
fi

# --- Summary ---
echo ""
summary
