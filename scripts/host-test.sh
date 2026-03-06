#!/bin/bash
# host-test.sh -- Run all Llamaste unit and integration tests on the host (WSL2)
#
# Usage: ./scripts/host-test.sh
#
# This script builds and runs ALL test suites without needing QEMU:
#   1. Hardware detection tests
#   2. Tools system unit tests
#   3. Agent loop and prompt builder tests
#   4. Tools integration tests (needs /data dir, run as root)
#   5. HTTP server integration tests
#   6. Network (mDNS) tests
#   7. Auth & Console tests
#   8. Inference integration tests
#   9. Model download tests
#
# Prerequisites:
#   - g++ with C++17 support
#   - Run as root for tests that write to /data
#
# Exit code = number of failed test suites (0 = all passed)

set -e

# Resolve project root (works whether called from project root or scripts/)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
SRC="${PROJECT_ROOT}/src/llamaste"
TESTS="${PROJECT_ROOT}/tests"
BUILD_DIR="${PROJECT_ROOT}/tests/build"

# --- Colors ---
GREEN='\033[0;32m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

SUITES_PASSED=0
SUITES_FAILED=0
SUITES_TOTAL=0

# Create build directory
mkdir -p "${BUILD_DIR}"

suite_pass() {
    echo -e "  ${GREEN}SUITE PASSED${NC}: $1"
    SUITES_PASSED=$((SUITES_PASSED + 1))
    SUITES_TOTAL=$((SUITES_TOTAL + 1))
}

suite_fail() {
    echo -e "  ${RED}SUITE FAILED${NC}: $1"
    SUITES_FAILED=$((SUITES_FAILED + 1))
    SUITES_TOTAL=$((SUITES_TOTAL + 1))
}

echo -e "${BOLD}=== Llamaste Host Test Suite ===${NC}"
echo "Project root: ${PROJECT_ROOT}"
echo "Compiler: $(g++ --version | head -1)"
echo ""

# ---------------------------------------------------------------
# Suite 1: Hardware Detection
# ---------------------------------------------------------------
echo -e "${BOLD}--- [1/11] Hardware Detection ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_hwdetect" \
    "${TESTS}/test_hwdetect.cpp" "${SRC}/hwdetect.cpp" 2>&1; then
    if "${BUILD_DIR}/test_hwdetect"; then
        suite_pass "Hardware Detection"
    else
        suite_fail "Hardware Detection (runtime)"
    fi
else
    suite_fail "Hardware Detection (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 2: Tools System
# ---------------------------------------------------------------
echo -e "${BOLD}--- [2/11] Tools System ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_tools" \
    "${TESTS}/test_tools.cpp" \
    "${SRC}/tools.cpp" \
    "${SRC}/tools_fs.cpp" \
    "${SRC}/tools_process.cpp" \
    "${SRC}/tools_network.cpp" \
    "${SRC}/tools_system.cpp" \
    "${SRC}/tools_config.cpp" \
    "${SRC}/tools_model.cpp" \
    "${SRC}/tools_model_download.cpp" \
    "${SRC}/tools_audio.cpp" \
    "${SRC}/voice.cpp" 2>&1; then
    if "${BUILD_DIR}/test_tools"; then
        suite_pass "Tools System"
    else
        suite_fail "Tools System (runtime)"
    fi
else
    suite_fail "Tools System (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 3: Agent Loop & Prompt Builder
# ---------------------------------------------------------------
echo -e "${BOLD}--- [3/11] Agent Loop ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_agent" \
    "${TESTS}/test_agent.cpp" \
    "${SRC}/agent.cpp" \
    "${SRC}/prompt_builder.cpp" \
    "${SRC}/tools.cpp" \
    "${SRC}/tools_fs.cpp" \
    "${SRC}/tools_process.cpp" \
    "${SRC}/tools_network.cpp" \
    "${SRC}/tools_system.cpp" \
    "${SRC}/tools_config.cpp" \
    "${SRC}/tools_model.cpp" \
    "${SRC}/tools_model_download.cpp" \
    "${SRC}/tools_audio.cpp" \
    "${SRC}/voice.cpp" \
    "${SRC}/hwdetect.cpp" 2>&1; then
    if "${BUILD_DIR}/test_agent"; then
        suite_pass "Agent Loop"
    else
        suite_fail "Agent Loop (runtime)"
    fi
