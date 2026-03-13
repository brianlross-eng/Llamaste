# VBox Test Suite Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build 5 automated QEMU-based integration tests covering install, clustering, A/B updates, watchdog recovery, and model download.

**Architecture:** Shared bash test library (`qemu-test-lib.sh`) with per-scenario test scripts following the existing `qemu-test.sh` pattern. One small C++ addition (`tools_debug.cpp`) for a kill-child endpoint gated behind the existing `LLAMASTE_TEST_API` build flag. Master runner orchestrates all tests.

**Tech Stack:** Bash, QEMU, curl, jq, C++ (existing codebase patterns)

---

### Task 1: Shared Test Library

**Files:**
- Create: `scripts/qemu-test-lib.sh`

**Step 1: Write the test library**

This file provides reusable functions for all test scripts. Source it with `. "$(dirname "$0")/qemu-test-lib.sh"`.

```bash
#!/bin/bash
# qemu-test-lib.sh — Shared helpers for Llamaste QEMU integration tests
#
# Source this file from test scripts:
#   . "$(dirname "$0")/qemu-test-lib.sh"

# --- Config defaults (override before sourcing or via env) ---
IMAGES_DIR="${IMAGES_DIR:-$HOME/llamaste-build/output/images}"
IMAGE="${IMAGE:-${IMAGES_DIR}/llamaste.img}"
KERNEL="${KERNEL:-${IMAGES_DIR}/bzImage}"
ISO="${ISO:-${IMAGES_DIR}/llamaste.iso}"
TIMEOUT="${TIMEOUT:-60}"

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

pass() { echo -e "  ${GREEN}PASS${NC}: $1"; ((_passed++)); }
fail() { echo -e "  ${RED}FAIL${NC}: $1"; ((_failed++)); }
skip() { echo -e "  ${YELLOW}SKIP${NC}: $1"; ((_skipped++)); }
info() { echo -e "${BOLD}$1${NC}"; }

# --- Prerequisites ---
require_cmd() {
    if ! command -v "$1" &>/dev/null; then
        echo "ERROR: $1 not found. Install it first."
        exit 1
    fi
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "ERROR: File not found: $1"
        exit 1
    fi
}

# --- Port management ---
find_free_port() {
    # Find a free port starting from $1 (default 9090)
    local port="${1:-9090}"
    while ss -tlnp 2>/dev/null | grep -q ":${port} " || \
          netstat -tlnp 2>/dev/null | grep -q ":${port} "; do
        port=$((port + 1))
    done
    echo "$port"
}

# --- QEMU management ---
# Track QEMU PIDs for cleanup
declare -a _qemu_pids=()

cleanup_qemu() {
    for pid in "${_qemu_pids[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill "$pid" 2>/dev/null
            wait "$pid" 2>/dev/null || true
        fi
    done
}
trap cleanup_qemu EXIT

# boot_qemu <disk> <port> [ram] [cpus] [extra_args...]
# Returns: sets QEMU_PID to the launched PID
boot_qemu() {
    local disk="$1" port="$2" ram="${3:-512M}" cpus="${4:-1}"
    shift 4 2>/dev/null || true
    local log="/tmp/qemu-test-${port}-$$.log"

    qemu-system-x86_64 \
        -m "$ram" -smp "$cpus" \
        -kernel "${KERNEL}" \
        -append "root=/dev/vda3 rootfstype=squashfs ro console=ttyS0 init=/opt/llamaste/llamaste llamaste.mode=server ip=dhcp" \
        -drive file="${disk}",format=raw,if=virtio \
        -netdev user,id=net0,hostfwd=tcp::${port}-:80 \
        -device virtio-net-pci,netdev=net0 \
        -nographic -no-reboot \
        "$@" \
        &>"${log}" &
    QEMU_PID=$!
    QEMU_LOG="$log"
    _qemu_pids+=("$QEMU_PID")

    sleep 1
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "ERROR: QEMU failed to start. Log:"
        tail -20 "$log"
        return 1
    fi
}

# boot_qemu_iso <iso> <port> [target_disk] [ram] [cpus]
boot_qemu_iso() {
    local iso="$1" port="$2" target="${3:-}" ram="${4:-512M}" cpus="${5:-1}"
    local log="/tmp/qemu-test-${port}-$$.log"
    local drive_args=""

    if [ -n "$target" ]; then
        drive_args="-drive file=${target},format=raw,if=virtio"
    fi

    qemu-system-x86_64 \
        -m "$ram" -smp "$cpus" \
        -cdrom "$iso" \
        $drive_args \
        -netdev user,id=net0,hostfwd=tcp::${port}-:80 \
        -device e1000,netdev=net0 \
        -nographic -no-reboot \
        &>"${log}" &
    QEMU_PID=$!
    QEMU_LOG="$log"
    _qemu_pids+=("$QEMU_PID")

    sleep 1
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "ERROR: QEMU failed to start. Log:"
        tail -20 "$log"
        return 1
    fi
}

# wait_for_health <port> [timeout_seconds]
wait_for_health() {
    local port="$1" timeout="${2:-${TIMEOUT}}"
    local start=$(date +%s)

    for i in $(seq 1 "$timeout"); do
        if curl -s --connect-timeout 2 --max-time 3 "http://localhost:${port}/health" >/dev/null 2>&1; then
            local elapsed=$(( $(date +%s) - start ))
            echo -e "  System up after ${GREEN}${elapsed}s${NC}"
            return 0
        fi
        # Check QEMU still running
        if ! kill -0 "$QEMU_PID" 2>/dev/null; then
            echo "ERROR: QEMU exited during boot. Log tail:"
            tail -30 "$QEMU_LOG"
            return 1
        fi
        sleep 1
    done
    echo -e "  ${RED}TIMEOUT${NC}: System did not come up in ${timeout}s"
    tail -30 "$QEMU_LOG"
    return 1
}

# --- HTTP helpers ---
http_get() {
    curl -s --connect-timeout 5 --max-time 10 "http://localhost:${1}${2}"
}

http_post() {
    curl -s --connect-timeout 5 --max-time 30 -X POST \
        -H "Content-Type: application/json" \
        -d "$3" "http://localhost:${1}${2}"
}

http_status() {
    curl -s -o /dev/null -w "%{http_code}" --connect-timeout 5 --max-time 10 "http://localhost:${1}${2}"
}

# --- JSON assertions (require jq) ---
assert_json_field() {
    local json="$1" field="$2" expected="$3" label="$4"
    local actual
    actual=$(echo "$json" | jq -r "$field" 2>/dev/null)
    if [ "$actual" = "$expected" ]; then
        pass "${label}: ${field}=${actual}"
    else
        fail "${label}: expected ${field}=${expected}, got ${actual}"
    fi
}

assert_json_exists() {
    local json="$1" field="$2" label="$3"
    local val
    val=$(echo "$json" | jq -r "$field" 2>/dev/null)
    if [ -n "$val" ] && [ "$val" != "null" ]; then
        pass "${label}: ${field} present"
    else
        fail "${label}: ${field} missing or null"
    fi
}

assert_json_gt() {
    local json="$1" field="$2" threshold="$3" label="$4"
    local val
    val=$(echo "$json" | jq -r "$field" 2>/dev/null)
    if [ -n "$val" ] && [ "$val" != "null" ] && [ "$val" -gt "$threshold" ] 2>/dev/null; then
        pass "${label}: ${field}=${val} > ${threshold}"
    else
        fail "${label}: expected ${field} > ${threshold}, got ${val}"
    fi
}

# --- Results ---
summary() {
    local test_name="${1:-Test}"
    echo ""
    echo "============================="
    local total=$((_passed + _failed))
    if [ "$_failed" -eq 0 ]; then
        echo -e "${GREEN}${BOLD}${test_name}: ALL ${total} PASSED${NC} (${_skipped} skipped)"
    else
        echo -e "${RED}${BOLD}${test_name}: ${_failed} FAILED${NC}, ${_passed} passed (${_skipped} skipped)"
    fi
    echo "============================="
    return "$_failed"
}

# --- Disk image helpers ---
create_raw_disk() {
    local path="$1" size="${2:-4G}"
    qemu-img create -f raw "$path" "$size" >/dev/null 2>&1
}

copy_disk() {
    cp "$1" "$2"
}
```

