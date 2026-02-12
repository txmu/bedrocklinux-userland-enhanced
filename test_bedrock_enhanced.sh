#!/bin/bash

# ==============================================================================
# Bedrock Linux (Enhanced Branch) Automated Feature Test Suite
# ==============================================================================

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

TEST_STRATUM="test-alpine"
TEST_IMPORT_STRATUM="test-import"
LOG_FILE="/tmp/bedrock_test.log"

# --- Helper Functions ---

log() {
    echo -e "$1"
    echo "[$(date '+%H:%M:%S')] $(echo "$1" | sed 's/\x1b\[[0-9;]*m//g')" >> "$LOG_FILE"
}

pass() {
    log "${GREEN}[PASS]${NC} $1"
}

fail() {
    log "${RED}[FAIL]${NC} $1"
    echo "    Check $LOG_FILE for details."
    # We don't exit immediately to try running other tests
    TESTS_FAILED=$((TESTS_FAILED+1))
}

skip() {
    log "${YELLOW}[SKIP]${NC} $1"
}

header() {
    log "\n${BLUE}=== Testing: $1 ===${NC}"
}

check_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo -e "${RED}Error: This script must be run as root.${NC}"
        exit 1
    fi
}

cleanup() {
    log "\n${BLUE}=== Cleaning Up ===${NC}"
    if brl list | grep -q "^${TEST_STRATUM}$"; then
        log "Removing test stratum: ${TEST_STRATUM}..."
        brl disable "${TEST_STRATUM}" >/dev/null 2>&1
        brl remove "${TEST_STRATUM}" >/dev/null 2>&1
    fi
    if brl list | grep -q "^${TEST_IMPORT_STRATUM}$"; then
        log "Removing imported stratum: ${TEST_IMPORT_STRATUM}..."
        brl disable "${TEST_IMPORT_STRATUM}" >/dev/null 2>&1
        brl remove "${TEST_IMPORT_STRATUM}" >/dev/null 2>&1
    fi
    rm -rf /tmp/bedrock-adhoc-test
    log "Done."
}

# --- Main Test Logic ---

check_root
trap cleanup EXIT
TESTS_FAILED=0
: > "$LOG_FILE"

log "Starting Bedrock Linux Enhanced Verification..."
log "Kernel: $(uname -r)"
log "Bedrock Version: $(brl version)"

# ------------------------------------------------------------------------------
# 0. Preparation
# ------------------------------------------------------------------------------
header "0. Preparation"

if ! brl list | grep -q "^${TEST_STRATUM}$"; then
    log "Fetching ${TEST_STRATUM} (alpine) for testing..."
    brl fetch -n "${TEST_STRATUM}" alpine >> "$LOG_FILE" 2>&1
    if [ $? -eq 0 ]; then
        pass "Fetched ${TEST_STRATUM}"
    else
        fail "Could not fetch ${TEST_STRATUM}. Aborting tests requiring it."
        exit 1
    fi
else
    pass "${TEST_STRATUM} already exists"
fi

# ------------------------------------------------------------------------------
# 1. Test strat Ephemeral Mode (-E)
# ------------------------------------------------------------------------------
header "1. strat Ephemeral Mode (-E)"
# Concept: Write a file inside ephemeral strat. It should not exist after exit.

TEST_FILE="/etc/bedrock_ephemeral_test"
strat -E "${TEST_STRATUM}" sh -c "touch ${TEST_FILE}" >> "$LOG_FILE" 2>&1

if [ -f "/bedrock/strata/${TEST_STRATUM}${TEST_FILE}" ]; then
    fail "File created in Ephemeral mode persisted to disk!"
    rm "/bedrock/strata/${TEST_STRATUM}${TEST_FILE}"
else
    pass "Ephemeral changes were successfully discarded"
fi

# ------------------------------------------------------------------------------
# 2. Test strat Protected/Pledge Mode (-P)
# ------------------------------------------------------------------------------
header "2. strat Protected Mode (-P)"
# Concept: Network should be unreachable, /home should be empty tmpfs.

# Test Network Isolation
if strat -P "${TEST_STRATUM}" ping -c 1 1.1.1.1 >/dev/null 2>&1; then
    fail "Protected mode allowed network access (Ping succeeded)"
else
    pass "Protected mode blocked network access"
fi

# Test Home Isolation
# Create a marker in real home
MARKER="/home/bedrock_test_marker"
touch "$MARKER"
if strat -P "${TEST_STRATUM}" ls "$MARKER" >/dev/null 2>&1; then
    fail "Protected mode leaked /home contents"
else
    pass "Protected mode masked /home"
fi
rm -f "$MARKER"

# ------------------------------------------------------------------------------
# 3. Test strat Ad-Hoc Path Support
# ------------------------------------------------------------------------------
header "3. strat Ad-Hoc Path Support"
# Concept: Create a fake root dir and strat into it without registering it.

ADHOC_DIR="/tmp/bedrock-adhoc-test"
mkdir -p "${ADHOC_DIR}/bin"
cp /bedrock/libexec/busybox "${ADHOC_DIR}/bin/sh"

RESULT=$(/bedrock/bin/strat "${ADHOC_DIR}" /bin/sh -c "echo working")

if [ "$RESULT" == "working" ]; then
    pass "Ad-Hoc directory execution worked"
else
    fail "Ad-Hoc directory execution failed. Output: $RESULT"
fi

# ------------------------------------------------------------------------------
# 4. Test strat Rootless Mode (-R)
# ------------------------------------------------------------------------------
header "4. strat Rootless Mode (-R)"
# Concept: Switch to 'nobody', run strat -R, verifying we appear as root inside.

if ! id nobody >/dev/null 2>&1; then
    skip "User 'nobody' not found, skipping rootless test"
