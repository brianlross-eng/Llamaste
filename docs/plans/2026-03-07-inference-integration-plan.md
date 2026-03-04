# Task 20: Real Inference Integration — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace stub_inference with real llama.cpp inference by spawning llama-server as a child process on localhost:8088 and proxying HTTP requests to it.

**Architecture:** llama-server runs as a separate process, spawned by child_main after tools are registered. A global `g_inference_fn` callback starts as `stub_inference` and swaps to `llama_inference` when a model loads successfully. If no model is present, the system remains in stub mode — fully functional web UI with canned responses.

**Tech Stack:** C++17, httplib (vendored), llama.cpp/llama-server (new Buildroot package), POSIX fork/exec/waitpid

**Design doc:** `docs/plans/2026-03-06-inference-integration-design.md`

---

### Task 1: Create llama-server Buildroot Package

**Files:**
- Create: `br2-external/package/llama-server/Config.in`
- Create: `br2-external/package/llama-server/llama-server.mk`
- Modify: `br2-external/Config.in` (line 1 — add source line)
- Modify: `br2-external/configs/llamaste_x86_64_defconfig` (after line 40 — add package enable)

**Step 1: Create the package Config.in**

Create `br2-external/package/llama-server/Config.in`:

```
config BR2_PACKAGE_LLAMA_SERVER
	bool "llama-server"
	help
	  llama.cpp inference server. Provides OpenAI-compatible
	  HTTP API for LLM inference with GGUF models.
	  https://github.com/ggerganov/llama.cpp
```

**Step 2: Create the package Makefile**

Create `br2-external/package/llama-server/llama-server.mk`:

```makefile
################################################################################
#
# llama-server — llama.cpp inference server
#
################################################################################

LLAMA_SERVER_VERSION = b5460
LLAMA_SERVER_SITE = https://github.com/ggerganov/llama.cpp/archive/refs/tags/$(LLAMA_SERVER_VERSION).tar.gz
LLAMA_SERVER_LICENSE = MIT
LLAMA_SERVER_LICENSE_FILES = LICENSE

LLAMA_SERVER_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DGGML_STATIC=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_CPU=ON \
	-DGGML_CPU_ALL_VARIANTS=ON \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_METAL=OFF \
	-DGGML_RPC=OFF \
	-DGGML_BLAS=OFF \
	-DLLAMA_CURL=OFF \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=OFF \
	-DLLAMA_BUILD_SERVER=ON

define LLAMA_SERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/buildroot-build/bin/llama-server \
		$(TARGET_DIR)/opt/llamaste/llama-server
endef

$(eval $(cmake-package))
```

**Step 3: Add llama-server to br2-external/Config.in**

Current content (1 line):
```
source "$BR2_EXTERNAL_LLAMASTE_PATH/package/llamaste/Config.in"
```

Add after it:
```
source "$BR2_EXTERNAL_LLAMASTE_PATH/package/llama-server/Config.in"
```

Note: `external.mk` uses a wildcard (`include $(sort $(wildcard .../*/*.mk))`) so no change needed there.

**Step 4: Enable llama-server in defconfig**

In `br2-external/configs/llamaste_x86_64_defconfig`, add after `BR2_PACKAGE_LLAMASTE=y` (line 40):
```
BR2_PACKAGE_LLAMA_SERVER=y
```

**Step 5: Commit**

```bash
git add br2-external/package/llama-server/Config.in \
       br2-external/package/llama-server/llama-server.mk \
       br2-external/Config.in \
       br2-external/configs/llamaste_x86_64_defconfig
git commit -m "feat: add llama-server Buildroot package (llama.cpp b5460)"
```

---

### Task 2: Write Inference Helper Functions + Tests (TDD)

**Files:**
- Create: `tests/test_inference.cpp`
- Modify: `src/llamaste/child_main.cpp` (add helper functions after line 92)
- Modify: `scripts/host-test.sh` (add Suite 8)

**Step 1: Write the test file**

Create `tests/test_inference.cpp`:

```cpp
// test_inference.cpp -- Tests for inference integration
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

// Extern declarations for functions defined in child_main.cpp
extern int compute_thread_count(int cpu_cores);
extern int compute_batch_thread_count(int cpu_cores);
extern int compute_context_size(int free_ram_mb);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

// --- Thread Count Tests ---

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
    TEST("compute_context_size(512-1024) == 4096");
    int c = compute_context_size(768);
    if (c == 4096) PASS(); else FAIL("expected 4096");
}

static void test_context_high_ram() {
    TEST("compute_context_size(1024-2048) == 8192");
    int c = compute_context_size(1500);
    if (c == 8192) PASS(); else FAIL("expected 8192");
}

static void test_context_very_high_ram() {
    TEST("compute_context_size(>2048) == 16384");
    int c = compute_context_size(4096);
    if (c == 16384) PASS(); else FAIL("expected 16384");
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
```

**Step 2: Run test — verify it fails to compile**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -pthread -o tests/build/test_inference \
  tests/test_inference.cpp src/llamaste/child_main.cpp \
  src/llamaste/agent.cpp src/llamaste/prompt_builder.cpp \
  src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
  src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
  src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
  src/llamaste/tools_model.cpp src/llamaste/tools_install.cpp \
  src/llamaste/tools_schedule.cpp src/llamaste/tools_auth.cpp \
  src/llamaste/hwdetect.cpp src/llamaste/net_mdns.cpp \
  src/llamaste/scheduler.cpp src/llamaste/bcrypt.cpp \
  src/llamaste/auth.cpp
```

Expected: FAIL — `compute_thread_count`, `compute_batch_thread_count`, `compute_context_size` undefined.

**Step 3: Implement the helper functions**

In `src/llamaste/child_main.cpp`, add after the globals block (after line 92, before `// ---------------------------------------------------------------------------`):

```cpp
// ---------------------------------------------------------------------------
// Inference configuration helpers (non-static for testability)
// ---------------------------------------------------------------------------

// Thread count for generation: max(1, cores * 3/4)
int compute_thread_count(int cpu_cores) {
    int t = cpu_cores * 3 / 4;
    return t < 1 ? 1 : t;
}

// Batch thread count: use all cores
int compute_batch_thread_count(int cpu_cores) {
    return cpu_cores < 1 ? 1 : cpu_cores;
}

// Context window size based on free RAM after model loading
int compute_context_size(int free_ram_mb) {
    if (free_ram_mb >= 2048) return 16384;
    if (free_ram_mb >= 1024) return 8192;
    if (free_ram_mb >= 512)  return 4096;
    return 2048;
}
```

**Step 4: Run tests — verify they pass**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -pthread -o tests/build/test_inference \
  tests/test_inference.cpp src/llamaste/child_main.cpp \
  src/llamaste/agent.cpp src/llamaste/prompt_builder.cpp \
  src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
  src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
  src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
  src/llamaste/tools_model.cpp src/llamaste/tools_install.cpp \
  src/llamaste/tools_schedule.cpp src/llamaste/tools_auth.cpp \
  src/llamaste/hwdetect.cpp src/llamaste/net_mdns.cpp \
  src/llamaste/scheduler.cpp src/llamaste/bcrypt.cpp \
  src/llamaste/auth.cpp && tests/build/test_inference
```

Expected: 10 passed, 0 failed.

**Step 5: Commit**

```bash
git add tests/test_inference.cpp src/llamaste/child_main.cpp
git commit -m "feat: add inference thread/context helper functions with tests"
```

---

### Task 3: Add Global Inference State + llama_inference Function

**Files:**
- Modify: `src/llamaste/child_main.cpp` (globals at ~line 92, new function after helpers)

**Step 1: Add inference globals**

In `src/llamaste/child_main.cpp`, add after `g_cage_pid` declaration (after line 92):

```cpp
// ---------------------------------------------------------------------------
// Inference backend state
// ---------------------------------------------------------------------------

// Inference function pointer: starts as stub, swapped to llama_inference on model load
static std::function<std::string(const std::string&)> g_inference_fn = stub_inference;

// llama-server process state
#ifndef _WIN32
static std::atomic<pid_t> g_llama_pid{0};
#endif
static std::atomic<bool> g_model_loaded{false};
static int g_llama_port = 8088;
static std::string g_model_name;
static std::mutex g_llama_mutex;
```

**Step 2: Add the llama_inference HTTP proxy function**

Add after the `compute_context_size` function (before `stub_inference`):

```cpp
// ---------------------------------------------------------------------------
// Real inference: HTTP proxy to llama-server on localhost
// ---------------------------------------------------------------------------