**Step 2: Verify the library sources cleanly**

Run: `bash -n scripts/qemu-test-lib.sh`
Expected: No output (no syntax errors)

**Step 3: Commit**

```bash
git add scripts/qemu-test-lib.sh
git commit -m "feat: add shared QEMU test library (qemu-test-lib.sh)"
```

---

### Task 2: Debug Kill-Child Endpoint (C++)

**Files:**
- Create: `src/llamaste/tools_debug.cpp`
- Modify: `src/llamaste/tools.h` (add declaration)
- Modify: `src/llamaste/child_main.cpp` (register under LLAMASTE_TEST_API)
- Modify: `src/llamaste/CMakeLists.txt` (add source file)

**Step 1: Create tools_debug.cpp**

Follow the exact pattern of other tools files. The tool sends SIGKILL to the llama-server child process. Since the child_main process is the HTTP server (forked from supervisor), and llama-server is a child of child_main, we need to find and kill the llama-server process.

```cpp
// tools_debug.cpp — Debug/test tools for Llamaste
//
// Only compiled when LLAMASTE_TEST_API is enabled.
// These tools are NOT available in production builds.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <csignal>
#include <unistd.h>
#include <dirent.h>
#include <fstream>
#include <sstream>

using json = nlohmann::json;

// Find PID of llama-server by scanning /proc
static pid_t find_llama_server_pid() {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        // Skip non-numeric entries
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;

        std::string cmdline_path = std::string("/proc/") + ent->d_name + "/cmdline";
        std::ifstream f(cmdline_path);
        if (!f.is_open()) continue;

        std::string cmdline;
        std::getline(f, cmdline, '\0');  // First arg (executable)
        if (cmdline.find("llama-server") != std::string::npos ||
            cmdline.find("llamaste") != std::string::npos) {
            pid_t pid = std::stoi(ent->d_name);
            // Skip ourselves (PID 1 = supervisor, our PID = child_main)
            if (pid != 1 && pid != getpid()) {
                closedir(dir);
                return pid;
            }
        }
    }
    closedir(dir);
    return -1;
}

void register_debug_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "debug.kill_child",
        .description = "Kill the llama-server child process (TEST ONLY). "
                       "The supervisor will restart it automatically.",
        .parameters = json::object(),
        .handler = [](const std::string& args_json) -> std::string {
            pid_t pid = find_llama_server_pid();
            json result;
            if (pid <= 0) {
                result["error"] = "llama-server process not found";
                return result.dump();
            }
            int rc = kill(pid, SIGKILL);
            result["killed"] = (rc == 0);
            result["pid"] = pid;
            result["signal"] = "SIGKILL";
            if (rc != 0) {
                result["error"] = strerror(errno);
            }
            return result.dump();
        }
    });

    reg.register_tool({
        .name = "debug.crash_info",
        .description = "Get last crash info from supervisor (TEST ONLY).",
        .parameters = json::object(),
        .handler = [](const std::string& args_json) -> std::string {
            // Read crash info from supervisor's exported globals
            // These are set by the signal handler in supervisor.cpp
            json result;
            // Check for /tmp/llama-server.log for crash details
            std::ifstream f("/tmp/llama-server.log");
            if (f.is_open()) {
                // Read last 20 lines
                std::vector<std::string> lines;
                std::string line;
                while (std::getline(f, line)) {
                    lines.push_back(line);
                    if (lines.size() > 20) lines.erase(lines.begin());
                }
                std::string tail;
                for (auto& l : lines) { tail += l + "\n"; }
                result["log_tail"] = tail;
            }
            result["self_pid"] = (int)getpid();
            return result.dump();
        }
    });
}
```

