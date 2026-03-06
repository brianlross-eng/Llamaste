// test_inference.cpp -- Tests for inference integration helpers
//
// Compile (from project root):
//   g++ -std=c++17 -I src/llamaste -pthread -o tests/build/test_inference \
//     tests/test_inference.cpp src/llamaste/child_main.cpp \
//     src/llamaste/agent.cpp src/llamaste/prompt_builder.cpp \
//     src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
//     src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
//     src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
//     src/llamaste/tools_model.cpp src/llamaste/tools_install.cpp \
//     src/llamaste/tools_schedule.cpp src/llamaste/tools_auth.cpp \
//     src/llamaste/hwdetect.cpp src/llamaste/net_mdns.cpp \
//     src/llamaste/scheduler.cpp src/llamaste/bcrypt.cpp \
//     src/llamaste/auth.cpp

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#include "../src/llamaste/supervisor.h"
#include "../src/llamaste/json.hpp"

using json = nlohmann::json;

// Extern declarations for non-static functions in child_main.cpp
extern int compute_thread_count(int cpu_cores);
extern int compute_batch_thread_count(int cpu_cores);
extern int compute_context_size(int free_ram_mb);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

// --- Thread Count Tests ---
// Formula: max(1, cores * 3/4)

static void test_thread_count_1_core() {
    TEST("compute_thread_count(1) == 1");
    int t = compute_thread_count(1);
    if (t == 1) PASS(); else FAIL("expected 1");
}

static void test_thread_count_2_cores() {
    TEST("compute_thread_count(2) == 1");
    int t = compute_thread_count(2);
    if (t == 1) PASS(); else FAIL("expected 1");
}

static void test_thread_count_4_cores() {
    TEST("compute_thread_count(4) == 3");
    int t = compute_thread_count(4);
    if (t == 3) PASS(); else FAIL("expected 3");
}

static void test_thread_count_8_cores() {
    TEST("compute_thread_count(8) == 6");
    int t = compute_thread_count(8);
    if (t == 6) PASS(); else FAIL("expected 6");
}

static void test_thread_count_16_cores() {
    TEST("compute_thread_count(16) == 12");
    int t = compute_thread_count(16);
    if (t == 12) PASS(); else FAIL("expected 12");
}

// --- Batch Thread Count Tests ---

static void test_batch_threads_equals_cores() {
    TEST("compute_batch_thread_count returns cores");
    if (compute_batch_thread_count(1) == 1 &&
        compute_batch_thread_count(4) == 4 &&
        compute_batch_thread_count(8) == 8 &&
        compute_batch_thread_count(16) == 16)
        PASS();
    else
        FAIL("batch threads should equal cores");
}

// --- Context Window Tests ---

static void test_context_low_ram() {
    TEST("compute_context_size(<512) == 2048");
    int c = compute_context_size(256);
    if (c == 2048) PASS(); else FAIL("expected 2048");
}

static void test_context_medium_ram() {
    TEST("compute_context_size(512-1024) == 2048");
    int c = compute_context_size(768);
    if (c == 2048) PASS(); else FAIL("expected 2048");
}

static void test_context_high_ram() {
    TEST("compute_context_size(1024-2048) == 4096");
    int c = compute_context_size(1500);
    if (c == 4096) PASS(); else FAIL("expected 4096");
}

static void test_context_very_high_ram() {
    TEST("compute_context_size(>4096) == 8192");
    int c = compute_context_size(4096);
    if (c == 8192) PASS(); else FAIL("expected 8192");
}

int main() {
    printf("=== Inference Integration Tests ===\n");

    // Thread count
    test_thread_count_1_core();
    test_thread_count_2_cores();
    test_thread_count_4_cores();
    test_thread_count_8_cores();
    test_thread_count_16_cores();

    // Batch threads
    test_batch_threads_equals_cores();

    // Context window
    test_context_low_ram();
    test_context_medium_ram();
    test_context_high_ram();
    test_context_very_high_ram();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
