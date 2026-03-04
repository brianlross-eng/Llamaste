# Task 20: Real Inference Integration — Design Document

**Date**: 2026-03-06
**Status**: Draft
**Depends on**: Phase 1 complete, Phase 2a-2c complete, Auth complete

---

## Problem Statement

Llamaste currently uses `stub_inference()` — a pattern-matching function that returns canned responses. The agent loop, tool system, web UI, and API endpoints are all wired up and working, but no actual LLM inference occurs. Task 20 replaces the stub with real llama.cpp inference so the system can generate genuine responses and execute multi-round tool-calling workflows.

---

## Architecture Decision: HTTP Proxy to llama-server

### Approach: Spawn llama-server as a child process, proxy HTTP requests to it

**Why this approach over embedding llama.cpp as a library:**

1. **Separation of concerns** — llama-server is a mature, battle-tested inference server. Embedding ggml/llama as a library into our binary would require linking ~50 source files, managing threading, memory mapping, and KV cache state directly. The HTTP API is clean and stable.

2. **Binary size** — Our llamaste binary is ~6 MB. llama-server is ~3-5 MB separately. Linking llama.cpp as a library would roughly double our binary size and add significant compilation complexity to the Buildroot package recipe.

3. **Process isolation** — If inference crashes (OOM, SIGFPE, bad model), only llama-server dies. The supervisor can restart it without losing the HTTP server, scheduler, auth state, or web UI.

4. **Hot reload** — Users can switch models without restarting the whole OS. Just kill and respawn llama-server with a different model path.

5. **Upstream compatibility** — llama.cpp updates frequently. Keeping it as a separate binary means we can update it independently of our codebase.

6. **Already in Buildroot** — llama.cpp has an unofficial Buildroot package recipe. We can adapt it or create our own `llama-server` package.

### Communication Path

```
Web UI / API client
    ↓ HTTP
child_main.cpp (port 80)
    ↓ inference_fn callback
llama_inference() function
    ↓ HTTP (localhost:8088)
llama-server process (port 8088)
    ↓
GGUF model in /data/models/
```

Port 8088 is chosen as an internal-only port, never exposed externally. Our HTTP server on port 80 acts as the gateway.

---

## Design

### 1. llama-server Lifecycle Management

**Startup sequence** (in `child_main()`, after tool registration):

```
1. Check config.model_path — if empty, stay in stub mode
2. Check if model file exists (access() check)
3. Spawn llama-server as a child process:
   llama-server -m <model_path> \
     --host 127.0.0.1 --port 8088 \
     --no-webui \
     -c 4096 \
     -t <cpu_cores - 1> \
     -tb <cpu_cores> \
     --mlock \
     -fa \
     --log-disable \
     --chat-template chatml
4. Poll http://127.0.0.1:8088/health until ready (up to 60s for large models)
5. Set global inference function to llama_inference()
6. Log: "[child] Model loaded: <filename> (<load_time>s)"
```

**Shutdown**: When child_main exits (SIGTERM), send SIGTERM to llama-server PID.

**Crash recovery**: A background thread monitors the llama-server PID. If it exits unexpectedly:
- Log the exit code
- Wait 2 seconds
- Respawn with same arguments
- Max 3 retries, then fall back to stub mode

### 2. Inference Function

Replace `stub_inference` with `llama_inference`:

```cpp
// Global state for llama-server connection
static std::atomic<bool> g_model_loaded{false};
static int g_llama_port = 8088;
static pid_t g_llama_pid = 0;
static std::mutex g_llama_mutex;

static std::string llama_inference(const std::string& request_json) {
    if (!g_model_loaded) {
        return stub_inference(request_json);  // graceful fallback
    }

    // Forward request to llama-server's /v1/chat/completions
    httplib::Client cli("127.0.0.1", g_llama_port);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(120);  // large models may take time

    auto res = cli.Post("/v1/chat/completions",
                        request_json,
                        "application/json");

    if (!res || res->status != 200) {
        // Fallback: return error as assistant message
        json response;
        json choice;
        json msg;
        msg["role"] = "assistant";
        msg["content"] = "[inference error: llama-server unreachable]";
        choice["index"] = 0;
        choice["message"] = msg;
        choice["finish_reason"] = "stop";
        response["id"] = "chatcmpl-error";
        response["object"] = "chat.completion";
        response["model"] = "llamaste-error";
        response["choices"] = json::array({choice});
        return response.dump();
    }

    return res->body;
}
```

### 3. Wiring Points (3 locations in child_main.cpp)

Replace `stub_inference` with `g_inference_fn`:

```cpp
// Global inference function (starts as stub, switches to llama when model loads)
static std::function<std::string(const std::string&)> g_inference_fn = stub_inference;
```

Then at the 3 call sites:
1. **SSE streaming** (line ~459): `agent_turn(conv, g_tools, g_inference_fn)`
2. **Non-streaming POST** (line ~498): `agent_turn(conv, g_tools, g_inference_fn)`
3. **Scheduler** (line ~680): `agent_turn(conv, g_tools, g_inference_fn)`