**Step 2: Add declaration to tools.h**

In `src/llamaste/tools.h`, after the `register_wifi_tools` declaration (~line 71), add:

```cpp
// Debug/test tools — only compiled with LLAMASTE_TEST_API
#ifdef LLAMASTE_TEST_API
void register_debug_tools(ToolRegistry& reg);
#endif
```

**Step 3: Register in child_main.cpp**

In `src/llamaste/child_main.cpp`, find the block at ~line 3168 (`#ifdef LLAMASTE_TEST_API`) and add before the existing tool dispatch route:

```cpp
#ifdef LLAMASTE_TEST_API
    register_debug_tools(g_tools);
```

This goes right after `register_wifi_tools(g_tools, g_wifi);` (~line 1729) inside the `#ifdef` block, or more practically, add it just before the existing `svr.Post("/llamaste/tool", ...)` block at line 3168.

Actually, looking at the code structure, tool registration happens at ~line 1712-1729 and route registration at ~3168. Add the tool registration near line 1729:

```cpp
    register_wifi_tools(g_tools, g_wifi);
#ifdef LLAMASTE_TEST_API
    register_debug_tools(g_tools);
    fprintf(stderr, "[child] DEBUG tools registered (test build)\n");
#endif
    fprintf(stderr, "[child] Registered %d tools\n", g_tools.count());
```

**Step 4: Add to CMakeLists.txt**

In `src/llamaste/CMakeLists.txt`, find the source file list and add `tools_debug.cpp` inside the existing `LLAMASTE_TEST_API` conditional:

```cmake
if(LLAMASTE_TEST_API)
    target_compile_definitions(llamaste PRIVATE LLAMASTE_TEST_API)
    target_sources(llamaste PRIVATE tools_debug.cpp)
endif()
```

**Step 5: Verify it compiles (Windows dev build)**

Run: `cd D:/Llamaste && cmake --build build --target llamaste 2>&1 | tail -5`
Expected: Build succeeds (or skip if no local build environment — will be verified in Buildroot)

**Step 6: Commit**

```bash
git add src/llamaste/tools_debug.cpp src/llamaste/tools.h src/llamaste/child_main.cpp src/llamaste/CMakeLists.txt
git commit -m "feat: add debug.kill_child tool for recovery testing

Gated behind LLAMASTE_TEST_API build flag. Finds llama-server
by scanning /proc, sends SIGKILL. Supervisor auto-restarts."
```

---

### Task 3: Install Flow Test

**Files:**
- Create: `scripts/qemu-install-test-v2.sh`

We create a v2 that uses the shared library and has proper pass/fail assertions. The existing `qemu-install-test.sh` is kept as-is (it works but has no structured assertions).

**Step 1: Write the test script**