static std::string llama_inference(const std::string& request_json) {
    if (!g_model_loaded.load()) {
        return stub_inference(request_json);  // graceful fallback
    }

    httplib::Client cli("127.0.0.1", g_llama_port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(120);  // large models may take time

    auto result = cli.Post("/v1/chat/completions",
                           request_json,
                           "application/json");

    if (!result || result->status != 200) {
        // Return error as a well-formed chat completion
        json response;
        json choice;
        json msg;
        msg["role"] = "assistant";

        if (!result) {
            msg["content"] = "[inference error: llama-server unreachable]";
        } else {
            msg["content"] = "[inference error: llama-server returned HTTP " +
                             std::to_string(result->status) + "]";
        }

        choice["index"] = 0;
        choice["message"] = msg;
        choice["finish_reason"] = "stop";
        response["id"] = "chatcmpl-error";
        response["object"] = "chat.completion";
        response["model"] = "llamaste-error";
        response["choices"] = json::array({choice});
        json usage;
        usage["prompt_tokens"] = 0;
        usage["completion_tokens"] = 0;
        usage["total_tokens"] = 0;
        response["usage"] = usage;
        return response.dump();
    }

    return result->body;
}
```

**Step 3: Verify compilation**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -pthread -o tests/build/test_inference \
  tests/test_inference.cpp src/llamaste/child_main.cpp \
  src/llamaste/agent.cpp src/llamaste/prompt_builder.cpp \
  src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
  src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
  src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
  src/llamaste/tools_model.cpp src/llamaste/tools_install.cpp \
  src/llamaste/tools_schedule.cpp src/llamaste/tools_auth.cpp \
  src/llamaste/hwdetect.cpp src/llamaste/net_mdns.cpp \
  src/llamaste/scheduler.cpp src/llamaste/bcrypt.cpp \
  src/llamaste/auth.cpp && tests/build/test_inference
```

Expected: compiles and 10 tests still pass.

**Step 4: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: add g_inference_fn global and llama_inference HTTP proxy"
```

---

### Task 4: Add llama-server Process Lifecycle Management

**Files:**
- Modify: `src/llamaste/child_main.cpp` (add spawn + monitor functions, wire into child_main)

This task adds the Linux-only process management code. It's guarded by `#ifndef _WIN32` so host tests still compile on WSL.

**Step 1: Add the spawn function**

Add after `llama_inference()`, before `stub_inference()`:

```cpp
#ifndef _WIN32
#include <sys/wait.h>

// ---------------------------------------------------------------------------
// llama-server process lifecycle
// ---------------------------------------------------------------------------

static bool spawn_llama_server(const std::string& model_path, int cpu_cores, int free_ram_mb) {
    std::lock_guard<std::mutex> lock(g_llama_mutex);

    int threads = compute_thread_count(cpu_cores);
    int batch_threads = compute_batch_thread_count(cpu_cores);
    int context = compute_context_size(free_ram_mb);

    std::string t_str = std::to_string(threads);
    std::string tb_str = std::to_string(batch_threads);
    std::string c_str = std::to_string(context);
    std::string port_str = std::to_string(g_llama_port);

    fprintf(stderr, "[child] Spawning llama-server: model=%s threads=%d batch=%d ctx=%d port=%d\n",
            model_path.c_str(), threads, batch_threads, context, g_llama_port);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[child] Failed to fork llama-server: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        // Child process: exec llama-server
        execl("/opt/llamaste/llama-server", "llama-server",
              "-m", model_path.c_str(),
              "--host", "127.0.0.1",
              "--port", port_str.c_str(),
              "--no-webui",
              "-c", c_str.c_str(),
              "-t", t_str.c_str(),
              "-tb", tb_str.c_str(),
              "--mlock",
              "-fa",
              "--log-disable",
              "--chat-template", "chatml",
              (char*)nullptr);
        // exec failed
        fprintf(stderr, "[child] execl llama-server failed: %s\n", strerror(errno));
        _exit(127);
    }

    // Parent: store PID
    g_llama_pid.store(pid);
    fprintf(stderr, "[child] llama-server spawned with PID %d\n", pid);
    return true;
}

// Poll llama-server /health until ready (or timeout)
static bool wait_for_llama_server(int timeout_seconds) {
    httplib::Client cli("127.0.0.1", g_llama_port);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(5);

    auto start = std::chrono::steady_clock::now();
    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_seconds) {
            fprintf(stderr, "[child] llama-server health timeout after %ds\n", timeout_seconds);
            return false;
        }

        auto result = cli.Get("/health");
        if (result && result->status == 200) {
            // Parse response to check if model is loaded
            auto body = json::parse(result->body, nullptr, false);
            if (!body.is_discarded()) {
                std::string status = body.value("status", "");
                if (status == "ok" || status == "no slot available") {
                    fprintf(stderr, "[child] llama-server healthy after %llds\n",
                            (long long)elapsed);
                    return true;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// Background thread: monitor llama-server PID, respawn on crash
static void llama_monitor_thread(std::string model_path, int cpu_cores, int free_ram_mb) {
    int retries = 0;
    const int max_retries = 3;

    while (g_running.load() && retries < max_retries) {
        pid_t pid = g_llama_pid.load();
        if (pid <= 0) break;

        int status = 0;
        pid_t result = waitpid(pid, &status, 0);
        if (result <= 0) break;

        if (!g_running.load()) break;  // shutting down, don't respawn

        // llama-server exited unexpectedly
        if (WIFEXITED(status)) {
            fprintf(stderr, "[child] llama-server exited with code %d\n",
                    WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[child] llama-server killed by signal %d\n",
                    WTERMSIG(status));
        }

        g_model_loaded.store(false);
        g_inference_fn = stub_inference;
        retries++;

        if (retries >= max_retries) {
            fprintf(stderr, "[child] llama-server crashed %d times, staying in stub mode\n",
                    max_retries);
            break;
        }

        fprintf(stderr, "[child] Respawning llama-server (attempt %d/%d) in 2s...\n",
                retries + 1, max_retries);
        std::this_thread::sleep_for(std::chrono::seconds(2));

        if (spawn_llama_server(model_path, cpu_cores, free_ram_mb)) {
            if (wait_for_llama_server(60)) {
                g_model_loaded.store(true);
                g_inference_fn = llama_inference;
                retries = 0;  // reset on successful restart
                fprintf(stderr, "[child] llama-server recovered successfully\n");
            }
        }
    }
}

#endif // _WIN32
```

**Step 2: Wire lifecycle into child_main()**

In the `child_main()` function, after hardware detection (after line 658, before system prompt build at line 661), add the llama-server startup block:

```cpp
    // Start llama-server if a model is available
#ifndef _WIN32
    if (!config.model_path.empty()) {
        // Extract model filename for display
        std::string model_file = config.model_path;
        auto slash = model_file.rfind('/');
        if (slash != std::string::npos) model_file = model_file.substr(slash + 1);
        g_model_name = model_file;

        // Estimate free RAM after model load (rough: assume model uses 60% of file size in RAM)
        // For now use detected free RAM minus a conservative reserve
        int free_ram_estimate = g_hwinfo.ram_free_mb - 512;  // reserve for system
        if (config.boot_mode == "desktop") free_ram_estimate -= 500;  // reserve for compositor

        if (spawn_llama_server(config.model_path, config.cpu_cores, free_ram_estimate)) {
            auto load_start = std::chrono::steady_clock::now();
            if (wait_for_llama_server(60)) {
                auto load_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - load_start).count();
                g_model_loaded.store(true);
                g_inference_fn = llama_inference;
                fprintf(stderr, "[child] Model loaded: %s (%.1fs)\n",
                        model_file.c_str(), load_time / 1000.0);
            } else {
                fprintf(stderr, "[child] llama-server failed health check, staying in stub mode\n");
            }

            // Start monitor thread for crash recovery
            std::thread monitor(llama_monitor_thread, config.model_path,
                               config.cpu_cores, free_ram_estimate);
            monitor.detach();
        } else {
            fprintf(stderr, "[child] Failed to spawn llama-server, staying in stub mode\n");
        }
    }
#endif
```

**Step 3: Add cleanup on shutdown**

Find the shutdown handling in child_main (where `g_running = false` is set or before `return 0`). Add llama-server termination. Look for the end of child_main's server loop. Add before the function's final return:

```cpp
    // Stop llama-server
#ifndef _WIN32
    pid_t llama_pid = g_llama_pid.load();
    if (llama_pid > 0) {
        fprintf(stderr, "[child] Stopping llama-server (PID %d)\n", llama_pid);
        kill(llama_pid, SIGTERM);
        // Wait up to 5 seconds for graceful exit
        for (int i = 0; i < 50; i++) {
            int status;
            if (waitpid(llama_pid, &status, WNOHANG) != 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(llama_pid, SIGKILL);  // force if still alive
        g_llama_pid.store(0);
    }
#endif
```

**Step 4: Verify compilation + existing tests still pass**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -pthread -o tests/build/test_inference \
  tests/test_inference.cpp src/llamaste/child_main.cpp \
  src/llamaste/agent.cpp src/llamaste/prompt_builder.cpp \
  src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
  src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
  src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
  src/llamaste/tools_model.cpp src/llamaste/tools_install.cpp \
  src/llamaste/tools_schedule.cpp src/llamaste/tools_auth.cpp \
  src/llamaste/hwdetect.cpp src/llamaste/net_mdns.cpp \
  src/llamaste/scheduler.cpp src/llamaste/bcrypt.cpp \
  src/llamaste/auth.cpp && tests/build/test_inference
```

Expected: compiles and 10 tests pass.

**Step 5: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: add llama-server lifecycle management (spawn, health poll, crash recovery)"
```

---

### Task 5: Wire Up g_inference_fn at All Call Sites

**Files:**
- Modify: `src/llamaste/child_main.cpp` (4 locations)

**Step 1: Replace stub_inference in SSE streaming handler (line 459)**

Change:
```cpp
std::string final_text = agent_turn(conv, g_tools, stub_inference);
```
To:
```cpp
std::string final_text = agent_turn(conv, g_tools, g_inference_fn);
```

**Step 2: Replace stub_inference in non-streaming handler (line 498)**

Change:
```cpp
std::string result = agent_turn(conv, g_tools, stub_inference);
```
To:
```cpp
std::string result = agent_turn(conv, g_tools, g_inference_fn);
```

**Step 3: Replace stub_inference in OpenAI completions handler (line 599)**

Change:
```cpp
std::string response = stub_inference(request);
```
To:
```cpp
std::string response = g_inference_fn(request);
```

**Step 4: Replace stub_inference in scheduler callback (line 680)**

Change:
```cpp
return agent_turn(conv, g_tools, stub_inference);
```
To:
```cpp
return agent_turn(conv, g_tools, g_inference_fn);
```

**Step 5: Verify no remaining references to stub_inference as a call target**

Search for `stub_inference` — it should only appear in:
1. The function definition itself
2. The `g_inference_fn` default initialization (`= stub_inference`)
3. The fallback in `llama_inference()` (`return stub_inference(...)`)
4. The crash recovery code (`g_inference_fn = stub_inference`)

NOT as a direct call argument to `agent_turn` or direct call anywhere else.

**Step 6: Run existing test_http suite to verify nothing broke**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -pthread -o tests/build/test_http \
  tests/test_http.cpp src/llamaste/child_main.cpp \
  src/llamaste/agent.cpp src/llamaste/prompt_builder.cpp \
  src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
  src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
  src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
  src/llamaste/tools_model.cpp src/llamaste/tools_install.cpp \
  src/llamaste/tools_schedule.cpp src/llamaste/tools_auth.cpp \
  src/llamaste/hwdetect.cpp src/llamaste/net_mdns.cpp \
  src/llamaste/scheduler.cpp src/llamaste/bcrypt.cpp \
  src/llamaste/auth.cpp && tests/build/test_http
```

Expected: all 15 HTTP tests pass (they use stub mode since no model path is set).

**Step 7: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: wire g_inference_fn at all 4 inference call sites"
```

---

### Task 6: Update Health Endpoint + Model Status

**Files:**
- Modify: `src/llamaste/child_main.cpp` (health handler at line 604)

**Step 1: Update the health endpoint**

Replace the `handle_health` function (lines 604-613):

```cpp
static void handle_health(const httplib::Request& /*req*/, httplib::Response& res) {
    json health;
    health["status"] = "ok";
    health["uptime_seconds"] = static_cast<int>(time(nullptr) - g_start_time);
    health["model_loaded"] = g_model_loaded.load();
    health["model_name"] = g_model_name.empty() ? json(nullptr) : json(g_model_name);
    health["inference_ready"] = g_model_loaded.load();
    health["tools_count"] = g_tools.count();
    health["mode"] = g_boot_mode;
    res.set_content(health.dump(), "application/json");
}
```

**Step 2: Verify test_http still passes**

```bash
cd /mnt/d/Llamaste && tests/build/test_http
```

Expected: 15 tests pass. Health endpoint tests should pass since `g_model_loaded` defaults to `false` (same as before — `model_loaded: false`).

**Step 3: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: health endpoint reports real model/inference status"
```

---

### Task 7: Add Inference Tests to Host Test Suite

**Files:**
- Modify: `scripts/host-test.sh` (add Suite 8)

**Step 1: Update host-test.sh**

Change the suite count from `[N/7]` to `[N/8]` in all existing suite headers (lines 62, 78, 101, 127, 164, 198, 215).

Add the new suite before the Results section (before line 231):

```bash
# ---------------------------------------------------------------
# Suite 8: Inference Integration
# ---------------------------------------------------------------
echo -e "${BOLD}--- [8/8] Inference Integration ---${NC}"
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
    "${SRC}/tools_install.cpp" \
    "${SRC}/tools_schedule.cpp" \
    "${SRC}/tools_auth.cpp" \
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
```

**Step 2: Run the full test suite**

```bash
cd /mnt/d/Llamaste && bash scripts/host-test.sh
```

Expected: **ALL 8 TEST SUITES PASSED** (118+ tests total).

**Step 3: Commit**

```bash
git add scripts/host-test.sh
git commit -m "test: add inference integration suite to host-test.sh (8/8 suites)"
```

---

### Task 8: Final Verification + Documentation Update

**Files:**
- Modify: `SESSION-STATUS.md`
- Modify: `DEVELOPER.md` (add inference section if needed)

**Step 1: Run the full test suite one more time**

```bash
cd /mnt/d/Llamaste && bash scripts/host-test.sh
```

Expected: ALL 8 SUITES PASSED.

**Step 2: Verify no stub_inference direct calls remain**

```bash
grep -n "stub_inference" src/llamaste/child_main.cpp
```

Expected hits (only these):
- Function definition (`static std::string stub_inference(`)
- Default for g_inference_fn (`= stub_inference;`)
- Fallback in llama_inference (`return stub_inference(request_json);`)
- Crash recovery fallback (`g_inference_fn = stub_inference;`)

NO hits for `agent_turn(..., stub_inference)` or direct call `stub_inference(request)`.

**Step 3: Update SESSION-STATUS.md**

Update Task 20 status from "DESIGN DONE" to "DONE":
- Update the table row for 2d: Real Inference
- Update the "Next Steps" section
- Add test count (118+ tests across 8 suites)

**Step 4: Commit**

```bash
git add SESSION-STATUS.md
git commit -m "docs: mark Task 20 inference integration complete"
```

---

## Summary

| Task | What | New Lines | Files |
|------|------|-----------|-------|
| 1 | Buildroot package | ~50 | 4 files (2 new, 2 modified) |
| 2 | Helper functions + tests | ~130 | 2 files (1 new, 1 modified) |
| 3 | Globals + llama_inference | ~60 | 1 file |
| 4 | Process lifecycle | ~150 | 1 file |
| 5 | Wire g_inference_fn | ~4 | 1 file |
| 6 | Health endpoint | ~5 | 1 file |
| 7 | Test suite update | ~25 | 1 file |
| 8 | Final verification | ~10 | 1 file |
| **Total** | | **~435** | **6 files (3 new)** |

**Note:** The Buildroot build itself (compiling llama.cpp) is NOT part of this plan. That requires WSL2 and takes ~5 minutes. Run it after all code changes are committed:

```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && export FORCE_UNSAFE_CONFIGURE=1 && cd /root/llamaste-build/output && make llama-server-dirclean && make llama-server && make llamaste-dirclean && make llamaste && make"
```
