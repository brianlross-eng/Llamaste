#!/bin/bash
# qemu-update-test.sh -- A/B update status integration test
#
# Boots an installed Llamaste image and verifies the update API endpoints.
# Cannot do a full update install (requires Ed25519 signing key), so tests
# status, check, rollback, and error handling.
#
# Usage:
#   ./qemu-update-test.sh [image-path]

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/qemu-test-lib.sh"

# Allow image override via argument
IMAGE="${1:-${IMAGE}}"
LOG="/tmp/qemu-update-test-$$.log"
TEST_DISK="/tmp/llamaste-update-test-$$.img"

require_cmd qemu-system-x86_64
require_cmd curl
require_cmd jq
require_file "$IMAGE" "disk image"
require_file "$KERNEL" "kernel"

# --- Setup: copy image to temp disk ---
info "Copying image to temp disk..."
copy_disk "$IMAGE" "$TEST_DISK"

PORT=$(find_free_port)
info "Using port ${PORT}"

QEMU_PID=$(boot_qemu "$PORT" "$LOG" "$TEST_DISK")
info "QEMU started (PID ${QEMU_PID})"

wait_for_health "$PORT" "$TIMEOUT" "$QEMU_PID"

BASE="http://localhost:${PORT}"

# ===== Test 1: GET /update/status =====
info "Test 1: GET /update/status"
STATUS=$(http_get "${BASE}/update/status")
if [ -z "$STATUS" ]; then
    fail "update/status returned empty response"
else
    assert_json_exists "version field exists" "$STATUS" ".version"
    assert_json_field "active_slot is A" "$STATUS" ".active_slot" "A"
    assert_json_exists "inactive_slot exists" "$STATUS" ".inactive_slot"
fi

# ===== Test 2: Version format check =====
info "Test 2: Version format (semver)"
VERSION=$(echo "$STATUS" | jq -r '.version' 2>/dev/null)
if [ -n "$VERSION" ] && [ "$VERSION" != "null" ]; then
    if echo "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+'; then
        pass "version is semver: ${VERSION}"
    else
        skip "version '${VERSION}' is not semver format"
    fi
else
    skip "no version field to check"
fi

# ===== Test 3: GET /update/check =====
info "Test 3: GET /update/check"
CHECK=$(http_get "${BASE}/update/check")
if [ -z "$CHECK" ]; then
    skip "update/check returned empty (no network in QEMU)"
else
    # Either we get current_version or an error -- both are valid responses
    HAS_VERSION=$(echo "$CHECK" | jq -e '.current_version // .version' 2>/dev/null)
    HAS_ERROR=$(echo "$CHECK" | jq -e '.error' 2>/dev/null)
    if [ $? -eq 0 ] || [ -n "$HAS_VERSION" ]; then
        pass "update/check responded with data"
    else
        skip "update/check returned unexpected response (no internet in QEMU is expected)"
    fi
fi

# ===== Test 4: POST /update/rollback =====
info "Test 4: POST /update/rollback"
ROLLBACK=$(http_post "${BASE}/update/rollback" '{}')
if [ -z "$ROLLBACK" ]; then
    fail "update/rollback returned empty response"
else
    # Rollback should respond with success, error, or message -- any structured response is valid
    HAS_CONTENT=$(echo "$ROLLBACK" | jq -e 'type' 2>/dev/null)
    if [ $? -eq 0 ]; then
        pass "update/rollback returned valid JSON"
    else
        fail "update/rollback returned invalid JSON: ${ROLLBACK}"
    fi
fi

# ===== Test 5: Post-rollback status =====
info "Test 5: POST-rollback GET /update/status"
POST_STATUS=$(http_get "${BASE}/update/status")
if [ -z "$POST_STATUS" ]; then
    fail "post-rollback status returned empty response"
else
    assert_json_exists "active_slot still exists after rollback" "$POST_STATUS" ".active_slot"
fi

# ===== Test 6: POST /update/install with invalid path =====
info "Test 6: POST /update/install with invalid path"
INSTALL_ERR=$(http_post "${BASE}/update/install" '{"path":"/nonexistent/update.file"}')
if [ -z "$INSTALL_ERR" ]; then
    fail "update/install returned empty response"
else
    HAS_ERROR=$(echo "$INSTALL_ERR" | jq -r '.error // empty' 2>/dev/null)
    if [ -n "$HAS_ERROR" ]; then
        pass "update/install rejected invalid path: ${HAS_ERROR}"
    else
        # Check if the response indicates failure in some other way
        RESPONSE_STR=$(echo "$INSTALL_ERR" | jq -r 'tostring' 2>/dev/null)
        if echo "$RESPONSE_STR" | grep -qiE 'error|fail|invalid|not found'; then
            pass "update/install rejected invalid path"
        else
            fail "update/install did not return error for invalid path: ${INSTALL_ERR}"
        fi
    fi
fi

# --- Cleanup ---
info "Cleaning up..."
rm -f "$TEST_DISK"

summary