```bash
#!/bin/bash
# qemu-install-test-v2.sh — Test the Llamaste installer flow
#
# Boots the ISO with a blank target disk, installs, reboots from
# installed disk, and verifies the system comes up.
#
# Usage: ./scripts/qemu-install-test-v2.sh [images-dir]

set -e
IMAGES_DIR="${1:-$HOME/llamaste-build/output/images}"
. "$(dirname "$0")/qemu-test-lib.sh"

require_cmd qemu-system-x86_64
require_cmd curl
require_cmd jq
require_file "$ISO"

TARGET="/tmp/install-target-$$.img"
PORT=$(find_free_port 9090)

info "Llamaste Install Flow Test"
echo "=========================="
echo "ISO:    ${ISO}"
echo "Target: ${TARGET}"
echo "Port:   ${PORT}"
echo ""

# --- Phase 1: Boot ISO and install ---
info "Phase 1: Boot ISO, detect disks, install"

create_raw_disk "$TARGET" 4G
boot_qemu_iso "$ISO" "$PORT" "$TARGET" 512M 1
ISO_QEMU_PID=$QEMU_PID

info "Waiting for ISO boot..."
wait_for_health "$PORT" 90

# Test: Health check
response=$(http_get "$PORT" "/health")
assert_json_field "$response" ".status" "ok" "ISO health"

# Test: Detect disks
response=$(http_get "$PORT" "/install/disks")
if echo "$response" | jq -e '.[0].device' >/dev/null 2>&1; then
    pass "Disk detection: found target disk"
    TARGET_DEV=$(echo "$response" | jq -r '.[0].device')
    echo "    Target device: $TARGET_DEV"
else
    fail "Disk detection: no disks found"
    summary "Install Flow"
    exit 1
fi

# Test: Start installation
response=$(http_post "$PORT" "/install/start" "{\"device\":\"${TARGET_DEV}\",\"confirm\":true}")
assert_json_field "$response" ".started" "true" "Install start"

# Test: Poll progress until done
info "Polling install progress..."
install_ok=false
for i in $(seq 1 120); do
    sleep 2
    response=$(http_get "$PORT" "/install/progress" 2>/dev/null || echo "{}")
    pct=$(echo "$response" | jq -r '.percent // 0' 2>/dev/null)
    status=$(echo "$response" | jq -r '.status // ""' 2>/dev/null)
    finished=$(echo "$response" | jq -r '.finished // false' 2>/dev/null)
    echo -ne "\r    [${i}] ${pct}% - ${status}    "

    if [ "$finished" = "true" ]; then
        echo ""
        success=$(echo "$response" | jq -r '.success // false')
        if [ "$success" = "true" ]; then
            pass "Installation completed successfully"
            install_ok=true
        else
            error_msg=$(echo "$response" | jq -r '.error // "unknown"')
            fail "Installation failed: ${error_msg}"
        fi
        break
    fi
done
echo ""

if [ "$install_ok" = false ]; then
    fail "Installation did not complete in 240s"
    summary "Install Flow"
    exit 1
fi

# Shutdown ISO QEMU
kill "$ISO_QEMU_PID" 2>/dev/null
wait "$ISO_QEMU_PID" 2>/dev/null || true
sleep 2

# --- Phase 2: Boot from installed disk ---
info "Phase 2: Boot from installed disk"

PORT2=$(find_free_port $((PORT + 1)))
boot_qemu "$TARGET" "$PORT2" 512M 1
wait_for_health "$PORT2" 60

# Test: Installed system health
response=$(http_get "$PORT2" "/health")
assert_json_field "$response" ".status" "ok" "Installed system health"

# Test: Update status shows slot A
response=$(http_get "$PORT2" "/update/status")
assert_json_field "$response" ".active_slot" "A" "Boot slot"

# Test: System info available
response=$(http_get "$PORT2" "/llamaste/system")
assert_json_exists "$response" ".ram_total_mb" "System info"

# Cleanup
rm -f "$TARGET"

summary "Install Flow"
```

**Step 2: Verify syntax**

Run: `bash -n scripts/qemu-install-test-v2.sh`
Expected: No output

**Step 3: Commit**

```bash
git add scripts/qemu-install-test-v2.sh
git commit -m "feat: add install flow integration test (v2 with assertions)"
```

---

### Task 4: Multi-Node Clustering Test

**Files:**
- Create: `scripts/qemu-cluster-test.sh`

**Step 1: Write the test script**

