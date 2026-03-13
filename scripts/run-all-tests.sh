#!/bin/bash
# run-all-tests.sh -- Master test runner for Llamaste QEMU integration tests
#
# Runs all integration tests in order and prints a summary table.
#
# Usage: ./scripts/run-all-tests.sh [--quick] [--test NAME] [images-dir]
#   --quick       Skip model test (slow, downloads model)
#   --test NAME   Run only the named test (install|cluster|update|recovery|model)
#   images-dir    Path to images directory (default: ~/llamaste-build/output/images)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# --- Colors ---
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
NC='\033[0m'

# --- Argument parsing ---
QUICK=false
ONLY_TEST=""
IMAGES_DIR=""

for arg in "$@"; do
    case "$arg" in
        --quick)
            QUICK=true
            ;;
        --test)
            # Next argument will be captured by the shift trick below
            ;;
        *)
            # Check if previous arg was --test
            if [ "${_prev_arg:-}" = "--test" ]; then
                ONLY_TEST="$arg"
            else
                IMAGES_DIR="$arg"
            fi
            ;;
    esac
    _prev_arg="$arg"
done

# Default images dir
IMAGES_DIR="${IMAGES_DIR:-$HOME/llamaste-build/output/images}"
export IMAGES_DIR

# Derive paths for scripts that expect them via env
export IMAGE="${IMAGES_DIR}/llamaste.img"
export KERNEL="${IMAGES_DIR}/bzImage"
export ISO="${IMAGES_DIR}/llamaste.iso"

# --- Test definitions ---
# Order: install, cluster, update, recovery, model
declare -a TEST_NAMES=(install cluster update recovery model)

declare -A TEST_SCRIPTS
TEST_SCRIPTS[install]="${SCRIPT_DIR}/qemu-install-test-v2.sh"
TEST_SCRIPTS[cluster]="${SCRIPT_DIR}/qemu-cluster-test.sh"
TEST_SCRIPTS[update]="${SCRIPT_DIR}/qemu-update-test.sh"
TEST_SCRIPTS[recovery]="${SCRIPT_DIR}/qemu-recovery-test.sh"
TEST_SCRIPTS[model]="${SCRIPT_DIR}/qemu-model-test.sh"

# Dedicated port ranges per test to avoid WSL2/SLIRP port reuse issues.
# Each test gets a 20-port window so TIME_WAIT from previous tests never collides.
declare -A TEST_PORT_BASE
TEST_PORT_BASE[install]=9090
TEST_PORT_BASE[cluster]=9110
TEST_PORT_BASE[update]=9130
TEST_PORT_BASE[recovery]=9150
TEST_PORT_BASE[model]=9170

# --- Results tracking ---
declare -A results
num_passed=0
num_failed=0
num_skipped=0

# --- Header ---
echo ""
echo -e "${BOLD}============================================${NC}"
echo -e "${BOLD}  Llamaste Integration Test Suite${NC}"
echo -e "${BOLD}============================================${NC}"
echo ""
echo "  Images dir: ${IMAGES_DIR}"
echo "  Quick mode: ${QUICK}"
if [ -n "$ONLY_TEST" ]; then
    echo "  Single test: ${ONLY_TEST}"
fi
echo ""

# --- Validate --test argument ---
if [ -n "$ONLY_TEST" ]; then
    valid=false
    for name in "${TEST_NAMES[@]}"; do
        if [ "$name" = "$ONLY_TEST" ]; then
            valid=true
            break
        fi
    done
    if ! $valid; then
        echo -e "${RED}ERROR: Unknown test '${ONLY_TEST}'${NC}"
        echo "  Available tests: ${TEST_NAMES[*]}"
        exit 1
    fi
fi

# --- Run tests ---
for name in "${TEST_NAMES[@]}"; do
    # Skip if --test specified and this isn't it
    if [ -n "$ONLY_TEST" ] && [ "$name" != "$ONLY_TEST" ]; then
        results[$name]="skipped (not selected)"
        num_skipped=$((num_skipped + 1))
        continue
    fi

    # Skip model test in quick mode
    if $QUICK && [ "$name" = "model" ]; then
        results[$name]="skipped (--quick)"
        num_skipped=$((num_skipped + 1))
        echo -e "${YELLOW}--- Skipping: ${name} (--quick) ---${NC}"
        echo ""
        continue
    fi

    script="${TEST_SCRIPTS[$name]}"

    echo -e "${BOLD}--- Running: ${name} ---${NC}"
    echo ""

    # Ensure no QEMU processes are running from previous tests
    pkill -f qemu-system-x86_64 2>/dev/null || true
    sleep 2

    # Assign a dedicated port range for this test to avoid TIME_WAIT collisions
    export QEMU_PORT_BASE="${TEST_PORT_BASE[$name]}"
    echo "  Port range: ${QEMU_PORT_BASE}-$((QEMU_PORT_BASE + 19))"

    # Run the test script; capture exit code
    set +e
    bash "$script"
    rc=$?
    set -e

    # Kill any leftover QEMU processes
    pkill -f qemu-system-x86_64 2>/dev/null || true
    sleep 1

    if [ $rc -eq 0 ]; then
        results[$name]="PASS"
        num_passed=$((num_passed + 1))
    else
        results[$name]="FAIL (exit $rc)"
        num_failed=$((num_failed + 1))
    fi

    echo ""

    # Wait for QEMU ports to fully release and processes to clean up
    sleep 10
done

# --- Summary table ---
echo ""
echo -e "${BOLD}=== Llamaste Test Suite Results ===${NC}"

for name in "${TEST_NAMES[@]}"; do
    status="${results[$name]}"
    case "$status" in
        PASS)
            color="$GREEN"
            label="PASS"
            ;;
        FAIL*)
            color="$RED"
            label="$status"
            ;;
        skipped*)
            color="$YELLOW"
            label="SKIP (${status})"
            ;;
    esac
    printf "  %-14s ${color}%s${NC}\n" "${name}:" "$label"
done

echo ""
echo -e "  ${BOLD}TOTAL:${NC} ${GREEN}${num_passed} passed${NC}, ${RED}${num_failed} failed${NC}, ${YELLOW}${num_skipped} skipped${NC}"
echo -e "${BOLD}===================================${NC}"
echo ""

exit $num_failed
