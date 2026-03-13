#!/bin/bash
# qemu-test-lib.sh -- Shared library for Llamaste QEMU integration tests
#
# Source this file from test scripts:
#   SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
#   source "${SCRIPT_DIR}/qemu-test-lib.sh"
#
# Provides:
#   Colors, counters, info/pass/fail/skip
#   require_cmd, require_file
#   find_free_port
#   boot_qemu, boot_qemu_iso
#   wait_for_health
#   http_get, http_post, http_status
#   assert_json_field, assert_json_exists, assert_json_gt
#   summary
#   create_raw_disk, copy_disk
#   cleanup_qemu (EXIT trap)

set -euo pipefail

# --- Defaults (override before sourcing or via env) ---
IMAGES_DIR="${IMAGES_DIR:-$HOME/llamaste-build/output/images}"
IMAGE="${IMAGE:-${IMAGES_DIR}/llamaste.img}"
KERNEL="${KERNEL:-${IMAGES_DIR}/bzImage}"
ISO="${ISO:-${IMAGES_DIR}/llamaste.iso}"
TIMEOUT="${TIMEOUT:-180}"
QEMU_MEM="${QEMU_MEM:-4G}"
QEMU_SMP="${QEMU_SMP:-4}"

# --- Colors ---
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

# --- Counters ---
_passed=0
_failed=0
_skipped=0

# --- QEMU PID tracking ---
declare -a _qemu_pids=()
BOOT_PID=""

# ===== Logging =====

info() { echo -e "${BOLD}$*${NC}"; }

pass() {
    echo -e "  ${GREEN}PASS${NC}: $1"
    ((_passed++)) || true
}

fail() {
    echo -e "  ${RED}FAIL${NC}: $1"
    ((_failed++)) || true
}

skip() {
    echo -e "  ${YELLOW}SKIP${NC}: $1"
    ((_skipped++)) || true
}

# ===== Prerequisites =====

require_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" &>/dev/null; then
        echo "ERROR: required command '${cmd}' not found"
        exit 1
    fi
}

require_file() {
    local path="$1"
    local label="${2:-$1}"
    if [ ! -f "$path" ]; then
        echo "ERROR: required file not found: ${label}"
        echo "  path: ${path}"
        exit 1
    fi
}

# ===== Port Management =====

# QEMU_PORT_BASE can be set per-test to avoid port reuse across sequential tests.
# Each test gets its own range (e.g., 9100, 9120, 9140) so TIME_WAIT ports from
# previous tests never collide with the current test.
QEMU_PORT_BASE="${QEMU_PORT_BASE:-9090}"

find_free_port() {
    local port="${QEMU_PORT_BASE}"
    local max=$((port + 20))
    while [ "$port" -lt "$max" ]; do
        # Check ALL TCP states (not just LISTEN) to avoid TIME_WAIT conflicts
        if ! ss -tanp 2>/dev/null | grep -q ":${port} " && \
           ! netstat -tanp 2>/dev/null | grep -q ":${port} "; then
            echo "$port"
            return 0
        fi
        ((port++))
    done
    echo "ERROR: no free port found in range ${QEMU_PORT_BASE}-$((max-1))" >&2
    return 1
}

# ===== Disk Helpers =====

create_raw_disk() {
    local path="$1"
    local size="${2:-4G}"
    qemu-img create -f raw "$path" "$size" >/dev/null 2>&1
}

copy_disk() {
    local src="$1"
    local dst="$2"
    cp "$src" "$dst"
}

# ===== QEMU Boot =====

boot_qemu() {
    local port="$1"
    local log="${2:-/tmp/qemu-test-$$.log}"
    local image="${3:-${IMAGE}}"
    local kernel="${4:-${KERNEL}}"

    require_cmd qemu-system-x86_64
    require_file "$image" "disk image"
    require_file "$kernel" "kernel"

    info "Starting QEMU (kernel direct boot, port ${port})..."
    qemu-system-x86_64 \
        -m "${QEMU_MEM}" -smp "${QEMU_SMP}" \
        -kernel "${kernel}" \
        -append "root=/dev/vda3 rootfstype=squashfs ro console=ttyS0 init=/opt/llamaste/llamaste llamaste.mode=server ip=dhcp" \
        -drive file="${image}",format=raw,if=virtio \
        -netdev user,id=net0,hostfwd=tcp::${port}-:80 \
        -device virtio-net-pci,netdev=net0 \
        -nographic \
        -no-reboot \
        &>"${log}" &

    local pid=$!
    _qemu_pids+=("$pid")
    BOOT_PID="$pid"

    # Verify QEMU started
    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "ERROR: QEMU failed to start. Log:"
        tail -20 "${log}"
        return 1
    fi

    # NOTE: Do NOT echo $pid here. Callers that use $(boot_qemu ...) create
    # a subshell; when that subshell exits, SIGHUP kills QEMU. Instead,
    # callers should read BOOT_PID or _qemu_pids[-1] after calling boot_qemu.
}