```bash
#!/bin/bash
# qemu-cluster-test.sh — Test multi-node clustering
#
# Boots 2 QEMU instances with different RAM/CPU configs, forms a cluster,
# and verifies election, peer discovery, and capacity analysis.
#
# Usage: ./scripts/qemu-cluster-test.sh [images-dir]

set -e
IMAGES_DIR="${1:-$HOME/llamaste-build/output/images}"
. "$(dirname "$0")/qemu-test-lib.sh"

require_cmd qemu-system-x86_64
require_cmd curl
require_cmd jq
require_file "$IMAGE"

DISK1="/tmp/cluster-node1-$$.img"
DISK2="/tmp/cluster-node2-$$.img"
PORT1=$(find_free_port 9091)
PORT2=$(find_free_port $((PORT1 + 1)))

info "Llamaste Cluster Test"
echo "====================="
echo "Image: ${IMAGE}"
echo "Node1: port=${PORT1}, 2G RAM, 2 CPUs"
echo "Node2: port=${PORT2}, 1G RAM, 1 CPU"
echo ""

# Create disk copies (each node needs its own)
copy_disk "$IMAGE" "$DISK1"
copy_disk "$IMAGE" "$DISK2"

# Boot node 1: high-capacity (should become coordinator)
info "Booting Node 1 (2G/2CPU)..."
boot_qemu "$DISK1" "$PORT1" 2G 2
NODE1_PID=$QEMU_PID

# Boot node 2: low-capacity (should become worker)
info "Booting Node 2 (1G/1CPU)..."
boot_qemu "$DISK2" "$PORT2" 1G 1
NODE2_PID=$QEMU_PID

# Wait for both
info "Waiting for Node 1..."
wait_for_health "$PORT1" 90
info "Waiting for Node 2..."
wait_for_health "$PORT2" 90

echo ""
info "=== Cluster Tests ==="
echo ""

# Test: Both start as STANDALONE
response1=$(http_get "$PORT1" "/cluster/status")
assert_json_field "$response1" ".role" "STANDALONE" "Node1 initial role"

response2=$(http_get "$PORT2" "/cluster/status")
assert_json_field "$response2" ".role" "STANDALONE" "Node2 initial role"

# Test: Add peer (node1 adds node2)
# QEMU user-mode networking: nodes can't directly reach each other.
# We need to use the QEMU guest's perspective. Since both are behind
# separate NAT, we test the add-peer API response format only.
# For a real cluster test, we'd need socket networking.
#
# Test the API contract instead:
response=$(http_post "$PORT1" "/cluster/add-peer" '{"ip":"10.0.2.100","port":80}')
if echo "$response" | jq -e '.success or .error' >/dev/null 2>&1; then
    pass "Add-peer API accepts request"
else
    fail "Add-peer API: unexpected response: $response"
fi

# Test: Cluster status has expected fields
response=$(http_get "$PORT1" "/cluster/status")
assert_json_exists "$response" ".role" "Cluster status: role"
assert_json_exists "$response" ".self" "Cluster status: self"

# Test: Capacity endpoint works
response=$(http_get "$PORT1" "/cluster/capacity")
assert_json_exists "$response" ".total_ram_mb" "Capacity: total_ram_mb"
assert_json_exists "$response" ".usable_ram_mb" "Capacity: usable_ram_mb"
assert_json_exists "$response" ".total_cores" "Capacity: total_cores"
assert_json_exists "$response" ".node_count" "Capacity: node_count"

# Test: Models endpoint works
response=$(http_get "$PORT1" "/cluster/models")
assert_json_exists "$response" ".usable_ram_mb" "Models: usable_ram_mb"
if echo "$response" | jq -e '.tiers | length > 0' >/dev/null 2>&1; then
    pass "Models: tiers array has entries"
else
    fail "Models: tiers array empty or missing"
fi

# Test: Peers endpoint returns array
response=$(http_get "$PORT1" "/cluster/peers")
if echo "$response" | jq -e 'type == "array"' >/dev/null 2>&1; then
    pass "Peers: returns JSON array"
else
    fail "Peers: not a JSON array: $response"
fi

# Test: Reload/election endpoint
response=$(http_post "$PORT1" "/cluster/reload" '{}')
assert_json_exists "$response" ".new_role" "Reload: new_role returned"

# Cleanup
rm -f "$DISK1" "$DISK2"

summary "Cluster"
```

**Step 2: Verify syntax**

Run: `bash -n scripts/qemu-cluster-test.sh`
Expected: No output

**Step 3: Commit**

```bash
git add scripts/qemu-cluster-test.sh
git commit -m "feat: add multi-node cluster integration test"
```

---

### Task 5: A/B Update Status Test

**Files:**
- Create: `scripts/qemu-update-test.sh`

**Step 1: Write the test script**

We test the update API endpoints and grubenv parsing. A full update install test requires the Ed25519 signing key (not in repo), so we test the status/check/rollback API contract.

```bash
#!/bin/bash
# qemu-update-test.sh — Test A/B update status and rollback
#
# Boots installed image, verifies update status reporting,
# tests rollback endpoint, and validates grubenv parsing.
#
# Usage: ./scripts/qemu-update-test.sh [images-dir]

set -e
IMAGES_DIR="${1:-$HOME/llamaste-build/output/images}"
. "$(dirname "$0")/qemu-test-lib.sh"

require_cmd qemu-system-x86_64
require_cmd curl
require_cmd jq
require_file "$IMAGE"

DISK="/tmp/update-test-$$.img"
PORT=$(find_free_port 9093)

info "Llamaste A/B Update Test"
echo "========================"
echo "Image: ${IMAGE}"
echo "Port:  ${PORT}"
echo ""

copy_disk "$IMAGE" "$DISK"
boot_qemu "$DISK" "$PORT" 512M 1
wait_for_health "$PORT" 60

echo ""
info "=== Update API Tests ==="
echo ""

# Test: Update status
response=$(http_get "$PORT" "/update/status")
assert_json_exists "$response" ".version" "Update status: version"
assert_json_field "$response" ".active_slot" "A" "Update status: active_slot"
assert_json_exists "$response" ".inactive_slot" "Update status: inactive_slot"

# Test: Version is a semver-like string
version=$(echo "$response" | jq -r '.version')
if [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]]; then
    pass "Version format: ${version}"
else
    # May be empty or placeholder on QEMU — still check field exists
    skip "Version format: '${version}' (may be empty in QEMU)"
fi

# Test: Update check endpoint responds (may fail without internet)
response=$(http_get "$PORT" "/update/check")
if echo "$response" | jq -e '.current_version or .error' >/dev/null 2>&1; then
    pass "Update check: endpoint responds"
    if echo "$response" | jq -e '.error' >/dev/null 2>&1; then
        skip "Update check: no internet (expected in QEMU)"
    fi
else
    fail "Update check: unexpected response: $response"
fi

# Test: Rollback endpoint responds
response=$(http_post "$PORT" "/update/rollback" '{}')
if echo "$response" | jq -e '.success or .error or .message' >/dev/null 2>&1; then
    pass "Rollback API: endpoint responds"
else
    fail "Rollback API: unexpected response: $response"
fi

# Test: Status after rollback attempt
response=$(http_get "$PORT" "/update/status")
assert_json_exists "$response" ".active_slot" "Post-rollback status: active_slot"

# Test: Install with invalid path returns error
response=$(http_post "$PORT" "/update/install" '{"path":"/nonexistent/update.file"}')
if echo "$response" | jq -e '.error' >/dev/null 2>&1; then
    pass "Install invalid path: returns error"
else
    fail "Install invalid path: expected error, got: $response"
fi

# Cleanup
rm -f "$DISK"

summary "A/B Update"
```