else
    # We use 'su' to drop privs, then run strat -R, then run 'id -u' inside.
    # Inside the namespace, id -u should be 0.
    INNER_UID=$(su -s /bin/sh nobody -c "/bedrock/bin/strat -R ${TEST_STRATUM} id -u")
    
    if [ "$INNER_UID" == "0" ]; then
        pass "Rootless mode successfully mapped user to root (0) inside container"
    else
        fail "Rootless mode failed. UID inside was: $INNER_UID"
    fi
fi

# ------------------------------------------------------------------------------
# 5. Test brl-do (Parallel Execution)
# ------------------------------------------------------------------------------
header "5. brl-do Functionality"

# Run echo across all enabled strata
OUTPUT=$(/bedrock/libexec/brl-do echo "BedrockTest")

# Check if output contains our test stratum and the echo result
if echo "$OUTPUT" | grep -q "\[${TEST_STRATUM}\] Executing:" && echo "$OUTPUT" | grep -q "BedrockTest"; then
    pass "brl-do executed command across strata"
else
    fail "brl-do failed to produce expected output"
fi

# ------------------------------------------------------------------------------
# 6. Test brl-service (Abstraction)
# ------------------------------------------------------------------------------
header "6. brl-service Abstraction"

# Alpine uses openrc. Let's try to query a non-existent service status.
# It should attempt to run 'rc-service' or similar inside the stratum.
# We capture stderr/stdout to ensure it tries to run the init system command.

SERVICE_OUT=$(/bedrock/libexec/brl-service "${TEST_STRATUM}" "dummy-service" "status" 2>&1)

if echo "$SERVICE_OUT" | grep -qE "does not exist|not found|rc-service"; then
    pass "brl-service correctly invoked underlying init system"
else
    fail "brl-service output unexpected: $SERVICE_OUT"
fi

# ------------------------------------------------------------------------------
# 7. Test brl-import (Container)
# ------------------------------------------------------------------------------
header "7. brl-import (Container Source)"

# Check for container engine
ENGINE=""
if command -v podman >/dev/null; then ENGINE="podman"; fi
if command -v docker >/dev/null; then ENGINE="docker"; fi

if [ -z "$ENGINE" ]; then
    skip "No docker/podman found. Skipping import test."
else
    log "Using $ENGINE to pull a small image (busybox)..."
    $ENGINE pull busybox:latest >/dev/null 2>&1
    
    log "Attempting brl import..."
    /bedrock/libexec/brl-import "${TEST_IMPORT_STRATUM}" "$ENGINE:busybox:latest" >> "$LOG_FILE" 2>&1
    
    if brl list | grep -q "^${TEST_IMPORT_STRATUM}$"; then
        pass "Successfully imported stratum from $ENGINE"
        # Verify it works
        if strat "${TEST_IMPORT_STRATUM}" true; then
            pass "Imported stratum is functional"
        else
            fail "Imported stratum exists but execution failed"
        fi
    else
        fail "brl-import failed to create stratum"
    fi
fi

# ------------------------------------------------------------------------------
# 8. Test Command Not Found Suggestion
# ------------------------------------------------------------------------------
header "8. Command Not Found Suggestion"

# Run a command that definitely doesn't exist locally but might be found via pmm suggestion logic
# We are just checking if the "Tip:" message appears on stderr/stdout when calling strat directly fails
# Note: This feature triggers when strat fails with ENOENT on arg0.

OUTPUT=$(/bedrock/bin/strat bedrock /bin/non_existent_command_test_123 2>&1)

if echo "$OUTPUT" | grep -q "Tip: Command not found locally"; then
    pass "Smart suggestion tip displayed"
else
    log "Output was: $OUTPUT"
    fail "Smart suggestion tip NOT displayed"
fi

# ------------------------------------------------------------------------------
# 9. Test brl-kmon (Kernel Monitor)
# ------------------------------------------------------------------------------
header "9. brl-kmon Daemon"

if pgrep -f "brl-kmon" >/dev/null; then
    pass "brl-kmon daemon is running"
else
    # It might not start automatically if not configured in bedrock.conf
    # Try to start it manually to verify binary works
    if /bedrock/libexec/brl-kmon >/dev/null 2>&1 & KMON_PID=$!; then
        sleep 1
        if ps -p $KMON_PID >/dev/null; then
            pass "brl-kmon binary starts successfully"
            kill $KMON_PID
        else
            fail "brl-kmon binary crashed immediately"
        fi
    else
        fail "brl-kmon binary not found or execution failed"
    fi
fi

# ------------------------------------------------------------------------------
# 10. pmm Parallel Search
# ------------------------------------------------------------------------------
header "10. pmm Parallel Search"

START_TIME=$(date +%s%N)
# Search for a common string. Output should be verbose if working correctly across multiple strata.
# Since we have bedrock and test-alpine, it should search both.
pmm search bash > /tmp/bedrock_pmm_test.out 2>&1
END_TIME=$(date +%s%N)
ELAPSED=$(( (END_TIME - START_TIME) / 1000000 ))

if grep -q "alpine" /tmp/bedrock_pmm_test.out; then
    pass "pmm search returned results from test stratum"
    log "Search took ${ELAPSED}ms"
else
    fail "pmm search failed to return results"
fi
rm -f /tmp/bedrock_pmm_test.out

# ==============================================================================
# Summary
# ==============================================================================
log "\n=========================================="
if [ "$TESTS_FAILED" -eq 0 ]; then
    echo -e "${GREEN}ALL TESTS PASSED!${NC} The enhanced features are working correctly."
else
    echo -e "${RED}$TESTS_FAILED TESTS FAILED.${NC} Please review the log."
fi
log "=========================================="

exit $TESTS_FAILED