And the OpenAI `/v1/chat/completions` endpoint (line ~599):
4. `std::string response = g_inference_fn(request);`

### 4. llama-server Buildroot Package

New package `br2-external/package/llama-server/`:

```makefile
LLAMA_SERVER_VERSION = b5460
LLAMA_SERVER_SITE = https://github.com/ggerganov/llama.cpp/archive/refs/tags/$(LLAMA_SERVER_VERSION).tar.gz
LLAMA_SERVER_LICENSE = MIT

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

### 5. Model Auto-Selection

Already implemented in `main.cpp` (`select_model()`). The model path flows through `SupervisorConfig` to `child_main()`. No changes needed — just needs a model file in `/data/models/`.

### 6. Token Streaming (SSE)

Currently the SSE streaming simulates word-by-word delivery with `sleep(20ms)`. With real inference, we should stream actual tokens from llama-server:

**Phase 1 (this task)**: Keep the current word-splitting approach. The full response comes back from `agent_turn()` and gets chunked into SSE events. This is simpler and already works.

**Phase 2 (future)**: Use llama-server's native streaming (`"stream": true`) to emit tokens as they're generated. This requires modifying the inference callback to support streaming, which is a larger change.

### 7. Thread Tuning

llama-server command line arguments based on detected hardware:

```
Cores | -t (gen) | -tb (batch) | Notes
------|----------|-------------|------
  1   |    1     |      1      | Minimum viable
  2   |    1     |      2      | Leave 1 core for HTTP server
  4   |    3     |      4      | Good balance
  8   |    6     |      8      | Reserve 2 for HTTP + scheduler
 16   |   12     |     16      | Reserve 4 for system
```

Formula: `-t = max(1, cores * 3/4)`, `-tb = cores`

### 8. Context Window Sizing

Based on available RAM after model loading:

```
Free RAM after model | Context (-c)
--------------------|-------------
< 512 MB            | 2048
512 MB - 1 GB       | 4096
1-2 GB              | 8192
> 2 GB              | 16384
```

Larger context allows longer conversations and bigger tool results, but uses more RAM.

### 9. Health Endpoint Update

Update `/health` to report real model status:

```json
{
  "status": "ok",
  "uptime_seconds": 123,
  "model_loaded": true,
  "model_name": "qwen2.5-0.5b-instruct-q4_k_m.gguf",
  "tools_count": 32,
  "mode": "server",
  "inference_ready": true
}
```

---

## Testing Strategy

### Host Tests (new suite: test_inference.cpp)

1. **Stub fallback**: When no model path, inference_fn stays as stub_inference
2. **Error handling**: When llama-server is unreachable, return error message (don't crash)
3. **Request format**: Verify build_inference_request produces valid OpenAI format
4. **Response parsing**: Verify parse_tool_calls/parse_content work with real llama-server response formats
5. **Lifecycle**: spawn/kill/respawn logic (mock the process operations)

### QEMU E2E Tests

1. **Boot without model**: System boots, web UI works, stub mode
2. **Boot with model**: Download test model, place in /data/models/, reboot, verify real inference
3. **Tool calling**: Send a message that should trigger fs.list_directory, verify tool output in response

### Test Model

`qwen2.5-0.5b-instruct-q4_k_m.gguf` (~400 MB):
- Small enough to download quickly
- Supports tool calling (Qwen2.5-Instruct series)
- Runs on any hardware (even 1 GB RAM with small context)
- Available from HuggingFace

---

## What's NOT in This Task

- **Token streaming from llama-server** — future enhancement (word-chunking works for now)
- **Grammar-constrained decoding** — optimization, not MVP
- **Speculative decoding** — optimization, not MVP
- **Dual-model strategy** — requires loading two models, Phase 3
- **Semantic cache** — optimization, Phase 3
- **Model download** — model.download tool exists but downloads are stubbed; that's a separate task
- **GPU inference** — CPU-only for now (GGML_CUDA=OFF)

---

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| llama-server binary too large for squashfs | Current 65 MB squashfs has room; llama-server adds ~3-5 MB |
| Build time increase | llama.cpp builds in ~5 min on WSL2; acceptable |
| Model loading time | mmap is near-instant; health poll catches it |
| OOM with model + WebKit | Desktop mode already reserves 500 MB; may need to increase for model |
| Qwen2.5 tool-call format mismatch | Qwen2.5-Instruct uses standard OpenAI format; agent loop already handles it |

---

## Summary

This is a clean integration task. The architecture is ready:
- Agent callback pattern accepts any inference function
- OpenAI-compatible request/response format already used
- Model selection already implemented
- Config plumbing already flows model_path to child_main

What we build:
1. Buildroot package for llama-server (~50 lines of Makefile)
2. Process lifecycle management (~100 lines in child_main.cpp)
3. HTTP proxy inference function (~40 lines)
4. Global inference function swap (~10 lines)
5. Host tests (~80 lines)

Estimated total: ~280 lines of new C++ code + ~50 lines of Buildroot recipe.