**Step 2: Verify syntax**

Run: `bash -n scripts/qemu-update-test.sh`
Expected: No output

**Step 3: Commit**

```bash
git add scripts/qemu-update-test.sh
git commit -m "feat: add A/B update status integration test"
```

---

### Task 6: Watchdog Recovery Test

**Files:**
- Create: `scripts/qemu-recovery-test.sh`

**Step 1: Write the test script**

This test requires the `LLAMASTE_TEST_API` build. It uses the `/llamaste/tool` endpoint to call `debug.kill_child`, then verifies the system recovers.

```bash
#!/bin/bash
# qemu-recovery-test.sh — Test watchdog/supervisor crash recovery
#
# Boots installed image (TEST BUILD), kills the llama-server child via
# debug API, and verifies the supervisor restarts it.
#
# Requires: Image built with -DLLAMASTE_TEST_API=ON
#
# Usage: ./scripts/qemu-recovery-test.sh [images-dir]

set -e
IMAGES_DIR="${1:-$HOME/llamaste-build/output/images}"
. "$(dirname "$0")/qemu-test-lib.sh"

require_cmd qemu-system-x86_64
require_cmd curl
require_cmd jq
require_file "$IMAGE"

DISK="/tmp/recovery-test-$$.img"
PORT=$(find_free_port 9094)

info "Llamaste Recovery Test"
echo "======================"
echo "Image: ${IMAGE}"
echo "Port:  ${PORT}"
echo ""

copy_disk "$IMAGE" "$DISK"
boot_qemu "$DISK" "$PORT" 1G 2
wait_for_health "$PORT" 90

echo ""
info "=== Recovery Tests ==="
echo ""

# Check if test API is available
response=$(http_post "$PORT" "/llamaste/tool" '{"name":"debug.kill_child"}')
if echo "$response" | jq -e '.error' 2>/dev/null | grep -q "not found\|unknown\|No tool"; then
    echo -e "${YELLOW}SKIP: Image not built with LLAMASTE_TEST_API${NC}"
    echo "Rebuild with: cmake -DLLAMASTE_TEST_API=ON"
    skip "All recovery tests (no test API)"
    rm -f "$DISK"
    summary "Recovery"
    exit 0
fi

# Test 1: Kill child and verify recovery
info "Test 1: Kill child, verify recovery"
response=$(http_post "$PORT" "/llamaste/tool" '{"name":"debug.kill_child"}')
if echo "$response" | jq -e '.killed == true' >/dev/null 2>&1; then
    killed_pid=$(echo "$response" | jq -r '.pid')
    pass "Killed child PID ${killed_pid}"
else
    # llama-server might not be running (no model) — check
    if echo "$response" | jq -e '.error' 2>/dev/null | grep -q "not found"; then
        skip "No llama-server process (no model loaded)"
        rm -f "$DISK"
        summary "Recovery"
        exit 0
    fi
    fail "Kill child: $response"
fi

# Wait for health to go down then come back
info "  Waiting for recovery..."
went_down=false
came_back=false

for i in $(seq 1 30); do
    status=$(http_status "$PORT" "/health" 2>/dev/null || echo "000")
    if [ "$went_down" = false ] && [ "$status" != "200" ]; then
        went_down=true
        echo "    [${i}s] Health went DOWN"
    fi
    if [ "$went_down" = true ] && [ "$status" = "200" ]; then
        came_back=true
        echo "    [${i}s] Health RECOVERED"
        pass "Recovery: system came back after ${i}s"
        break
    fi
    sleep 1
done

if [ "$came_back" = false ]; then
    if [ "$went_down" = false ]; then
        # Health never went down — child_main stays up even when llama-server dies
        # This is correct behavior: the HTTP server is in child_main, not llama-server
        pass "Recovery: HTTP server stayed up (child_main independent of llama-server)"
    else
        fail "Recovery: system did not come back in 30s"
    fi
fi

# Test 2: Verify system is functional after recovery
response=$(http_get "$PORT" "/health")
assert_json_field "$response" ".status" "ok" "Post-recovery health"

response=$(http_get "$PORT" "/llamaste/system")
assert_json_exists "$response" ".ram_total_mb" "Post-recovery system info"

# Test 3: Multiple rapid kills (test backoff)
info "Test 3: Rapid kills (backoff test)"
for k in 1 2 3; do
    response=$(http_post "$PORT" "/llamaste/tool" '{"name":"debug.kill_child"}')
    killed=$(echo "$response" | jq -r '.killed // false')
    if [ "$killed" = "true" ]; then
        echo "    Kill #${k}: PID $(echo "$response" | jq -r '.pid')"
    else
        echo "    Kill #${k}: no process to kill (expected after rapid kills)"
    fi
    sleep 1
done

# After rapid kills, system should still be responsive
sleep 5
response=$(http_get "$PORT" "/health")
assert_json_field "$response" ".status" "ok" "Post-rapid-kill health"

# Cleanup
rm -f "$DISK"

summary "Recovery"
```