else
    suite_fail "Agent Loop (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 4: Tools Integration
# ---------------------------------------------------------------
echo -e "${BOLD}--- [4/11] Tools Integration ---${NC}"
# These tests need /data directory (run as root in WSL2)
if [ ! -d "/data" ]; then
    echo "Creating /data directory for integration tests..."
    mkdir -p /data 2>/dev/null || {
        echo "WARNING: Cannot create /data (not root?). Skipping integration tests."
        suite_fail "Tools Integration (needs /data)"
        echo ""
        # Skip to next suite
        SKIP_INTEGRATION=1
    }
fi

if [ "${SKIP_INTEGRATION:-0}" != "1" ]; then
    if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_tools_integration" \
        "${TESTS}/test_tools_integration.cpp" \
        "${SRC}/tools.cpp" \
        "${SRC}/tools_fs.cpp" \
        "${SRC}/tools_process.cpp" \
        "${SRC}/tools_network.cpp" \
        "${SRC}/tools_system.cpp" \
        "${SRC}/tools_config.cpp" \
        "${SRC}/tools_model.cpp" \
        "${SRC}/tools_model_download.cpp" \
        "${SRC}/tools_audio.cpp" \
        "${SRC}/voice.cpp" 2>&1; then
        if "${BUILD_DIR}/test_tools_integration"; then
            suite_pass "Tools Integration"
        else
            suite_fail "Tools Integration (runtime)"
        fi
    else
        suite_fail "Tools Integration (compile)"
    fi
    echo ""
fi

# ---------------------------------------------------------------
# Suite 5: HTTP Server
# ---------------------------------------------------------------
echo -e "${BOLD}--- [5/11] HTTP Server ---${NC}"
if g++ -std=c++17 -I "${SRC}" -pthread -o "${BUILD_DIR}/test_http" \
    "${TESTS}/test_http.cpp" \
    "${SRC}/child_main.cpp" \
    "${SRC}/agent.cpp" \
    "${SRC}/prompt_builder.cpp" \
    "${SRC}/tools.cpp" \
    "${SRC}/tools_fs.cpp" \
    "${SRC}/tools_process.cpp" \
    "${SRC}/tools_network.cpp" \
    "${SRC}/tools_system.cpp" \
    "${SRC}/tools_config.cpp" \
    "${SRC}/tools_model.cpp" \
    "${SRC}/tools_model_download.cpp" \
    "${SRC}/tools_install.cpp" \
    "${SRC}/tools_schedule.cpp" \
    "${SRC}/tools_auth.cpp" \
    "${SRC}/tools_audio.cpp" \
    "${SRC}/voice.cpp" \
    "${SRC}/mcp_server.cpp" \
    "${SRC}/hwdetect.cpp" \
    "${SRC}/net_mdns.cpp" \
    "${SRC}/scheduler.cpp" \
    "${SRC}/bcrypt.cpp" \
    "${SRC}/auth.cpp" 2>&1; then
    if "${BUILD_DIR}/test_http" 2>/dev/null; then
        suite_pass "HTTP Server"
    else
        suite_fail "HTTP Server (runtime)"
    fi
else
    suite_fail "HTTP Server (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 6: Network (mDNS)
# ---------------------------------------------------------------
echo -e "${BOLD}--- [6/11] Network (mDNS) ---${NC}"
if g++ -std=c++17 -I "${SRC}" -pthread -o "${BUILD_DIR}/test_net" \
    "${TESTS}/test_net.cpp" \
    "${SRC}/net_mdns.cpp" 2>&1; then
    if "${BUILD_DIR}/test_net"; then
        suite_pass "Network (mDNS)"
    else
        suite_fail "Network (mDNS) (runtime)"
    fi
else
    suite_fail "Network (mDNS) (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 7: Auth & Console
# ---------------------------------------------------------------
echo -e "${BOLD}--- [7/11] Auth & Console ---${NC}"
if g++ -std=c++17 -I "${SRC}" -pthread -o "${BUILD_DIR}/test_auth" \
    "${TESTS}/test_auth.cpp" \
    "${SRC}/bcrypt.cpp" \
    "${SRC}/auth.cpp" \
    "${SRC}/supervisor.cpp" 2>&1; then
    if "${BUILD_DIR}/test_auth"; then
        suite_pass "Auth & Console"
    else
        suite_fail "Auth & Console (runtime)"
    fi