boot_qemu_iso() {
    local port="$1"
    local log="${2:-/tmp/qemu-iso-test-$$.log}"
    local iso="${3:-${ISO}}"
    local target_disk="${4:-}"
    local kernel="${5:-}"

    require_cmd qemu-system-x86_64
    require_file "$iso" "ISO image"

    local drive_args="-cdrom ${iso}"
    if [ -n "$target_disk" ]; then
        drive_args="${drive_args} -drive file=${target_disk},format=raw,if=virtio"
    fi

    # Direct kernel boot bypasses GRUB (saves 10s timeout) and avoids the
    # LABEL= root resolution issue when the embedded PXE initramfs is present.
    # The initramfs /init script handles root=/dev/sr0 in its PATH A branch.
    if [ -n "$kernel" ] && [ -f "$kernel" ]; then
        info "Starting QEMU (ISO + direct kernel boot, port ${port})..."
        # shellcheck disable=SC2086
        qemu-system-x86_64 \
            -m "${QEMU_MEM}" -smp "${QEMU_SMP}" \
            -kernel "${kernel}" \
            -append "root=/dev/sr0 rootfstype=iso9660 ro console=ttyS0 init=/opt/llamaste/llamaste llamaste.mode=live rootwait ip=dhcp" \
            ${drive_args} \
            -netdev user,id=net0,hostfwd=tcp::${port}-:80 \
            -device virtio-net-pci,netdev=net0 \
            -nographic \
            -no-reboot \
            &>"${log}" &
    else
        info "Starting QEMU (ISO boot via GRUB, port ${port})..."
        # shellcheck disable=SC2086
        qemu-system-x86_64 \
            -m "${QEMU_MEM}" -smp "${QEMU_SMP}" \
            ${drive_args} \
            -netdev user,id=net0,hostfwd=tcp::${port}-:80 \
            -device virtio-net-pci,netdev=net0 \
            -nographic \
            -no-reboot \
            &>"${log}" &
    fi

    local pid=$!
    _qemu_pids+=("$pid")
    BOOT_PID="$pid"

    sleep 1
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "ERROR: QEMU (ISO) failed to start. Log:"
        tail -20 "${log}"
        return 1
    fi

    # NOTE: Do NOT echo $pid here — see boot_qemu comment above.
}

# ===== Health Polling =====

wait_for_health() {
    local port="$1"
    local timeout="${2:-${TIMEOUT}}"
    local qemu_pid="${3:-}"

    info "Waiting for health (timeout: ${timeout}s)..."
    local start
    start=$(date +%s)

    for i in $(seq 1 "$timeout"); do
        if curl -s --connect-timeout 2 --max-time 3 \
            "http://localhost:${port}/health" >/dev/null 2>&1; then
            local elapsed=$(( $(date +%s) - start ))
            echo -e "  System up after ${GREEN}${elapsed}s${NC}"
            return 0
        fi
        # If we have a PID, check QEMU is still alive
        if [ -n "$qemu_pid" ] && ! kill -0 "$qemu_pid" 2>/dev/null; then
            echo "ERROR: QEMU exited prematurely"
            return 1
        fi
        sleep 1
    done

    echo -e "  ${RED}TIMEOUT${NC}: system did not respond in ${timeout}s"
    return 1
}

# ===== HTTP Helpers =====

http_get() {
    local url="$1"
    curl -s --connect-timeout 5 --max-time 10 "$url" 2>/dev/null
}

http_post() {
    local url="$1"
    local data="$2"
    curl -s --connect-timeout 5 --max-time 30 \
        -X POST "$url" \
        -H "Content-Type: application/json" \
        -d "$data" 2>/dev/null
}

http_status() {
    local url="$1"
    curl -s -o /dev/null -w "%{http_code}" \
        --connect-timeout 5 --max-time 10 "$url" 2>/dev/null || echo "000"
}

# ===== JSON Assertions (require jq) =====

assert_json_field() {
    local label="$1"
    local json="$2"
    local field="$3"
    local expected="$4"

    local actual
    actual=$(echo "$json" | jq -r "$field" 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        pass "$label"
    else
        fail "${label}: expected '${expected}', got '${actual}'"
    fi
}

assert_json_exists() {
    local label="$1"
    local json="$2"
    local field="$3"

    local val
    val=$(echo "$json" | jq -e "$field" 2>/dev/null) || true
    if [ -n "$val" ] && [ "$val" != "null" ]; then
        pass "$label"
    else
        fail "${label}: field '${field}' missing or null"
    fi
}

assert_json_gt() {
    local label="$1"
    local json="$2"
    local field="$3"
    local threshold="$4"

    local val
    val=$(echo "$json" | jq -r "$field" 2>/dev/null)
    if [ -n "$val" ] && [ "$val" != "null" ] && \
       awk "BEGIN{exit(!($val > $threshold))}"; then
        pass "$label (${val} > ${threshold})"
    else
        fail "${label}: '${field}' = '${val}', expected > ${threshold}"
    fi
}

# ===== Summary =====

summary() {
    local total=$((_passed + _failed + _skipped))
    echo ""
    echo "============================="
    if [ "$_failed" -eq 0 ]; then
        echo -e "${GREEN}${BOLD}ALL ${_passed} TESTS PASSED${NC} (${_skipped} skipped)"
    else
        echo -e "${RED}${BOLD}${_failed} FAILED${NC}, ${_passed} passed, ${_skipped} skipped (${total} total)"
    fi
    echo "============================="
    return "$_failed"
}

# ===== Cleanup =====

cleanup_qemu() {
    for pid in "${_qemu_pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null || true
        fi
    done
    _qemu_pids=()
}

trap cleanup_qemu EXIT