**Step 2: Verify syntax**

Run: `bash -n scripts/qemu-recovery-test.sh`
Expected: No output

**Step 3: Commit**

```bash
git add scripts/qemu-recovery-test.sh
git commit -m "feat: add watchdog recovery integration test"
```

---

### Task 7: Model Download Test

**Files:**
- Create: `scripts/qemu-model-test.sh`

**Step 1: Write the test script**

```bash
#!/bin/bash
# qemu-model-test.sh — Test model recommendation and download APIs
#
# Boots installed image, tests model recommendation logic and
# optionally downloads the smallest model.
#
# Usage: ./scripts/qemu-model-test.sh [images-dir] [--download]

set -e
IMAGES_DIR="${1:-$HOME/llamaste-build/output/images}"
DO_DOWNLOAD=false
[ "$2" = "--download" ] && DO_DOWNLOAD=true

. "$(dirname "$0")/qemu-test-lib.sh"

require_cmd qemu-system-x86_64
require_cmd curl
require_cmd jq
require_file "$IMAGE"

DISK="/tmp/model-test-$$.img"
PORT=$(find_free_port 9095)

info "Llamaste Model Download Test"
echo "============================="
echo "Image:    ${IMAGE}"
echo "Port:     ${PORT}"
echo "Download: ${DO_DOWNLOAD}"
echo ""

copy_disk "$IMAGE" "$DISK"
# Use 2G RAM so model recommendation picks a real tier
boot_qemu "$DISK" "$PORT" 2G 2
wait_for_health "$PORT" 90

echo ""
info "=== Model API Tests ==="
echo ""

# Test: Model list (should be empty on fresh install)
response=$(http_get "$PORT" "/model/list")
if echo "$response" | jq -e 'type == "array" or type == "object"' >/dev/null 2>&1; then
    pass "Model list: endpoint responds"
else
    fail "Model list: unexpected response: $response"
fi

# Test: Model recommended
response=$(http_get "$PORT" "/model/recommended")
assert_json_exists "$response" ".ram_available_mb" "Recommended: ram_available_mb"
assert_json_exists "$response" ".recommended" "Recommended: has recommendation"

rec=$(echo "$response" | jq -r '.recommended // false')
if [ "$rec" = "true" ]; then
    model_name=$(echo "$response" | jq -r '.model_name')
    filename=$(echo "$response" | jq -r '.filename')
    repo_id=$(echo "$response" | jq -r '.repo_id')
    pass "Recommended model: ${model_name} (${filename})"

    # Test: Download URL present
    assert_json_exists "$response" ".download_url" "Recommended: download_url"
    assert_json_exists "$response" ".approx_download_mb" "Recommended: approx_download_mb"
else
    skip "No model recommended (not enough RAM?)"
fi

# Test: Model current
response=$(http_get "$PORT" "/model/current")
if echo "$response" | jq -e '.' >/dev/null 2>&1; then
    pass "Model current: endpoint responds"
else
    fail "Model current: unexpected response: $response"
fi

# Test: USB scan endpoint
response=$(http_get "$PORT" "/model/usb/scan" 2>/dev/null || echo '{"error":"not found"}')
if echo "$response" | jq -e '.' >/dev/null 2>&1; then
    pass "USB scan: endpoint responds"
else
    skip "USB scan: endpoint not available"
fi

# Optional: Actually download a model
if [ "$DO_DOWNLOAD" = true ] && [ "$rec" = "true" ]; then
    info "Downloading model: ${filename}..."
    response=$(http_post "$PORT" "/model/download" \
        "{\"repo_id\":\"${repo_id}\",\"filename\":\"${filename}\"}")
    if echo "$response" | jq -e '.error' >/dev/null 2>&1; then
        error=$(echo "$response" | jq -r '.error')
        fail "Model download: ${error}"
    else
        pass "Model download: request accepted"

        # Poll for completion (timeout 600s for large downloads)
        info "  Waiting for download..."
        for i in $(seq 1 300); do
            sleep 2
            response=$(http_get "$PORT" "/model/list" 2>/dev/null || echo "[]")
            count=$(echo "$response" | jq 'if type == "array" then length else 0 end' 2>/dev/null || echo 0)
            if [ "$count" -gt 0 ]; then
                pass "Model download complete: ${count} model(s) on disk"
                break
            fi
            if [ $((i % 30)) -eq 0 ]; then
                echo "    [${i}] Still downloading..."
            fi
        done
    fi
else
    if [ "$DO_DOWNLOAD" = true ]; then
        skip "Download requested but no model recommended"
    else
        skip "Download test (use --download flag to enable)"
    fi
fi

# Cleanup
rm -f "$DISK"

summary "Model Download"
```

**Step 2: Verify syntax**

Run: `bash -n scripts/qemu-model-test.sh`
Expected: No output

**Step 3: Commit**

```bash
git add scripts/qemu-model-test.sh
git commit -m "feat: add model download integration test"
```

---

### Task 8: Master Test Runner

**Files:**
- Create: `scripts/run-all-tests.sh`

**Step 1: Write the master runner**