else
    suite_fail "Auth & Console (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 8: Inference Integration
# ---------------------------------------------------------------
echo -e "${BOLD}--- [8/11] Inference Integration ---${NC}"
if g++ -std=c++17 -I "${SRC}" -pthread -o "${BUILD_DIR}/test_inference" \
    "${TESTS}/test_inference.cpp" \
    "${SRC}/child_main.cpp" \
    "${SRC}/agent.cpp" \
    "${SRC}/prompt_builder.cpp" \
    "${SRC}/tools.cpp" \
    "${SRC}/tools_fs.cpp" \
    "${SRC}/tools_process.cpp" \
    "${SRC}/tools_network.cpp" \
    "${SRC}/tools_system.cpp" \
    "${SRC}/tools_config.cpp" \
    "${SRC}/tools_model.cpp" \
    "${SRC}/tools_model_download.cpp" \
    "${SRC}/tools_install.cpp" \
    "${SRC}/tools_schedule.cpp" \
    "${SRC}/tools_auth.cpp" \
    "${SRC}/tools_audio.cpp" \
    "${SRC}/voice.cpp" \
    "${SRC}/mcp_server.cpp" \
    "${SRC}/hwdetect.cpp" \
    "${SRC}/net_mdns.cpp" \
    "${SRC}/scheduler.cpp" \
    "${SRC}/bcrypt.cpp" \
    "${SRC}/auth.cpp" 2>&1; then
    if "${BUILD_DIR}/test_inference"; then
        suite_pass "Inference Integration"
    else
        suite_fail "Inference Integration (runtime)"
    fi
else
    suite_fail "Inference Integration (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 9: Model Download
# ---------------------------------------------------------------
echo -e "${BOLD}--- [9/11] Model Download ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_model_download" \
    "${TESTS}/test_model_download.cpp" \
    "${SRC}/tools_model_download.cpp" 2>&1; then
    if "${BUILD_DIR}/test_model_download"; then
        suite_pass "Model Download"
    else
        suite_fail "Model Download (runtime)"
    fi
else
    suite_fail "Model Download (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 10: Audio Tools
# ---------------------------------------------------------------
echo -e "${BOLD}--- [10/11] Audio Tools ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_audio_tools" \
    "${TESTS}/test_audio_tools.cpp" \
    "${SRC}/tools.cpp" \
    "${SRC}/tools_fs.cpp" \
    "${SRC}/tools_process.cpp" \
    "${SRC}/tools_network.cpp" \
    "${SRC}/tools_system.cpp" \
    "${SRC}/tools_config.cpp" \
    "${SRC}/tools_model.cpp" \
    "${SRC}/tools_model_download.cpp" \
    "${SRC}/tools_audio.cpp" \
    "${SRC}/voice.cpp" 2>&1; then
    if "${BUILD_DIR}/test_audio_tools"; then
        suite_pass "Audio Tools"
    else
        suite_fail "Audio Tools (runtime)"
    fi
else
    suite_fail "Audio Tools (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Suite 11: Cluster
# ---------------------------------------------------------------
echo -e "${BOLD}--- [11/11] Cluster ---${NC}"
if g++ -std=c++17 -I "${SRC}" -pthread -o "${BUILD_DIR}/test_cluster" \
    "${TESTS}/test_cluster.cpp" \
    "${SRC}/cluster.cpp" 2>&1; then
    if "${BUILD_DIR}/test_cluster"; then
        suite_pass "Cluster"
    else
        suite_fail "Cluster (runtime)"
    fi
else
    suite_fail "Cluster (compile)"
fi
echo ""

# ---------------------------------------------------------------
# Results
# ---------------------------------------------------------------
echo "=========================================="
if [ "${SUITES_FAILED}" -eq 0 ]; then
    echo -e "${GREEN}${BOLD}ALL ${SUITES_TOTAL} TEST SUITES PASSED${NC}"
else
    echo -e "${RED}${BOLD}${SUITES_FAILED} SUITE(S) FAILED${NC}, ${SUITES_PASSED} passed out of ${SUITES_TOTAL} total"
fi
echo "=========================================="

# Clean up build artifacts
# (leave them for debugging; uncomment to auto-clean)
# rm -rf "${BUILD_DIR}"

exit "${SUITES_FAILED}"