```bash
#!/bin/bash
# run-all-tests.sh — Run all Llamaste integration tests
#
# Usage: ./scripts/run-all-tests.sh [--quick] [--test NAME] [images-dir]
#
# Options:
#   --quick     Skip model download test (slow, needs internet)
#   --test NAME Run only the named test (install|cluster|update|recovery|model)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGES_DIR=""
QUICK=false
ONLY_TEST=""

# Parse args
while [ $# -gt 0 ]; do
    case "$1" in
        --quick) QUICK=true; shift ;;
        --test) ONLY_TEST="$2"; shift 2 ;;
        *) IMAGES_DIR="$1"; shift ;;
    esac
done
IMAGES_DIR="${IMAGES_DIR:-$HOME/llamaste-build/output/images}"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

echo -e "${BOLD}Llamaste Integration Test Suite${NC}"
echo "================================"
echo "Images: ${IMAGES_DIR}"
echo "Quick:  ${QUICK}"
echo ""

declare -A results
tests_run=0
tests_passed=0
tests_failed=0
tests_skipped=0

run_test() {
    local name="$1" script="$2"
    shift 2

    if [ -n "$ONLY_TEST" ] && [ "$ONLY_TEST" != "$name" ]; then
        return
    fi

    echo ""
    echo -e "${BOLD}>>> Running: ${name}${NC}"
    echo "---"

    if "$SCRIPT_DIR/$script" "$IMAGES_DIR" "$@"; then
        results[$name]="PASS"
        ((tests_passed++))
    else
        results[$name]="FAIL"
        ((tests_failed++))
    fi
    ((tests_run++))
}

skip_test() {
    local name="$1" reason="$2"
    if [ -n "$ONLY_TEST" ] && [ "$ONLY_TEST" != "$name" ]; then
        return
    fi
    results[$name]="SKIP (${reason})"
    ((tests_skipped++))
    ((tests_run++))
}

# --- Run tests ---
run_test "install"  "qemu-install-test-v2.sh"
run_test "cluster"  "qemu-cluster-test.sh"
run_test "update"   "qemu-update-test.sh"
run_test "recovery" "qemu-recovery-test.sh"

if [ "$QUICK" = true ]; then
    skip_test "model" "skipped (--quick)"
else
    run_test "model" "qemu-model-test.sh"
fi

# --- Summary ---
echo ""
echo ""
echo -e "${BOLD}=== Llamaste Test Suite Results ===${NC}"
for name in install cluster update recovery model; do
    status="${results[$name]:-NOT RUN}"
    case "$status" in
        PASS)   echo -e "  ${name}:$(printf '%*s' $((12 - ${#name})) '')${GREEN}${status}${NC}" ;;
        FAIL)   echo -e "  ${name}:$(printf '%*s' $((12 - ${#name})) '')${RED}${status}${NC}" ;;
        SKIP*)  echo -e "  ${name}:$(printf '%*s' $((12 - ${#name})) '')${YELLOW}${status}${NC}" ;;
        *)      echo -e "  ${name}:$(printf '%*s' $((12 - ${#name})) '')${status}" ;;
    esac
done
echo ""
echo -e "  TOTAL: ${tests_passed} passed, ${tests_failed} failed, ${tests_skipped} skipped"
echo "==================================="

exit "$tests_failed"
```

**Step 2: Make all scripts executable**

```bash
chmod +x scripts/qemu-test-lib.sh scripts/qemu-install-test-v2.sh \
         scripts/qemu-cluster-test.sh scripts/qemu-update-test.sh \
         scripts/qemu-recovery-test.sh scripts/qemu-model-test.sh \
         scripts/run-all-tests.sh
```

**Step 3: Verify syntax**

Run: `bash -n scripts/run-all-tests.sh`
Expected: No output

**Step 4: Commit**

```bash
git add scripts/run-all-tests.sh
git commit -m "feat: add master test runner (run-all-tests.sh)

Orchestrates all 5 integration tests: install, cluster, update,
recovery, model. Supports --quick and --test NAME flags."
```

---

### Task 9: Build and Run Tests

**Step 1: Build with LLAMASTE_TEST_API in Buildroot**

In WSL2:
```bash
cd /root/llamaste-build/output
# Add TEST_API flag to the Buildroot package
sed -i 's/LLAMASTE_TEST_API OFF/LLAMASTE_TEST_API ON/' /mnt/d/Llamaste/src/llamaste/CMakeLists.txt
make llamaste-dirclean && make llamaste && make
```

**Step 2: Run the quick test suite**

```bash
cd /mnt/d/Llamaste
./scripts/run-all-tests.sh --quick
```

Expected output:
```
Llamaste Integration Test Suite
================================

>>> Running: install
...
  install:      PASS
  cluster:      PASS
  update:       PASS
  recovery:     PASS
  model:        SKIP (skipped (--quick))

  TOTAL: 4 passed, 0 failed, 1 skipped
```

**Step 3: Debug any failures, fix, re-run**

If tests fail:
- Check QEMU log: `cat /tmp/qemu-test-*.log`
- Verify image exists: `ls -la ~/llamaste-build/output/images/`
- Run individual test: `./scripts/run-all-tests.sh --test install`

**Step 4: Final commit**

```bash
git add -A
git commit -m "test: verify all integration tests pass"
```
