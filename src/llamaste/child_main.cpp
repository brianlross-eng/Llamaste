// child_main.cpp -- HTTP server with agent chat, dashboard, and OpenAI API routes
//
// This is the child process entry point, called by the supervisor after fork().
// It runs the HTTP server with all custom routes for the Llamaste LLM-OS:
//   - Static web UI files (served from embedded data or filesystem)
//   - Agent chat endpoint with SSE streaming
//   - System dashboard JSON endpoint
//   - Tool listing endpoint
//   - Conversation listing endpoint
//   - OpenAI-compatible /v1/chat/completions endpoint
//   - Health check endpoint
//
// In Phase 1, inference is handled by a stub that returns canned responses.
// This will be replaced with real llama-server inference integration later.

#include "supervisor.h"
#include "agent.h"
#include "prompt_builder.h"
#include "tools.h"
#include "hwdetect.h"
#include "net_mdns.h"
#include "scheduler.h"
#include "auth.h"
#include "voice.h"
#include "cluster.h"
#include "mcp_server.h"
#include "updater.h"
#include "version.h"
#include "wifi.h"
#include "tools_model_download.h"
#include "json.hpp"

// httplib must be included in exactly one translation unit with implementation.
// Explicitly ensure OpenSSL support is NOT enabled (we don't need it for Phase 1).
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "httplib.h"

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <string>
#include <map>
#include <mutex>
#include <fstream>
#include <sstream>
#include <chrono>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <termios.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};
static time_t g_start_time = 0;

// Boot mode (set during child_main initialization)
static std::string g_boot_mode = "server";

// In-memory conversation store (Phase 1)
static std::map<std::string, ConversationState> g_conversations;
static std::mutex g_conversations_mutex;

// Tool registry (initialized once at startup)
static ToolRegistry g_tools;

// Hardware info (detected once at startup)
static HardwareInfo g_hwinfo;

// System prompt (built once at startup)
static std::string g_system_prompt;

// Heartbeat scheduler
static Scheduler g_scheduler;

// Device authentication
static AuthManager g_auth;

// Mesh clustering
static ClusterManager g_cluster;

// WiFi manager
static WiFiManager g_wifi;

// Desktop compositor PID (set by compositor launch thread)
#ifndef _WIN32
static std::atomic<pid_t> g_cage_pid{0};
#endif

// ---------------------------------------------------------------------------
// Inference backend state
// ---------------------------------------------------------------------------

// Inference function pointer: starts as stub, swapped to llama_inference on model load
static std::function<std::string(const std::string&)> g_inference_fn;

// llama-server process state
#ifndef _WIN32
static std::atomic<pid_t> g_llama_pid{0};
#endif
static std::atomic<bool> g_model_loaded{false};
static int g_llama_port = 8088;
static std::string g_model_name;
static std::mutex g_llama_mutex;

// Persistent HTTP client for inference (HTTP keep-alive — avoids TCP reconnect per request).
// Protected by g_inference_cli_mutex. Created lazily, recreated if connection drops.
static std::unique_ptr<httplib::Client> g_inference_cli;
static std::mutex g_inference_cli_mutex;

// Semantic cache: hash(system_prompt + user_message) → {response, timestamp}
// Avoids redundant LLM inference for repeated identical queries (e.g. help, status).
struct SemanticCacheEntry {
    std::string response;
    std::time_t timestamp = 0;
};
static std::map<std::string, SemanticCacheEntry> g_semantic_cache;
static std::mutex g_semantic_cache_mutex;
static constexpr int SEMANTIC_CACHE_TTL_SECONDS = 600;  // 10 minutes
static constexpr int SEMANTIC_CACHE_MAX_ENTRIES  = 50;

// llama-rpc-server process state (mesh clustering)
#ifndef _WIN32
static std::atomic<pid_t> g_rpc_pid{0};
#endif
static int g_rpc_port = 50052;

// Cluster auto-upgrade: prevents concurrent background downloads
static std::atomic<bool> g_upgrade_downloading{false};

// ---------------------------------------------------------------------------
// Inference configuration helpers (non-static for testability)
// ---------------------------------------------------------------------------

// Thread count for generation.
// Use all available CPU cores for maximum inference throughput.
// The watchdog is now kept alive by a dedicated kicker thread in the supervisor,
// so slow inference with multiple threads won't trigger the softdog.
int compute_thread_count(int cpu_cores) {
    return cpu_cores < 1 ? 1 : cpu_cores;
}

// Batch thread count: use all cores
int compute_batch_thread_count(int cpu_cores) {
    return cpu_cores < 1 ? 1 : cpu_cores;
}

// Context window size based on free RAM after model loading.
// 4096 is used for diagnostic purposes: halves KV cache memory vs 8192.
// The 62-tool system prompt is ~700 tokens when in /completion chatml format
// (tools are NOT passed to llama-server, only the system prompt + messages),
// so 4096 is sufficient for the actual inference workload.
int compute_context_size(int free_ram_mb) {
    if (free_ram_mb >= 2048) return 4096;
    if (free_ram_mb >= 1024) return 2048;
    if (free_ram_mb >= 512)  return 2048;
    return 2048;
}

// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------

static void child_signal(int) {
    g_running = false;
}

// ---------------------------------------------------------------------------
// File serving helpers
// ---------------------------------------------------------------------------

// Try to read a file from the filesystem (development mode)
static bool read_file_to_string(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Determine content type from file extension
static const char* content_type_for(const std::string& filename) {
    if (filename.find(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (filename.find(".css") != std::string::npos) return "text/css; charset=utf-8";
    if (filename.find(".js") != std::string::npos) return "application/javascript; charset=utf-8";
    if (filename.find(".json") != std::string::npos) return "application/json; charset=utf-8";
    return "application/octet-stream";
}

// Serve a static web file. In production (LLAMASTE_HAS_EMBED), serve from
// embedded byte arrays. In development, read from the web/ directory.
static void serve_static_file(
    const httplib::Request& /*req*/,
    httplib::Response& res,
    const std::string& filename,
    [[maybe_unused]] const unsigned char* embed_data,
    [[maybe_unused]] unsigned int embed_len
) {
#ifdef LLAMASTE_HAS_EMBED
    if (embed_data && embed_len > 0) {
        res.set_content(
            std::string(reinterpret_cast<const char*>(embed_data), embed_len),
            content_type_for(filename)
        );
        return;
    }
#endif
    // Development mode: read from filesystem
    // Try several paths relative to where the binary might be running
    std::vector<std::string> search_paths = {
        "src/llamaste/web/" + filename,
        "../src/llamaste/web/" + filename,
        "web/" + filename,
    };

    for (const auto& path : search_paths) {
        std::string content;
        if (read_file_to_string(path, content)) {
            res.set_content(content, content_type_for(filename));
            return;
        }
    }

    res.status = 404;
    res.set_content("File not found: " + filename, "text/plain");
}

// ---------------------------------------------------------------------------
// Stub inference function (Phase 1 -- no real model)
// ---------------------------------------------------------------------------

// Returns a canned response that looks like a real chat completion.
// The response varies based on the user message to make the UI feel interactive.
static std::string stub_inference(const std::string& request_json) {
    // Parse the request to extract the last user message for context
    auto req = json::parse(request_json, nullptr, false);
    std::string user_msg;
    if (!req.is_discarded() && req.contains("messages") && req["messages"].is_array()) {
        for (auto it = req["messages"].rbegin(); it != req["messages"].rend(); ++it) {
            if ((*it).value("role", "") == "user") {
                user_msg = (*it).value("content", "");
                break;
            }
        }
    }

    // Generate contextual stub response
    std::string response_text;
    if (user_msg.empty()) {
        response_text = "I'm Llamaste, your LLM operating system. How can I help?";
    } else if (user_msg.find("hello") != std::string::npos ||
               user_msg.find("Hello") != std::string::npos ||
               user_msg.find("hi") != std::string::npos) {
        response_text = "Hello! I'm Llamaste, your AI operating system. "
                        "I can help you manage files, check system status, configure settings, "
                        "and more. What would you like to do?";
    } else if (user_msg.find("file") != std::string::npos ||
               user_msg.find("ls") != std::string::npos ||
               user_msg.find("dir") != std::string::npos) {
        response_text = "I can help you with file operations! I have tools like "
                        "`fs.list_directory`, `fs.read_file`, and `fs.write_file`. "
                        "What path would you like me to look at? "
                        "(Note: in stub mode, tool calls are simulated.)";
    } else if (user_msg.find("system") != std::string::npos ||
               user_msg.find("status") != std::string::npos ||
               user_msg.find("info") != std::string::npos) {
        response_text = "I can check the system status for you. The dashboard in the "
                        "sidebar shows real-time CPU, memory, disk, and temperature data. "
                        "You can also ask me to run specific system tools.";
    } else {
        response_text = "I understand your request. In the current Phase 1 build, "
                        "I'm running in stub mode without a real language model. "
                        "Once a model is loaded, I'll be able to give you intelligent responses "
                        "and use my 25 system tools to interact with the OS. "
                        "For now, the web UI, dashboard, and API endpoints are all functional.";
    }

    // Build an OpenAI-compatible chat completion response
    json response;
    json choice;
    json message;
    message["role"] = "assistant";
    message["content"] = response_text;
    choice["index"] = 0;
    choice["message"] = message;
    choice["finish_reason"] = "stop";
    response["id"] = "chatcmpl-stub-001";
    response["object"] = "chat.completion";
    response["model"] = "llamaste-stub";
    response["choices"] = json::array({choice});
    json usage;
    usage["prompt_tokens"] = 0;
    usage["completion_tokens"] = 0;
    usage["total_tokens"] = 0;
    response["usage"] = usage;

    return response.dump();
}

// ---------------------------------------------------------------------------
// Semantic cache helpers
// ---------------------------------------------------------------------------

// Compute a simple 64-bit FNV-1a hash of a string (fast, no crypto needed).
static uint64_t fnv1a_hash(const std::string& s) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : s) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Check the semantic cache. Returns cached response or "" if miss/expired.
// Only caches single-turn user messages (avoids multi-turn pollution).
static std::string semantic_cache_lookup(const std::string& request_json) {
    // Only cache single-turn requests: one system + one user message.
    // More complex requests (tool results, multi-turn) are always fresh.
    auto req = json::parse(request_json, nullptr, false);
    if (req.is_discarded()) return "";
    if (!req.contains("messages") || !req["messages"].is_array()) return "";

    const auto& msgs = req["messages"];
    // Count non-system messages
    int non_sys = 0;
    std::string user_text;
    std::string sys_text;
    for (const auto& m : msgs) {
        const std::string role = m.value("role", "");
        if (role == "system") {
            sys_text = m.value("content", "");
        } else if (role == "user") {
            user_text = m.value("content", "");
            non_sys++;
        } else {
            non_sys++;  // tool result or assistant turn → don't cache
        }
    }
    if (non_sys != 1 || user_text.empty()) return "";  // not a simple query

    const std::string key_src = sys_text + "\x00" + user_text;
    const uint64_t h = fnv1a_hash(key_src);
    const std::string key = std::to_string(h);

    std::lock_guard<std::mutex> lock(g_semantic_cache_mutex);
    auto it = g_semantic_cache.find(key);
    if (it == g_semantic_cache.end()) return "";

    const std::time_t now = std::time(nullptr);
    if (now - it->second.timestamp > SEMANTIC_CACHE_TTL_SECONDS) {
        g_semantic_cache.erase(it);
        return "";
    }

    fprintf(stderr, "[inference] semantic cache HIT (key=%s)\n", key.c_str());
    return it->second.response;
}

// Store response in semantic cache (evicts oldest if over cap).
static void semantic_cache_store(const std::string& request_json,
                                  const std::string& response_json) {
    auto req = json::parse(request_json, nullptr, false);
    if (req.is_discarded()) return;
    if (!req.contains("messages") || !req["messages"].is_array()) return;

    const auto& msgs = req["messages"];
    int non_sys = 0;
    std::string user_text;
    std::string sys_text;
    for (const auto& m : msgs) {
        const std::string role = m.value("role", "");
        if (role == "system") {
            sys_text = m.value("content", "");
        } else if (role == "user") {
            user_text = m.value("content", "");
            non_sys++;
        } else {
            non_sys++;
        }
    }
    if (non_sys != 1 || user_text.empty()) return;

    const std::string key_src = sys_text + "\x00" + user_text;
    const uint64_t h = fnv1a_hash(key_src);
    const std::string key = std::to_string(h);

    std::lock_guard<std::mutex> lock(g_semantic_cache_mutex);
    // Evict oldest if at capacity
    if (static_cast<int>(g_semantic_cache.size()) >= SEMANTIC_CACHE_MAX_ENTRIES) {
        std::time_t oldest_t = std::numeric_limits<std::time_t>::max();
        std::string oldest_k;
        for (const auto& kv : g_semantic_cache) {
            if (kv.second.timestamp < oldest_t) {
                oldest_t = kv.second.timestamp;
                oldest_k = kv.first;
            }
        }
        if (!oldest_k.empty()) g_semantic_cache.erase(oldest_k);
    }
    g_semantic_cache[key] = {response_json, std::time(nullptr)};
}

// ---------------------------------------------------------------------------
// Real inference: HTTP proxy to llama-server on localhost
// HTTP keep-alive: reuse persistent Client across calls.
// ---------------------------------------------------------------------------

// Get or create the persistent inference httplib::Client.
// Call with g_inference_cli_mutex held.
static httplib::Client& get_inference_client() {
    if (!g_inference_cli) {
        g_inference_cli = std::make_unique<httplib::Client>("127.0.0.1", g_llama_port);
        g_inference_cli->set_connection_timeout(5);
        // 300s: large prompts (62 tools + system prompt ≈ 6300 tokens) can take
        // 130-200s on a 2-core CPU VM.  Previous 120s was too short.
        g_inference_cli->set_read_timeout(300);
        g_inference_cli->set_keep_alive(true);
    }
    return *g_inference_cli;
}

// Build an error response JSON string (well-formed chat completion).
static std::string make_inference_error(const std::string& message) {
    json response;
    json choice;
    json msg;
    msg["role"] = "assistant";
    msg["content"] = message;
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

// Build a chatml-formatted prompt string from OpenAI-format messages.
// Used by llama_inference to bypass the broken chat template in llama-server.
static std::string build_chatml_prompt(const json& messages) {
    std::string prompt;
    for (const auto& msg : messages) {
        const std::string role = msg.value("role", "user");
        std::string content = msg.value("content", "");

        // Assistant messages with tool_calls: serialize tool calls as Qwen2.5 native format
        if (role == "assistant" && content.empty() && msg.contains("tool_calls")
            && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                if (tc.contains("function")) {
                    std::string name = tc["function"].value("name", "");
                    std::string args = tc["function"].value("arguments", "{}");
                    content += "<tool_call>\n{\"name\": \"" + name +
                               "\", \"arguments\": " + args + "}\n</tool_call>\n";
                }
            }
        }

        // Tool result messages → "tool" role (Qwen2.5 training format)
        if (role == "tool") {
            std::string name = msg.value("name", "");
            prompt += "<|im_start|>tool\n";
            if (!name.empty()) {
                prompt += "{\"name\": \"" + name + "\", \"content\": " +
                          json(content).dump() + "}\n";
            } else {
                prompt += content + "\n";
            }
            prompt += "<|im_end|>\n";
            continue;
        }

        prompt += "<|im_start|>" + role + "\n" + content + "<|im_end|>\n";
    }
    prompt += "<|im_start|>assistant\n";
    return prompt;
}

static std::string llama_inference(const std::string& request_json) {
    fprintf(stderr, "[inference] START request_size=%zu\n", request_json.size());
    if (!g_model_loaded.load()) {
        return stub_inference(request_json);  // graceful fallback
    }

    // --- Semantic cache lookup (single-turn queries only) ---
    std::string cached = semantic_cache_lookup(request_json);
    if (!cached.empty()) return cached;

    // --- Build chatml prompt and use /completion endpoint ---
    // llama-server's /v1/chat/completions is broken in this build:
    //   - --jinja silently ignored (compiled without minja Jinja2 support)
    //   - "Content-only" mode: no stop tokens → generation stalls indefinitely
    //   - Hardware watchdog fires (~60s) and reboots the system
    // Fix: format messages as chatml manually, POST to /completion (raw generation),
    // parse the plain-text response, and wrap it in OpenAI format.
    auto req_obj = json::parse(request_json, nullptr, false);
    if (req_obj.is_discarded()) {
        return make_inference_error("[inference error: failed to parse request]");
    }

    int   max_tokens  = req_obj.value("max_tokens", 2048);
    float temperature = req_obj.value("temperature", 0.7f);

    std::string prompt;
    if (req_obj.contains("messages") && req_obj["messages"].is_array()) {
        prompt = build_chatml_prompt(req_obj["messages"]);
    } else {
        prompt = "<|im_start|>user\nhello<|im_end|>\n<|im_start|>assistant\n";
    }

    // Build /completion request body
    json comp_req;
    comp_req["prompt"]       = prompt;
    comp_req["n_predict"]    = max_tokens;
    comp_req["temperature"]  = temperature;
    comp_req["stop"]   = json::array({"<|im_end|>", "<|endoftext|>", "<|im_start|>"});
    comp_req["stream"] = false;
    std::string comp_req_str = comp_req.dump();

    fprintf(stderr, "[inference] chatml prompt_len=%zu n_predict=%d POSTing to /completion\n",
            prompt.size(), max_tokens);

    // --- POST to /completion with a fresh connection (no keep-alive) ---
    // Previously used keep-alive but the persistent connection might be causing
    // the Post() to block waiting for data after llama-server finishes generating.
    // Use a fresh connection per request to eliminate connection state as a variable.
    fprintf(stderr, "[inference] creating fresh client for /completion\n");
    httplib::Result result;
    {
        httplib::Client fresh_cli("127.0.0.1", g_llama_port);
        fresh_cli.set_connection_timeout(5);
        fresh_cli.set_read_timeout(300);
        fresh_cli.set_keep_alive(false);  // fresh connection, no keep-alive
        fprintf(stderr, "[inference] posting to /completion (fresh connection, stream=false)\n");
        result = fresh_cli.Post("/completion", comp_req_str, "application/json");
        fprintf(stderr, "[inference] Post returned result=%s status=%d\n",
                result ? "ok" : "null", result ? result->status : 0);
    }

    if (!result || result->status != 200) {
        std::string msg;
        if (!result) {
            fprintf(stderr, "[inference] llama-server unreachable (no result)\n");
            msg = "[inference error: llama-server unreachable]";
        } else {
            fprintf(stderr, "[inference] llama-server HTTP %d: %.500s\n",
                    result->status, result->body.c_str());
            msg = "[inference error: llama-server returned HTTP " +
                  std::to_string(result->status) + "]";
        }
        return make_inference_error(msg);
    }

    // --- Convert /completion response → OpenAI chat.completion format ---
    fprintf(stderr, "[inference] /completion response body_size=%zu body_preview=%.200s\n",
            result->body.size(), result->body.c_str());
    auto comp_resp = json::parse(result->body, nullptr, false);
    std::string content;
    if (!comp_resp.is_discarded() && comp_resp.contains("content")) {
        content = comp_resp["content"].get<std::string>();
    }

    json response;
    response["id"]      = "chatcmpl-" + std::to_string(std::time(nullptr));
    response["object"]  = "chat.completion";
    response["model"]   = "llamaste";
    json msg_obj;
    msg_obj["role"]    = "assistant";
    msg_obj["content"] = content;
    json choice;
    choice["index"]        = 0;
    choice["message"]      = msg_obj;
    choice["finish_reason"] = "stop";
    response["choices"] = json::array({choice});
    json usage;
    usage["prompt_tokens"]     = comp_resp.is_discarded() ? 0 : comp_resp.value("tokens_evaluated", 0);
    usage["completion_tokens"] = comp_resp.is_discarded() ? 0 : comp_resp.value("tokens_predicted", 0);
    usage["total_tokens"]      = usage["prompt_tokens"].get<int>() + usage["completion_tokens"].get<int>();
    response["usage"] = usage;

    fprintf(stderr, "[inference] generated %d tokens: %.100s\n",
            usage["completion_tokens"].get<int>(), content.c_str());

    std::string response_str = response.dump();

    // --- Semantic cache store ---
    semantic_cache_store(request_json, response_str);

    return response_str;
}

#ifndef _WIN32

// ---------------------------------------------------------------------------
// llama-server process lifecycle
// ---------------------------------------------------------------------------

static bool spawn_llama_server(const std::string& model_path, int cpu_cores,
                                int free_ram_mb, const std::string& rpc_endpoints = "",
                                const std::string& tensor_split = "") {
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
        // Redirect llama-server stdout/stderr to a log file.
        // Without this, llama-server's verbose logs (when --log-disable is off) flood
        // the serial port buffer (/dev/console), causing all stderr writes to block —
        // including the supervisor's watchdog kick loop, which then misses kicks and
        // triggers a softdog reboot. With this redirect, logs go to /tmp/llama-server.log
        // and can be inspected via fs_read_file without affecting the serial port.
        int log_fd = open("/tmp/llama-server.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }

        // Child process: build args vector for variable-length command
        std::vector<const char*> args;
        args.push_back("llama-server");
        args.push_back("-m"); args.push_back(model_path.c_str());
        args.push_back("--host"); args.push_back("127.0.0.1");
        args.push_back("--port"); args.push_back(port_str.c_str());
        args.push_back("--no-webui");
        args.push_back("-c"); args.push_back(c_str.c_str());
        args.push_back("-t"); args.push_back(t_str.c_str());
        args.push_back("-tb"); args.push_back(tb_str.c_str());
        // Note: --mlock, -fa (Flash Attention), and --cache-reuse removed.
        // -fa caused the decode step to hang indefinitely after prefill.
        // Note: llama_inference() uses /completion endpoint with manual chatml.
        // --log-disable REMOVED for debugging (logs go to /tmp/llama-server.log)
        // args.push_back("--log-disable");

        if (!rpc_endpoints.empty()) {
            args.push_back("--rpc");
            args.push_back(rpc_endpoints.c_str());
            fprintf(stderr, "[child] Using RPC endpoints: %s\n", rpc_endpoints.c_str());
        }

        if (!tensor_split.empty()) {
            args.push_back("--tensor-split");
            args.push_back(tensor_split.c_str());
            fprintf(stderr, "[child] Using tensor split: %s\n", tensor_split.c_str());
        }

        args.push_back(nullptr);
        execv("/opt/llamaste/llama-server", const_cast<char**>(args.data()));
        fprintf(stderr, "[child] execv llama-server failed: %s\n", strerror(errno));
        _exit(127);
    }

    // Parent: store PID
    g_llama_pid.store(pid);
    fprintf(stderr, "[child] llama-server spawned with PID %d\n", pid);
    return true;
}

static bool spawn_rpc_server() {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[child] Failed to fork llama-rpc-server: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        std::string port_str = std::to_string(g_rpc_port);
        execl("/opt/llamaste/llama-rpc-server", "llama-rpc-server",
              "--host", "0.0.0.0",
              "--port", port_str.c_str(),
              "-c",  // Enable tensor caching for faster model reloads
              (char*)nullptr);
        fprintf(stderr, "[child] execl llama-rpc-server failed: %s\n", strerror(errno));
        _exit(127);
    }

    g_rpc_pid.store(pid);
    fprintf(stderr, "[child] llama-rpc-server spawned with PID %d on port %d\n",
            pid, g_rpc_port);
    return true;
}

// Spawn wpa_supplicant for WiFi management on the given interface.
// Config is created at /data/llamaste/wifi/wpa.conf if absent.
#ifndef _WIN32
static void spawn_wpa_supplicant(const std::string& iface) {
    // Ensure config directory and skeleton config exist
    mkdir("/data/llamaste", 0755);
    mkdir("/data/llamaste/wifi", 0755);

    const char* conf = "/data/llamaste/wifi/wpa.conf";
    struct stat st;
    if (stat(conf, &st) != 0) {
        // Write minimal wpa_supplicant.conf skeleton
        FILE* f = fopen(conf, "w");
        if (f) {
            fprintf(f, "ctrl_interface=/run/wpa_supplicant\n");
            fprintf(f, "ctrl_interface_group=0\n");
            fprintf(f, "update_config=1\n");
            fclose(f);
        }
    }

    // Ensure control directory exists
    mkdir("/run/wpa_supplicant", 0755);

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[child] Failed to fork wpa_supplicant: %s\n",
                strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/usr/sbin/wpa_supplicant", "wpa_supplicant",
              "-B",           // background (daemonize)
              "-i", iface.c_str(),
              "-c", conf,
              "-D", "nl80211,wext",
              (char*)nullptr);
        fprintf(stderr, "[child] execl wpa_supplicant failed: %s\n",
                strerror(errno));
        _exit(127);
    }
    // wpa_supplicant daemonizes, so the child exits quickly — no need to track pid
    int wstatus;
    waitpid(pid, &wstatus, 0);
    // The real daemon is now running in the background with its own PID file
    fprintf(stderr, "[child] wpa_supplicant spawned on %s\n", iface.c_str());
}

static void spawn_dhcpcd(const std::string& iface) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[child] Failed to fork dhcpcd: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        // Buildroot installs dhcpcd to /sbin/dhcpcd (not /usr/sbin/)
        execl("/sbin/dhcpcd", "dhcpcd",
              "-b",            // background — retries until it gets a lease
              iface.c_str(),
              (char*)nullptr);
        fprintf(stderr, "[child] execl dhcpcd failed: %s\n", strerror(errno));
        _exit(127);
    }
    int wstatus;
    waitpid(pid, &wstatus, 0);
    if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 127) {
        fprintf(stderr, "[child] dhcpcd failed to exec — binary missing?\n");
    } else {
        fprintf(stderr, "[child] dhcpcd spawned on %s\n", iface.c_str());
    }
}
#endif // _WIN32

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

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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

        if (WIFEXITED(status)) {
            fprintf(stderr, "[child] llama-server exited with code %d\n",
                    WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            fprintf(stderr, "[child] llama-server killed by signal %d\n",
                    WTERMSIG(status));
        }

        g_model_loaded.store(false);
        g_inference_fn = stub_inference;
        // Reset persistent inference client — old connection is dead after crash
        { std::lock_guard<std::mutex> lk(g_inference_cli_mutex); g_inference_cli.reset(); }
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

// ---------------------------------------------------------------------------
// System info gathering (for /llamaste/system endpoint)
// ---------------------------------------------------------------------------

static json gather_system_info(const SupervisorConfig& config) {
    json info;

    // Version and A/B slot
    info["version"] = LLAMASTE_VERSION;
    info["active_slot"] = detect_current_slot();

    // Model info
    if (config.model_path.empty()) {
        info["model"] = "stub (no model loaded)";
    } else {
        // Extract just the filename from path
        auto pos = config.model_path.rfind('/');
        if (pos == std::string::npos) pos = config.model_path.rfind('\\');
        if (pos != std::string::npos) {
            info["model"] = config.model_path.substr(pos + 1);
        } else {
            info["model"] = config.model_path;
        }
    }

    // Boot mode
    info["mode"] = g_boot_mode;

    // Uptime
    time_t now = time(nullptr);
    info["uptime"] = static_cast<int>(now - g_start_time);
    info["uptime_seconds"] = static_cast<int>(now - g_start_time);

    // IP address - use MdnsResponder's cross-platform IP detection
    info["ip"] = MdnsResponder::get_local_ip();

    // CPU percent - read from /proc/stat
    double cpu_pct = 0.0;
#ifndef _WIN32
    {
        // Read /proc/stat twice with a small delay to calculate CPU usage
        auto read_cpu_stat = []() -> std::vector<long long> {
            std::ifstream f("/proc/stat");
            std::string line;
            if (std::getline(f, line) && line.substr(0, 3) == "cpu") {
                std::istringstream ss(line.substr(5));
                std::vector<long long> vals;
                long long v;
                while (ss >> v) vals.push_back(v);
                return vals;
            }
            return {};
        };

        auto stat1 = read_cpu_stat();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto stat2 = read_cpu_stat();

        if (stat1.size() >= 4 && stat2.size() >= 4) {
            long long idle1 = stat1[3], idle2 = stat2[3];
            long long total1 = 0, total2 = 0;
            for (auto v : stat1) total1 += v;
            for (auto v : stat2) total2 += v;
            long long total_diff = total2 - total1;
            long long idle_diff = idle2 - idle1;
            if (total_diff > 0) {
                cpu_pct = 100.0 * (1.0 - static_cast<double>(idle_diff) / total_diff);
            }
        }
    }
#endif
    info["cpu_percent"] = cpu_pct;

    // Temperature - read from thermal zone
    double temp_c = 0.0;
#ifndef _WIN32
    {
        std::ifstream f("/sys/class/thermal/thermal_zone0/temp");
        int millideg = 0;
        if (f >> millideg) {
            temp_c = millideg / 1000.0;
        }
    }
#endif
    info["temperature"] = temp_c;
    info["temperature_c"] = temp_c;

    // RAM - use hwdetect helpers
    int total_kb = read_meminfo_kb("MemTotal");
    int avail_kb = read_meminfo_kb("MemAvailable");
    int total_mb = total_kb / 1024;
    int used_mb = (total_kb - avail_kb) / 1024;
    if (total_mb == 0) {
        // Fallback from config
        total_mb = config.ram_total_mb;
        used_mb = 0;
    }
    info["ram_total_mb"] = total_mb;
    info["ram_used_mb"] = used_mb;

    // Disk - check /data partition (or fallback to /)
    double disk_total_gb = 0.0;
    double disk_used_gb = 0.0;
#ifndef _WIN32
    {
        struct statvfs st;
        const char* mount = "/data";
        if (statvfs(mount, &st) != 0) {
            mount = "/";
            statvfs(mount, &st);
        }
        if (st.f_blocks > 0) {
            double total = static_cast<double>(st.f_blocks) * st.f_frsize;
            double avail = static_cast<double>(st.f_bavail) * st.f_frsize;
            disk_total_gb = total / (1024.0 * 1024.0 * 1024.0);
            disk_used_gb = (total - avail) / (1024.0 * 1024.0 * 1024.0);
        }
    }
#endif
    info["disk_total_gb"] = disk_total_gb;
    info["disk_used_gb"] = disk_used_gb;

    // Hardware details (from startup detection)
    info["cpu_model"] = g_hwinfo.cpu_model;
    info["cpu_cores"] = g_hwinfo.cpu_cores;
    info["gpu_name"] = g_hwinfo.gpu_detected ? g_hwinfo.gpu_name : "";
    info["gpu_detected"] = g_hwinfo.gpu_detected;
    info["has_avx2"] = g_hwinfo.has_avx2;
    info["has_avx512"] = g_hwinfo.has_avx512;

    // Phase 2 fields
    info["tokens_per_sec"] = 0.0;  // will be real when inference is wired
    info["scheduled_tasks_count"] = g_scheduler.active_task_count();
    auto next_sched = g_scheduler.next_scheduled_time();
    info["next_scheduled_time"] = next_sched > 0 ? static_cast<long>(next_sched) : 0;

    // Cluster info
    json cluster_info;
    cluster_info["role"] = g_cluster.role_name();
    cluster_info["peer_count"] = g_cluster.peer_count();
    if (g_cluster.peer_count() > 0) {
        cluster_info["coordinator"] = g_cluster.coordinator().hostname;
        cluster_info["rpc_endpoints"] = g_cluster.rpc_endpoint_list();
        uint64_t total_ram = g_cluster.self_info().ram_mb;
        for (const auto& p : g_cluster.peers()) total_ram += p.ram_mb;
        cluster_info["total_ram_mb"] = total_ram;
    }
    info["cluster"] = cluster_info;

    return info;
}

// ---------------------------------------------------------------------------
// Generate a simple unique ID
// ---------------------------------------------------------------------------

static std::string generate_id() {
    static std::atomic<int> counter{0};
    int c = counter.fetch_add(1);
    char buf[64];
    snprintf(buf, sizeof(buf), "conv_%ld_%d",
             static_cast<long>(time(nullptr)), c);
    return buf;
}

// ---------------------------------------------------------------------------
// Get or create a conversation
// ---------------------------------------------------------------------------

static ConversationState& get_or_create_conversation(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_conversations_mutex);
    auto it = g_conversations.find(id);
    if (it != g_conversations.end()) {
        return it->second;
    }
    // Create new conversation
    auto& conv = g_conversations[id];
    conv.conversation_id = id;
    conv.system_prompt = g_system_prompt;
    return conv;
}

// ---------------------------------------------------------------------------
// SSE streaming helper
// ---------------------------------------------------------------------------

// Emit a single SSE event
static void sse_write(httplib::DataSink& sink, const std::string& event_type, const json& data) {
    json payload = data;
    payload["type"] = event_type;
    std::string line = "data: " + payload.dump() + "\n\n";
    sink.write(line.c_str(), line.size());
}

// ---------------------------------------------------------------------------
// Route handlers
// ---------------------------------------------------------------------------

// POST /llamaste/chat — Agent chat with optional SSE streaming
static void handle_chat(const httplib::Request& req, httplib::Response& res) {
    // Parse request body
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
        res.status = 400;
        json err;
        err["error"] = "Invalid JSON body";
        res.set_content(err.dump(), "application/json");
        return;
    }

    std::string message = body.value("message", "");
    std::string conv_id = body.value("conversation_id", "");
    bool stream = body.value("stream", true);

    if (message.empty()) {
        res.status = 400;
        json err;
        err["error"] = "Missing 'message' field";
        res.set_content(err.dump(), "application/json");
        return;
    }

    if (conv_id.empty()) {
        conv_id = generate_id();
    }

    if (stream) {
        // SSE streaming response
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("X-Accel-Buffering", "no");

        res.set_chunked_content_provider(
            "text/event-stream",
            [message, conv_id](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                // Get or create conversation
                auto& conv = get_or_create_conversation(conv_id);
                conv.add_user_message(message);

                // Run agent turn with current inference function
                std::string final_text = agent_turn(conv, g_tools, g_inference_fn);

                // Simulate streaming: emit tokens one word at a time
                std::istringstream words(final_text);
                std::string word;
                bool first = true;
                while (words >> word) {
                    json token_data;
                    if (!first) {
                        token_data["content"] = " " + word;
                    } else {
                        token_data["content"] = word;
                        first = false;
                    }
                    sse_write(sink, "token", token_data);

                    // Small delay to simulate streaming
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }

                // Send done event
                json done_data;
                done_data["content"] = final_text;
                done_data["conversation_id"] = conv_id;
                sse_write(sink, "done", done_data);

                // Send SSE terminator
                std::string done_line = "data: [DONE]\n\n";
                sink.write(done_line.c_str(), done_line.size());

                sink.done();
                return true;
            }
        );
    } else {
        // Non-streaming response
        auto& conv = get_or_create_conversation(conv_id);
        conv.add_user_message(message);

        std::string result = agent_turn(conv, g_tools, g_inference_fn);

        json response;
        response["conversation_id"] = conv_id;
        response["content"] = result;
        response["message_count"] = conv.message_count();
        res.set_content(response.dump(), "application/json");
    }
}

// GET /llamaste/system — System dashboard JSON
static void handle_system(
    const httplib::Request& /*req*/,
    httplib::Response& res,
    const SupervisorConfig& config
) {
    json info = gather_system_info(config);
    res.set_content(info.dump(), "application/json");
}

// GET /llamaste/tools — List available tools
static void handle_tools(const httplib::Request& /*req*/, httplib::Response& res) {
    std::string tools_json = g_tools.to_openai_tools_json();
    res.set_content(tools_json, "application/json");
}

// GET /llamaste/conversations — List saved conversations
static void handle_conversations(const httplib::Request& /*req*/, httplib::Response& res) {
    json result;
    json conversations_array = json::array();

    {
        std::lock_guard<std::mutex> lock(g_conversations_mutex);
        for (const auto& [id, conv] : g_conversations) {
            json entry;
            entry["id"] = id;
            // Use first user message as title, or fallback
            const auto& msgs = conv.messages();
            std::string title;
            for (const auto& msg : msgs) {
                if (msg.role == "user" && !msg.content.empty()) {
                    title = msg.content;
                    if (title.size() > 60) {
                        title = title.substr(0, 57) + "...";
                    }
                    break;
                }
            }
            entry["title"] = title.empty() ? ("Conversation " + id.substr(0, 8)) : title;
            entry["message_count"] = conv.message_count();
            conversations_array.push_back(entry);
        }
    }

    // Also scan /data/llamaste/conversations/ for persisted conversations
    // (Phase 1: in-memory only, but we include the directory scan structure)
#ifndef _WIN32
    {
        std::string conv_dir = "/data/llamaste/conversations";
        // In Phase 1, we only return in-memory conversations
        // Disk persistence will be added later
    }
#endif

    result["conversations"] = conversations_array;
    res.set_content(result.dump(), "application/json");
}

// POST /v1/chat/completions — OpenAI-compatible API
static void handle_openai_completions(const httplib::Request& req, httplib::Response& res) {
    fprintf(stderr, "[completions] ENTER body_size=%zu\n", req.body.size());
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
        res.status = 400;
        json err;
        err["error"] = json::object();
        err["error"]["message"] = "Invalid JSON body";
        err["error"]["type"] = "invalid_request_error";
        res.set_content(err.dump(), "application/json");
        return;
    }

    // For Phase 1 stub: extract last user message and return canned response
    std::string user_msg;
    if (body.contains("messages") && body["messages"].is_array()) {
        for (auto it = body["messages"].rbegin(); it != body["messages"].rend(); ++it) {
            if ((*it).value("role", "") == "user") {
                user_msg = (*it).value("content", "");
                break;
            }
        }
    }

    // Extract generation params from request body (pass caller's values through)
    int   max_tokens  = body.value("max_tokens",  2048);
    float temperature = body.value("temperature", 0.7f);

    // Create a minimal conversation for the stub
    ConversationState conv;
    conv.system_prompt = g_system_prompt;
    if (!user_msg.empty()) {
        conv.add_user_message(user_msg);
    }

    // Use current inference function to get the response
    fprintf(stderr, "[completions] calling build_inference_request max_tokens=%d\n", max_tokens);
    std::string request = build_inference_request(conv, g_tools, max_tokens, temperature);
    fprintf(stderr, "[completions] request_size=%zu calling inference\n", request.size());
    std::string response = g_inference_fn(request);
    fprintf(stderr, "[completions] inference returned %zu bytes\n", response.size());

    res.set_content(response, "application/json");
}

// GET /health — Health check
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

// ---------------------------------------------------------------------------
// do_auto_upgrade_check — start background model download if a better model
// fits available RAM but isn't on disk yet. Safe to call from any thread;
// the g_upgrade_downloading atomic prevents concurrent downloads.
// ---------------------------------------------------------------------------
static void do_auto_upgrade_check(const ClusterCapacity& cap) {
    if (!cap.upgrade_available) return;
    const ModelInfo* mi = recommend_model((int)cap.usable_ram_mb);
    if (!mi) return;
    std::string dest = "/data/models/" + std::string(mi->filename);
    if (access(dest.c_str(), R_OK) == 0) return;          // already on disk
    if (g_upgrade_downloading.exchange(true)) return;      // already downloading

    std::string repo_id = mi->repo_id;
    std::string gguf    = mi->filename;
    std::string model   = mi->name;
    std::string url     = build_hf_download_url(repo_id, gguf);
    fprintf(stderr, "[cluster] Auto-upgrade: downloading %s\n", gguf.c_str());

    Notification notif;
    notif.id    = "cluster_upgrade_start";
    notif.type  = "cluster";
    notif.title = "Cluster model upgrade";
    notif.body  = "Downloading " + model + " for your expanded cluster…";
    notif.time  = time(nullptr);
    g_scheduler.push_notification(std::move(notif));

    std::thread([dest, gguf, model, url]() {
#ifdef HAVE_LIBCURL
        std::string part = dest + ".part";
        uint64_t existing = 0;
        {
            struct stat st;
            if (stat(part.c_str(), &st) == 0) existing = st.st_size;
        }
        FILE* fp = fopen(part.c_str(), existing > 0 ? "ab" : "wb");
        if (!fp) {
            fprintf(stderr, "[cluster] Cannot write %s\n", part.c_str());
            g_upgrade_downloading.store(false);
            return;
        }
        CURL* curl = curl_easy_init();
        if (!curl) {
            fclose(fp);
            g_upgrade_downloading.store(false);
            return;
        }
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Llamaste/0.1");
        if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
            curl_easy_setopt(curl, CURLOPT_CAINFO,
                             "/etc/ssl/certs/ca-certificates.crt");
        if (existing > 0)
            curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                             (curl_off_t)existing);
        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);
        fclose(fp);

        if (res == CURLE_OK && http_code < 400) {
            rename(part.c_str(), dest.c_str());
            fprintf(stderr, "[cluster] Download complete: %s\n", gguf.c_str());
            Notification done;
            done.id    = "cluster_upgrade_done";
            done.type  = "cluster";
            done.title = "Model upgrade ready";
            done.body  = model + " downloaded — reload to activate.";
            done.time  = time(nullptr);
            g_scheduler.push_notification(std::move(done));
            // Fire topology callback directly — run_election() won't re-fire
            // it when state is already STANDALONE (no state change detected).
            g_cluster.fire_topology_callback();
        } else {
            fprintf(stderr, "[cluster] Download failed for %s (curl=%d http=%ld)\n",
                    gguf.c_str(), (int)res, http_code);
        }
#else
        fprintf(stderr, "[cluster] Auto-upgrade requires libcurl (not built)\n");
#endif
        g_upgrade_downloading.store(false);
    }).detach();
}

// ---------------------------------------------------------------------------
// child_main — HTTP server entry point (called from supervisor fork)
// ---------------------------------------------------------------------------

int child_main(const SupervisorConfig& config) {
    signal(SIGTERM, child_signal);
    signal(SIGINT, child_signal);
    signal(SIGPIPE, SIG_IGN);  // ignore broken pipe — client disconnect mid-response must not crash child

    // Tee stderr to /tmp/child.log so the supervisor's 'D' debug view can show it.
    // Keep writing to /dev/console too (for serial debugging).
#ifndef _WIN32
    {
        int log_fd = open("/tmp/child.log",
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (log_fd >= 0) {
            int pipefd[2];
            if (pipe2(pipefd, O_CLOEXEC) == 0) {
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                int read_end = pipefd[0];
                int orig_console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
                std::thread([read_end, log_fd, orig_console]() {
                    char buf[512];
                    while (true) {
                        ssize_t n = read(read_end, buf, sizeof(buf));
                        if (n <= 0) break;
                        write(log_fd, buf, n);
                        if (orig_console >= 0) write(orig_console, buf, n);
                    }
                    close(read_end);
                    close(log_fd);
                    if (orig_console >= 0) close(orig_console);
                }).detach();
            } else {
                close(log_fd);
            }
        }
    }
#endif

    g_start_time = time(nullptr);
    g_boot_mode = config.boot_mode;
    g_inference_fn = stub_inference;  // default; swapped to llama_inference when model loads

    fprintf(stderr, "[child] Inference child started\n");
    fprintf(stderr, "[child] Model: %s\n",
            config.model_path.empty() ? "(stub mode)" : config.model_path.c_str());

    // Determine port: use config, but default to 8080 for non-root development
    int port = config.http_port;
    if (port == 80) {
#ifndef _WIN32
        if (getuid() != 0) {
            port = 8080;
            fprintf(stderr, "[child] Not running as root, using port %d\n", port);
        }
#else
        port = 8080;
#endif
    }

    fprintf(stderr, "[child] Port: %d\n", port);

    // Initialize tools
    register_all_tools(g_tools);
    if (g_boot_mode == "live") {
        register_install_tools(g_tools);
        fprintf(stderr, "[child] Live mode: installer tools enabled\n");
    }
    register_schedule_tools(g_tools, g_scheduler);
    register_auth_tools(g_tools, g_auth);
    register_cluster_tools(g_tools, g_cluster);
    register_update_tools(g_tools);
    register_wifi_tools(g_tools, g_wifi);
    fprintf(stderr, "[child] Registered %d tools\n", g_tools.count());

    // Detect hardware
    g_hwinfo = detect_hardware();
    fprintf(stderr, "[child] CPU: %s (%d cores), RAM: %d MB\n",
            g_hwinfo.cpu_model.c_str(), g_hwinfo.cpu_cores, g_hwinfo.ram_total_mb);

    // Start llama-server if a model is available
#ifndef _WIN32
    if (!config.model_path.empty()) {
        // Extract model filename for display
        std::string model_file = config.model_path;
        auto slash = model_file.rfind('/');
        if (slash != std::string::npos) model_file = model_file.substr(slash + 1);
        g_model_name = model_file;

        // Estimate free RAM after model load
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

    // Start llama-rpc-server for mesh clustering (port 50052)
    // Runs on all nodes — even standalone, so it's ready when peers discover us
#ifndef _WIN32
    if (access("/opt/llamaste/llama-rpc-server", X_OK) == 0) {
        if (spawn_rpc_server()) {
            fprintf(stderr, "[child] RPC server ready for mesh clustering\n");
        } else {
            fprintf(stderr, "[child] RPC server failed to start — mesh clustering unavailable\n");
        }
    } else {
        fprintf(stderr, "[child] llama-rpc-server not found — mesh clustering unavailable\n");
    }
#endif

    // Initialize voice pipeline — desktop mode only.
    // Server mode: no TTS/STT needed (headless; voice is desktop-only UX).
    // Live mode: no espeak-ng data dir or TTS models; installer doesn't need voice.
    VoicePipeline voice_pipeline;
    {
        extern VoicePipeline* g_voice;  // defined in tools_audio.cpp
#ifndef _WIN32
        if (g_boot_mode != "desktop") {
            fprintf(stderr, "[child] %s mode: voice pipeline disabled\n", g_boot_mode.c_str());
        } else {
            VoiceConfig vcfg;
            if (voice_pipeline.init(vcfg)) {
                g_voice = &voice_pipeline;
                fprintf(stderr, "[child] Voice pipeline initialized (tts=%s)\n",
                        voice_pipeline.tts_engine_name().c_str());

                // Desktop mode: enable always-listening (ALSA capture + VAD)
                if (access(vcfg.whisper_model.c_str(), R_OK) == 0) {
                    // Wire command callback: voice commands go through the agent loop
                    voice_pipeline.set_command_callback([&](const std::string& command) {
                        fprintf(stderr, "[voice] Processing command: \"%s\"\n", command.c_str());

                        ConversationState conv;
                        conv.system_prompt = g_system_prompt;
                        conv.add_user_message(command);
                        std::string response = agent_turn(conv, g_tools, g_inference_fn);

                        fprintf(stderr, "[voice] Agent response: %.80s%s\n",
                                response.c_str(),
                                response.size() > 80 ? "..." : "");

                        // Speak the response via TTS
                        if (!response.empty()) {
                            voice_pipeline.speak(response);
                        }
                    });

                    // Start the always-listening thread (ALSA capture + VAD)
                    voice_pipeline.start();
                } else {
                    fprintf(stderr, "[child] Desktop mode: TTS ready, whisper model not found (STT disabled)\n");
                }
            } else {
                fprintf(stderr, "[child] Voice pipeline init failed: %s\n",
                        voice_pipeline.last_error().c_str());
            }
        }
#endif
    }

    // Build system prompt
    g_system_prompt = build_system_prompt(g_hwinfo, g_tools, config.boot_mode);
    fprintf(stderr, "[child] System prompt: %zu bytes\n", g_system_prompt.size());

    // Start mDNS responder so the box is discoverable as "llamaste.local"
    // Also advertise the MCP server as _mcp._tcp so DNS-SD clients can find it.
    MdnsResponder mdns;
    if (mdns.start("llamaste")) {
        fprintf(stderr, "[child] mDNS: responding as llamaste.local (%s)\n",
                MdnsResponder::get_local_ip().c_str());
        mdns.advertise_service("_mcp._tcp", 80,
            {"path=/mcp", "version=2025-03-26", "auth=bearer"});
    } else {
        fprintf(stderr, "[child] mDNS: could not start (non-fatal)\n");
    }

    // --- Mesh clustering ---
    // Advertise RPC service for peer discovery
    {
        std::vector<std::string> rpc_txt;
        rpc_txt.push_back("ram=" + std::to_string(g_hwinfo.ram_total_mb));
        rpc_txt.push_back("cores=" + std::to_string(g_hwinfo.cpu_cores));
        rpc_txt.push_back("rpc_port=" + std::to_string(g_rpc_port));
        rpc_txt.push_back("model=" + (g_model_name.empty() ? "none" : g_model_name));

        mdns.advertise_service("_llama-rpc._tcp", (uint16_t)g_rpc_port, rpc_txt);

        // Set self info for election
        PeerInfo self;
        self.hostname = config.device_name.empty() ? "llamaste" : config.device_name;
        self.ip = MdnsResponder::get_local_ip();
        self.rpc_port = (uint16_t)g_rpc_port;
        self.ram_mb = (uint32_t)g_hwinfo.ram_total_mb;
        self.cpu_cores = (uint32_t)g_hwinfo.cpu_cores;
        self.model = g_model_name.empty() ? "none" : g_model_name;
        g_cluster.set_self_info(self);

        // Discover peers (2-second window)
        auto discovered = mdns.discover_services("_llama-rpc._tcp", 2000);
        for (const auto& d : discovered) {
            if (d.ip == self.ip) continue;  // skip self
            PeerInfo peer;
            peer.hostname = d.hostname;
            peer.ip = d.ip;
            peer.rpc_port = d.port;
            // Parse TXT records
            for (const auto& t : d.txt) {
                auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                std::string key = t.substr(0, eq);
                std::string val = t.substr(eq + 1);
                if (key == "ram") peer.ram_mb = (uint32_t)std::stoul(val);
                else if (key == "cores") peer.cpu_cores = (uint32_t)std::stoul(val);
                else if (key == "model") peer.model = val;
            }
            g_cluster.add_peer(peer);
            fprintf(stderr, "[cluster] Discovered peer: %s (%s) ram=%uMB cores=%u\n",
                    peer.hostname.c_str(), peer.ip.c_str(), peer.ram_mb, peer.cpu_cores);
        }

        // Run election
        g_cluster.run_election();
        fprintf(stderr, "[cluster] Role: %s | Peers: %zu\n",
                g_cluster.role_name().c_str(), g_cluster.peer_count());

        if (g_cluster.is_coordinator() && g_cluster.peer_count() > 0) {
            fprintf(stderr, "[cluster] Coordinator — RPC endpoints: %s\n",
                    g_cluster.rpc_endpoint_list().c_str());
        }

        // Topology change callback: auto-select model + tensor-split for cluster
        g_cluster.set_topology_change_callback([]() {
            if (!g_cluster.is_coordinator()) return;

            auto cap = g_cluster.analyze_capacity();
            fprintf(stderr, "[cluster] Topology changed — %zu nodes, %u MB usable, recommend: %s\n",
                    cap.node_count, cap.usable_ram_mb, cap.recommended_model.c_str());

#ifndef _WIN32
            // Kill existing llama-server
            pid_t pid = g_llama_pid.load();
            if (pid > 0) {
                kill(pid, SIGTERM);
                waitpid(pid, nullptr, 0);
                g_llama_pid.store(0);
                g_model_loaded.store(false);
                g_inference_fn = stub_inference;
                // Reset persistent client — old connection is dead after restart
                { std::lock_guard<std::mutex> lk(g_inference_cli_mutex); g_inference_cli.reset(); }
            }

            // Select best model for current cluster capacity
            ModelTier best = g_cluster.select_model();
            std::string model_path;
            if (!best.name.empty()) {
                model_path = "/data/models/" + best.gguf_file;
                g_model_name = best.gguf_file;
                fprintf(stderr, "[cluster] Auto-selected model: %s (%u MB, %u layers)\n",
                        best.name.c_str(), best.size_mb, best.layers);
            } else if (!g_model_name.empty()) {
                // No tier match, keep current model
                model_path = "/data/models/" + g_model_name;
                fprintf(stderr, "[cluster] Keeping current model: %s\n", g_model_name.c_str());
            } else {
                fprintf(stderr, "[cluster] No model available — staying in stub mode\n");
                // Don't return — fall through so do_auto_upgrade_check() still runs
            }

            // Restart llama-server only if a model was found on disk
            if (!model_path.empty()) {
                // Build RPC endpoints and tensor split
                std::string rpc = g_cluster.rpc_endpoint_list();
                std::string tensor_split = g_cluster.compute_tensor_split();

                int free_ram_estimate = g_hwinfo.ram_free_mb - 512;
                // Use a shorter timeout when RPC peers are involved: if peers are
                // unreachable, llama-server never passes health and we'd block for
                // the full timeout before the fallback can fire.
                int rpc_wait = rpc.empty() ? 120 : 30;
                if (spawn_llama_server(model_path, g_hwinfo.cpu_cores, free_ram_estimate, rpc,
                                       tensor_split)) {
                    if (wait_for_llama_server(rpc_wait)) {
                        g_model_loaded.store(true);
                        g_inference_fn = llama_inference;
                        fprintf(stderr, "[cluster] llama-server restarted: rpc=%s split=%s\n",
                                rpc.c_str(), tensor_split.c_str());
                        // Start a fresh monitor thread for the newly spawned server.
                        // The original monitor from boot exits early (ECHILD) when the
                        // topology callback steals its waitpid, leaving this server unmonitored.
                        std::thread mon(llama_monitor_thread, model_path,
                                        g_hwinfo.cpu_cores, free_ram_estimate);
                        mon.detach();
                    } else if (!rpc.empty()) {
                        // Timed out waiting for llama-server — likely a dead RPC peer.
                        // Kill the stuck process and retry solo so inference stays available.
                        fprintf(stderr, "[cluster] llama-server timed out after %ds (rpc=%s) — retrying solo\n",
                                rpc_wait, rpc.c_str());
                        pid_t stuck = g_llama_pid.load();
                        if (stuck > 0) {
                            kill(stuck, SIGTERM);
                            waitpid(stuck, nullptr, 0);
                            g_llama_pid.store(0);
                        }
                        if (spawn_llama_server(model_path, g_hwinfo.cpu_cores,
                                               free_ram_estimate, "", "")) {
                            if (wait_for_llama_server(60)) {
                                g_model_loaded.store(true);
                                g_inference_fn = llama_inference;
                                fprintf(stderr, "[cluster] llama-server started solo (fallback)\n");
                                std::thread mon(llama_monitor_thread, model_path,
                                                g_hwinfo.cpu_cores, free_ram_estimate);
                                mon.detach();
                            } else {
                                fprintf(stderr, "[cluster] llama-server solo fallback also failed\n");
                            }
                        }
                    }
                }
            }

            // Auto-upgrade: if a better model fits RAM but isn't on disk, download it.
            // Uses shared helper to avoid code duplication with heartbeat path.
            do_auto_upgrade_check(cap);
#endif
        });
    }

    // Cluster heartbeat: re-announce service and expire stale peers every 30s
    std::thread cluster_heartbeat_thread([&mdns]() {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!g_running.load()) break;

            // Re-announce our RPC service
            std::vector<std::string> rpc_txt;
            rpc_txt.push_back("ram=" + std::to_string(g_hwinfo.ram_total_mb));
            rpc_txt.push_back("cores=" + std::to_string(g_hwinfo.cpu_cores));
            rpc_txt.push_back("rpc_port=" + std::to_string(g_rpc_port));
            rpc_txt.push_back("model=" + (g_model_name.empty() ? "none" : g_model_name));
            mdns.advertise_service("_llama-rpc._tcp", (uint16_t)g_rpc_port, rpc_txt);

            // Expire peers not seen in 90 seconds
            g_cluster.expire_peers(90);

            // Re-discover peers
            auto discovered = mdns.discover_services("_llama-rpc._tcp", 1000);
            auto self = g_cluster.self_info();
            for (const auto& d : discovered) {
                if (d.ip == self.ip) continue;
                PeerInfo peer;
                peer.hostname = d.hostname;
                peer.ip = d.ip;
                peer.rpc_port = d.port;
                for (const auto& t : d.txt) {
                    auto eq = t.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = t.substr(0, eq);
                    std::string val = t.substr(eq + 1);
                    if (key == "ram") peer.ram_mb = (uint32_t)std::stoul(val);
                    else if (key == "cores") peer.cpu_cores = (uint32_t)std::stoul(val);
                    else if (key == "model") peer.model = val;
                }
                g_cluster.add_peer(peer);
            }

            // Re-run election (topology may have changed)
            g_cluster.run_election();

#ifndef _WIN32
            // Watchdog: detect if llama-server has crashed without the monitor thread
            // noticing (e.g. when topology callback steals waitpid from the monitor,
            // leaving the new RPC-mode server unmonitored).
            if (g_model_loaded.load()) {
                pid_t lpid = g_llama_pid.load();
                if (lpid > 0 && kill(lpid, 0) != 0 && errno == ESRCH) {
                    fprintf(stderr, "[heartbeat] llama-server (PID %d) died unexpectedly\n",
                            (int)lpid);
                    g_llama_pid.store(0);
                    g_model_loaded.store(false);
                    g_inference_fn = stub_inference;
                    // Reset persistent client — old connection is dead
                    { std::lock_guard<std::mutex> lk(g_inference_cli_mutex); g_inference_cli.reset(); }
                }
            }

            // Recovery: if model is down but was previously loaded, restart solo.
            // Fires within one heartbeat cycle (30s) of any crash — regardless of
            // whether the monitor thread is still watching or has already exited.
            if (!g_model_loaded.load() && g_llama_pid.load() == 0 && !g_model_name.empty()) {
                std::string path = "/data/models/" + g_model_name;
                if (access(path.c_str(), R_OK) == 0) {
                    fprintf(stderr, "[heartbeat] llama-server down — restarting solo\n");
                    int free_ram = g_hwinfo.ram_free_mb - 512;
                    if (spawn_llama_server(path, g_hwinfo.cpu_cores, free_ram)) {
                        if (wait_for_llama_server(60)) {
                            g_model_loaded.store(true);
                            g_inference_fn = llama_inference;
                            fprintf(stderr, "[heartbeat] llama-server recovered (solo)\n");
                            // Fresh monitor thread for the newly started server
                            std::thread mon(llama_monitor_thread, path,
                                            g_hwinfo.cpu_cores, free_ram);
                            mon.detach();
                        }
                    }
                }
            }
#endif

            // Auto-upgrade check: runs every 30s regardless of topology state changes.
            // This handles the standalone-forever case where the topology callback never
            // fires (it only fires on state *transitions*, not stable state).
            if (!g_model_loaded.load()) {
                auto cap = g_cluster.analyze_capacity();
                do_auto_upgrade_check(cap);
            }
        }
    });
    cluster_heartbeat_thread.detach();

    // Start heartbeat scheduler
    g_scheduler.set_data_dir("/data/llamaste");
    g_scheduler.set_inference_fn([](const std::string& prompt) -> std::string {
        // Create a temporary conversation for the scheduled task
        ConversationState conv;
        conv.system_prompt = g_system_prompt;
        conv.add_user_message(prompt);
        return agent_turn(conv, g_tools, g_inference_fn);
    });
    g_scheduler.set_notify_fn([](const Notification& /*notif*/) {
        // Notifications are drained by the SSE endpoint; no-op callback
    });
    g_scheduler.set_model_check_fn([]() -> bool {
        return g_model_loaded.load();
    });
    g_scheduler.start();

    // Initialize device authentication
    g_auth.load_config("/data/config");
    fprintf(stderr, "[child] Auth: setup_complete=%s\n",
            g_auth.is_setup_complete() ? "yes" : "no (first-boot mode)");

    // --- WiFi diagnostics: dump everything visible before attempting init ---
#ifndef _WIN32
    {
        // 1. All network interfaces and whether they look like WiFi
        fprintf(stderr, "[wifi-diag] /sys/class/net interfaces:\n");
        DIR* nd = opendir("/sys/class/net");
        if (nd) {
            struct dirent* ne;
            while ((ne = readdir(nd))) {
                if (ne->d_name[0] == '.') continue;
                char wpath[256], ppath[256];
                snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/wireless", ne->d_name);
                snprintf(ppath, sizeof(ppath), "/sys/class/net/%s/phy80211", ne->d_name);
                bool has_w = access(wpath, F_OK) == 0;
                bool has_p = access(ppath, F_OK) == 0;
                fprintf(stderr, "[wifi-diag]   %s%s%s\n", ne->d_name,
                        has_w ? " [wireless]" : "",
                        has_p ? " [phy80211]" : "");
            }
            closedir(nd);
        } else {
            fprintf(stderr, "[wifi-diag]   cannot open /sys/class/net: %m\n");
        }

        // 2. PCI devices — look for Realtek [10ec] or Intel [8086] WiFi
        fprintf(stderr, "[wifi-diag] /sys/bus/pci/devices (network class 0280):\n");
        DIR* pd = opendir("/sys/bus/pci/devices");
        if (pd) {
            struct dirent* pe;
            while ((pe = readdir(pd))) {
                if (pe->d_name[0] == '.') continue;
                char cpath[256], vpath[256];
                snprintf(cpath, sizeof(cpath), "/sys/bus/pci/devices/%s/class", pe->d_name);
                FILE* cf = fopen(cpath, "r");
                if (!cf) continue;
                unsigned cls = 0;
                fscanf(cf, "%x", &cls);
                fclose(cf);
                if ((cls >> 16) != 0x02) continue; // network class only
                snprintf(vpath, sizeof(vpath), "/sys/bus/pci/devices/%s/vendor", pe->d_name);
                FILE* vf = fopen(vpath, "r"); unsigned vid = 0;
                if (vf) { fscanf(vf, "%x", &vid); fclose(vf); }
                snprintf(vpath, sizeof(vpath), "/sys/bus/pci/devices/%s/device", pe->d_name);
                FILE* df = fopen(vpath, "r"); unsigned did = 0;
                if (df) { fscanf(df, "%x", &did); fclose(df); }
                // Check if driver bound
                char drv[256]; drv[0] = 0;
                snprintf(vpath, sizeof(vpath), "/sys/bus/pci/devices/%s/driver", pe->d_name);
                char lnk[256];
                ssize_t lr = readlink(vpath, lnk, sizeof(lnk)-1);
                if (lr > 0) { lnk[lr] = 0; snprintf(drv, sizeof(drv), " driver=%s", strrchr(lnk,'/')+1); }
                fprintf(stderr, "[wifi-diag]   %s vendor=%04x device=%04x class=%06x%s\n",
                        pe->d_name, vid, did, cls, drv);
            }
            closedir(pd);
        }

        // 3. All rtw* PCI driver directories (tells us what's compiled into kernel)
        fprintf(stderr, "[wifi-diag] /sys/bus/pci/drivers/ rtw* entries:\n");
        DIR* drv_d = opendir("/sys/bus/pci/drivers");
        if (drv_d) {
            struct dirent* de;
            bool found_any = false;
            while ((de = readdir(drv_d))) {
                if (de->d_name[0] == '.') continue;
                if (strncmp(de->d_name, "rtw", 3) == 0 || strncmp(de->d_name, "8821", 4) == 0) {
                    fprintf(stderr, "[wifi-diag]   %s\n", de->d_name);
                    found_any = true;
                }
            }
            if (!found_any) fprintf(stderr, "[wifi-diag]   (none)\n");
            closedir(drv_d);
        }

        // 4. Driver symlink for the RTL8821CE device specifically
        {
            char lnk[512]; lnk[0] = 0;
            ssize_t lr = readlink("/sys/bus/pci/devices/0000:01:00.0/driver", lnk, sizeof(lnk)-1);
            if (lr > 0) { lnk[lr] = 0; fprintf(stderr, "[wifi-diag] 0000:01:00.0 driver: %s\n", lnk); }
            else fprintf(stderr, "[wifi-diag] 0000:01:00.0 driver: NONE (no driver bound)\n");
        }

        // 5. Firmware file accessible?
        fprintf(stderr, "[wifi-diag] rtw8821c_fw.bin: %s\n",
                access("/lib/firmware/rtw88/rtw8821c_fw.bin", F_OK) == 0 ? "found" : "MISSING");

        // 6. Kernel version (confirms which kernel is running)
        {
            FILE* kv = fopen("/proc/version", "r");
            if (kv) { char buf[256]; if (fgets(buf, sizeof(buf), kv)) fprintf(stderr, "[wifi-diag] kernel: %s", buf); fclose(kv); }
        }
    }
#endif

    // Initialize WiFi and start wpa_supplicant + dhcpcd if hardware found
#ifndef _WIN32
    {
        bool wifi_init_ok = g_wifi.init();
        std::string wifi_iface;
        if (wifi_init_ok && g_wifi.has_wifi())
            wifi_iface = g_wifi.status().iface;
        fprintf(stderr, "[wifi] init=%s has_wifi=%s iface='%s' mode='%s'\n",
                wifi_init_ok ? "ok" : "fail",
                g_wifi.has_wifi() ? "yes" : "no",
                wifi_iface.c_str(),
                g_boot_mode.c_str());
    if (wifi_init_ok) {
        if (!wifi_iface.empty()) {
            spawn_wpa_supplicant(wifi_iface);
            // Give wpa_supplicant a moment to create its control socket
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            // Re-open control socket now that daemon is running
            g_wifi.init();

            // Console WiFi setup (first-boot SSID/PSK prompt) runs in the
            // supervisor before this child starts — see supervisor_preflight_wifi().
            // wpa_supplicant starts here with whatever config was saved.

            spawn_dhcpcd(wifi_iface);
        }
    }
    } // end outer wifi block
#endif

    // Start session expiry thread (runs every 60 seconds)
    std::thread session_expiry_thread([]() {
        while (g_running) {
            g_auth.expire_sessions();
            for (int i = 0; i < 120 && g_running; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
    });
    session_expiry_thread.detach();

    // --- Desktop mode: launch Wayland compositor ---
    //
    // Primary: cage + cog (kiosk compositor via wlroots).
    //   cage is purpose-built for single-app fullscreen kiosk mode.
    //   wlroots implements zwp_text_input_v3 so typing in cog/WebKit forms works.
    //   labwc 0.6.6 (Buildroot 2024.02.9) lacks text-input-v3 — typing is broken.
    //
    // Fallback 1: labwc (full WM, reads /etc/labwc/ config)
    // Fallback 2: weston (most compatible DRM backend)
#ifndef _WIN32
    if (g_boot_mode == "desktop") {
        fprintf(stderr, "[child] Desktop mode: launching cage+cog compositor\n");

        // Wayland runtime dir (required by wlroots)
        mkdir("/run/user", 0755);
        mkdir("/run/user/0", 0700);
        setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);

        // WLR_LIBINPUT_NO_DEVICES=1: let cage start even if libinput finds 0 devices.
        // Without this, wlroots aborts with "No input devices found" when udev hasn't
        // enumerated devices yet. The flag does NOT prevent real devices from working —
        // it only suppresses the hard-abort-on-zero-devices check.
        setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);

        // simpledrm (EFI framebuffer DRM) does not support hardware cursors.
        // Without this flag wlroots aborts during cursor plane setup on real hardware
        // when no native GPU driver is present (Xe/i915 firmware not yet loaded).
        setenv("WLR_NO_HARDWARE_CURSORS", "1", 1);

        // Force pixman software renderer as fallback when no GPU driver is active.
        // wlroots will use GL/Vulkan if available; pixman ensures we always get a
        // display even with simpledrm only (no 3D acceleration).
        // Can be overridden by setting WLR_RENDERER=gles2 from /etc/labwc/autostart
        // once a proper GPU driver is confirmed.
        if (!getenv("WLR_RENDERER"))
            setenv("WLR_RENDERER", "pixman", 1);

        // Point XDG config to /etc so labwc reads /etc/labwc/rc.xml etc.
        setenv("XDG_CONFIG_DIRS", "/etc", 1);
        setenv("XDG_CONFIG_HOME", "/etc", 1);

        // Required for wlroots-based compositors (labwc, cage, weston) to run
        // without seatd or logind — when running as PID 1 there is no seat manager.
        setenv("LIBSEAT_BACKEND", "noop", 1);

        // XKB keymap data root — libxkbcommon looks here for rules/symbols/etc.
        // xkeyboard-config installs to /usr/share/X11/xkb. Without this explicit
        // path, libxkbcommon may use a compile-time default that doesn't exist on
        // this rootfs. If the keymap can't be compiled, wlroots stores a NULL
        // keyboard pointer and segfaults (SIGSEGV) on the first key press.
        if (!getenv("XKB_CONFIG_ROOT"))
            setenv("XKB_CONFIG_ROOT", "/usr/share/X11/xkb", 1);

        // Disable atomic KMS — simpledrm (EFI framebuffer DRM) on bare metal
        // often doesn't support atomic modesetting. Without this flag wlroots
        // probes atomic ioctls, gets unexpected results, and segfaults (signal 11).
        setenv("WLR_DRM_NO_ATOMIC", "1", 1);

        // PATH for autostart script and child processes
        setenv("PATH", "/usr/bin:/usr/sbin:/bin:/sbin", 1);

        // Force Wayland backend for GTK/Qt apps launched from autostart
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);

        // Start udevd so it can assign udev properties (ID_SEAT etc.) that
        // libinput uses for device enumeration. We do NOT run udevadm trigger
        // here — it runs later, AFTER cage creates its Wayland socket. Running
        // trigger too early causes a race: keyboard device is enumerated while
        // cage is still initializing → wlroots fails to allocate the shm file
        // for the XKB keymap ("Failed to allocate shm file for XKB keymap:
        // [errno]") → SIGSEGV on first key press. Delaying until the socket
        // exists ensures wlroots is fully ready before handling any devices.
        {
            pid_t upid = fork();
            if (upid == 0) {
                int null_fd = open("/dev/null", O_WRONLY);
                if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
                execl("/sbin/udevd", "udevd", "--daemon", nullptr);
                execl("/usr/sbin/udevd", "udevd", "--daemon", nullptr);
                _exit(1);
            } else if (upid > 0) {
                fprintf(stderr, "[child] Started udevd pid=%d\n", upid);
                sleep(1);  // give udevd time to initialise before compositor starts
            } else {
                fprintf(stderr, "[child] fork for udevd failed: %m\n");
            }
        }


        // Compositor spawn with crash fallback chain.
        //
        // Strategy: cage is primary (wlroots, supports zwp_text_input_v3).
        //   - cage+cog are restarted up to 5 times if cage crashes or cog crashes.
        //   - If cage exits cleanly (user action, exit 0) → stop.
        //   - If exec fails (exit 127, binary not found) → fall through to next.
        // Fallback 1: labwc (stacking WM). Autostart launches cog.
        // Fallback 2: weston (most protocol-complete, own DRM backend).
        //
        // KEY REQUIREMENT: xkeyboard-config must be installed (BR2_PACKAGE_XKEYBOARD_CONFIG=y).
        // Without XKB data files, xkb_keymap_new_from_names() returns NULL and wlroots
        // segfaults (SIGSEGV) on the first key press. XKB_CONFIG_ROOT env is also set above.
        std::thread([]() {
            // Helper: run udevadm settle then trigger --action=add for input devices.
            // IMPORTANT: must use --action=add (not the default "change") because
            // libinput only reacts to ADD events to register new input devices.
            // Without --action=add, libinput never sees the keyboard/touchpad and
            // key events are silently dropped even though cage doesn't crash.
            // --subsystem-match=input limits scope to only input devices (faster).
            auto run_udevadm_trigger = []() {
                // First: settle — wait for udevd to process all pending events
                {
                    pid_t tpid = fork();
                    if (tpid == 0) {
                        int null_fd = open("/dev/null", O_WRONLY);
                        if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
                        execl("/sbin/udevadm",     "udevadm", "settle", "--timeout=3", nullptr);
                        execl("/usr/sbin/udevadm", "udevadm", "settle", "--timeout=3", nullptr);
                        execl("/usr/bin/udevadm",  "udevadm", "settle", "--timeout=3", nullptr);
                        _exit(1);
                    }
                    if (tpid > 0) waitpid(tpid, nullptr, 0);
                }
                // Then: trigger ADD events for input subsystem → libinput picks up devices
                {
                    pid_t tpid = fork();
                    if (tpid == 0) {
                        int null_fd = open("/dev/null", O_WRONLY);
                        if (null_fd >= 0) { dup2(null_fd, STDOUT_FILENO); dup2(null_fd, STDERR_FILENO); close(null_fd); }
                        execl("/sbin/udevadm",     "udevadm", "trigger", "--action=add", "--subsystem-match=input", nullptr);
                        execl("/usr/sbin/udevadm", "udevadm", "trigger", "--action=add", "--subsystem-match=input", nullptr);
                        execl("/usr/bin/udevadm",  "udevadm", "trigger", "--action=add", "--subsystem-match=input", nullptr);
                        _exit(1);
                    }
                    if (tpid > 0) {
                        waitpid(tpid, nullptr, 0);
                        fprintf(stderr, "[child] udevadm trigger --action=add done\n");
                    }
                }
                // Log what input devices are now visible so we can diagnose missing devices
                {
                    DIR* dir = opendir("/dev/input");
                    if (dir) {
                        fprintf(stderr, "[child] /dev/input devices after trigger:");
                        struct dirent* ent;
                        while ((ent = readdir(dir))) {
                            if (ent->d_name[0] != '.') fprintf(stderr, " %s", ent->d_name);
                        }
                        fprintf(stderr, "\n");
                        closedir(dir);
                    } else {
                        fprintf(stderr, "[child] /dev/input not accessible: %m\n");
                    }
                }
            };

            // Helper: wait for the compositor's Wayland socket to appear.
            // Returns true if socket appeared within timeout_ms, false otherwise.
            auto wait_for_socket = [](int timeout_ms) -> bool {
                const char* path = "/run/user/0/wayland-0";
                for (int i = 0; i < timeout_ms / 100; i++) {
                    if (access(path, F_OK) == 0) return true;
                    usleep(100000);
                }
                return false;
            };

            // Helper: try one compositor, return its waitpid raw status.
            // After the compositor socket appears, runs udevadm trigger so
            // input devices are enumerated AFTER wlroots is fully initialised.
            // This prevents the shm-for-XKB race: keyboard enumerated while
            // cage is still starting → os_create_anonymous_file() fails → SIGSEGV.
            // Returns -1 if fork failed.
            static bool udev_triggered = false;
            auto try_compositor = [&](const char* label,
                                      std::function<void()> exec_fn) -> int {
                pid_t pid = fork();
                if (pid == 0) {
                    exec_fn();
                    _exit(127);
                }
                if (pid < 0) {
                    fprintf(stderr, "[child] fork for %s failed: %m\n", label);
                    return -1;
                }
                g_cage_pid.store(pid);

                // First compositor start: wait for socket then trigger udev.
                // Subsequent restarts: socket may take a moment; trigger again
                // so hot-plug events fire if devices were missed.
                if (wait_for_socket(5000)) {
                    if (!udev_triggered) {
                        // Small delay: socket exists but wlroots is still completing
                        // startup internally (especially libinput udev monitor setup).
                        // Without this, trigger fires while libinput isn't ready to
                        // handle ADD events → keyboard still silently dropped.
                        usleep(500000);  // 500ms
                        fprintf(stderr, "[child] %s socket ready, triggering udev\n", label);
                        run_udevadm_trigger();
                        udev_triggered = true;
                    }
                } else {
                    fprintf(stderr, "[child] %s socket not ready after 5s\n", label);
                }

                auto start_time = std::chrono::steady_clock::now();
                int status = 0;
                waitpid(pid, &status, 0);
                g_cage_pid.store(0);

                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count();
                if (WIFSIGNALED(status))
                    fprintf(stderr, "[child] %s crashed signal=%d (ran %lds)\n",
                            label, WTERMSIG(status), (long)elapsed);
                else
                    fprintf(stderr, "[child] %s exited code=%d (ran %lds)\n",
                            label, WEXITSTATUS(status), (long)elapsed);
                return status;
            };

            // Helper: "was this a clean intentional exit (exit 0, ran > 5s)?"
            // exit 0 after >5s → user-initiated close, stop chain.
            // exit 0 within 5s → probably startup failure, try next/retry.
            // signal            → crashed, retry/try next.
            // exit 127          → exec failed (not installed), try next.
            auto is_clean_exit = [](int st, long elapsed_secs) -> bool {
                return WIFEXITED(st) && WEXITSTATUS(st) == 0 && elapsed_secs > 5;
            };

            // 1. cage + cog (kiosk compositor — wlroots-based, lightweight).
            //    cage 0.1.5 supports zwp_text_input_v3 via wlroots.
            //    Restart up to 5 times so a transient cog crash doesn't kill the UI.
            {
                bool cage_ran_cleanly = false;
                for (int attempt = 0; attempt < 5; attempt++) {
                    if (attempt > 0) {
                        fprintf(stderr, "[child] cage+cog restart %d/5\n", attempt + 1);
                        sleep(1);
                        // Remove stale wayland socket from crashed cage instance
                        unlink("/run/user/0/wayland-0");
                    }
                    auto t0 = std::chrono::steady_clock::now();
                    int st = try_compositor("cage+cog", []() {
                        execl("/usr/bin/cage", "cage", "-s", "--",
                              "/usr/bin/cog", "http://localhost", nullptr);
                    });
                    long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    if (st < 0) break;  // fork failed
                    if (is_clean_exit(st, elapsed)) { cage_ran_cleanly = true; break; }
                    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) break;  // not installed
                    // Crashed, quick exit, or non-zero → retry
                }
                if (cage_ran_cleanly) return;
            }

            fprintf(stderr, "[child] cage unavailable/crashed, trying labwc\n");

            // 2. labwc (stacking WM — reads /etc/labwc/autostart which launches cog)
            {
                auto t0 = std::chrono::steady_clock::now();
                int st = try_compositor("labwc", []() {
                    execl("/usr/bin/labwc", "labwc", nullptr);
                });
                long elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - t0).count();
                if (st >= 0 && is_clean_exit(st, elapsed)) return;
            }

            fprintf(stderr, "[child] labwc unavailable/crashed, trying weston\n");

            // 3. weston (most robust fallback — own DRM backend, broad protocol support)
            {
                try_compositor("weston", []() {
                    execl("/usr/bin/weston", "weston", "--shell=kiosk",
                          "--continue-without-input", nullptr);
                });
            }
        }).detach();
    }
#endif

    // Create HTTP server
    httplib::Server svr;

    // CORS headers for development
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
    });

    // Handle OPTIONS preflight requests
    svr.Options(".*", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.status = 204;
    });

    // --- Auth helper: wraps route handlers to require authentication ---
    auto require_auth = [](std::function<void(const httplib::Request&, httplib::Response&)> handler) {
        return [handler](const httplib::Request& req, httplib::Response& res) {
            // Desktop mode: all requests come from cog on localhost — no auth needed.
            // Remove this bypass once keyboard input in Wayland is confirmed working.
            if (g_boot_mode == "desktop") {
                handler(req, res);
                return;
            }
            if (!g_auth.is_authenticated(req)) {
                res.status = 401;
                json err;
                err["error"] = "Unauthorized";
                err["login_url"] = "/login.html";
                res.set_content(err.dump(), "application/json");
                return;
            }
            handler(req, res);
        };
    };

    // --- Static file routes ---
    // Root route: serve main UI, login page, or setup page based on auth state
    svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        // Desktop mode: skip auth/setup gates — go straight to the main UI.
        // cog only accesses localhost so trust is implicit.
        if (g_boot_mode == "desktop") {
#ifdef LLAMASTE_HAS_EMBED
            extern const unsigned char WEB_INDEX_HTML[];
            extern const unsigned int WEB_INDEX_HTML_LEN;
            serve_static_file(req, res, "index.html", WEB_INDEX_HTML, WEB_INDEX_HTML_LEN);
#else
            serve_static_file(req, res, "index.html", nullptr, 0);
#endif
            return;
        }

        // If setup not complete, redirect to setup
        if (!g_auth.is_setup_complete()) {
#ifdef LLAMASTE_HAS_EMBED
            extern const unsigned char WEB_SETUP_HTML[];
            extern const unsigned int WEB_SETUP_HTML_LEN;
            serve_static_file(req, res, "setup.html", WEB_SETUP_HTML, WEB_SETUP_HTML_LEN);
#else
            serve_static_file(req, res, "setup.html", nullptr, 0);
#endif
            return;
        }

        // If not authenticated, show login page
        if (!g_auth.is_authenticated(req)) {
#ifdef LLAMASTE_HAS_EMBED
            extern const unsigned char WEB_LOGIN_HTML[];
            extern const unsigned int WEB_LOGIN_HTML_LEN;
            serve_static_file(req, res, "login.html", WEB_LOGIN_HTML, WEB_LOGIN_HTML_LEN);
#else
            serve_static_file(req, res, "login.html", nullptr, 0);
#endif
            return;
        }

        // Authenticated: serve main UI
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_INDEX_HTML[];
        extern const unsigned int WEB_INDEX_HTML_LEN;
        serve_static_file(req, res, "index.html", WEB_INDEX_HTML, WEB_INDEX_HTML_LEN);
#else
        serve_static_file(req, res, "index.html", nullptr, 0);
#endif
    });

    svr.Get("/style.css", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_STYLE_CSS[];
        extern const unsigned int WEB_STYLE_CSS_LEN;
        serve_static_file(req, res, "style.css", WEB_STYLE_CSS, WEB_STYLE_CSS_LEN);
#else
        serve_static_file(req, res, "style.css", nullptr, 0);
#endif
    });

    svr.Get("/chat.js", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_CHAT_JS[];
        extern const unsigned int WEB_CHAT_JS_LEN;
        serve_static_file(req, res, "chat.js", WEB_CHAT_JS, WEB_CHAT_JS_LEN);
#else
        serve_static_file(req, res, "chat.js", nullptr, 0);
#endif
    });

    svr.Get("/dashboard.js", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_DASHBOARD_JS[];
        extern const unsigned int WEB_DASHBOARD_JS_LEN;
        serve_static_file(req, res, "dashboard.js", WEB_DASHBOARD_JS, WEB_DASHBOARD_JS_LEN);
#else
        serve_static_file(req, res, "dashboard.js", nullptr, 0);
#endif
    });

    svr.Get("/files.js", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_FILES_JS[];
        extern const unsigned int WEB_FILES_JS_LEN;
        serve_static_file(req, res, "files.js", WEB_FILES_JS, WEB_FILES_JS_LEN);
#else
        serve_static_file(req, res, "files.js", nullptr, 0);
#endif
    });

    svr.Get("/system.js", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_SYSTEM_JS[];
        extern const unsigned int WEB_SYSTEM_JS_LEN;
        serve_static_file(req, res, "system.js", WEB_SYSTEM_JS, WEB_SYSTEM_JS_LEN);
#else
        serve_static_file(req, res, "system.js", nullptr, 0);
#endif
    });

    svr.Get("/notifications.js", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_NOTIFICATIONS_JS[];
        extern const unsigned int WEB_NOTIFICATIONS_JS_LEN;
        serve_static_file(req, res, "notifications.js", WEB_NOTIFICATIONS_JS, WEB_NOTIFICATIONS_JS_LEN);
#else
        serve_static_file(req, res, "notifications.js", nullptr, 0);
#endif
    });

    // Login and setup pages (always accessible, no auth required)
    svr.Get("/login.html", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_LOGIN_HTML[];
        extern const unsigned int WEB_LOGIN_HTML_LEN;
        serve_static_file(req, res, "login.html", WEB_LOGIN_HTML, WEB_LOGIN_HTML_LEN);
#else
        serve_static_file(req, res, "login.html", nullptr, 0);
#endif
    });

    svr.Get("/setup.html", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_SETUP_HTML[];
        extern const unsigned int WEB_SETUP_HTML_LEN;
        serve_static_file(req, res, "setup.html", WEB_SETUP_HTML, WEB_SETUP_HTML_LEN);
#else
        serve_static_file(req, res, "setup.html", nullptr, 0);
#endif
    });

    // --- Auth API routes (no auth required) ---

    // POST /llamaste/auth/setup — First-boot password setup
    svr.Post("/llamaste/auth/setup", [](const httplib::Request& req, httplib::Response& res) {
        if (g_auth.is_setup_complete()) {
            res.status = 403;
            json err;
            err["error"] = "Setup already complete";
            res.set_content(err.dump(), "application/json");
            return;
        }

        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.status = 400;
            json err;
            err["error"] = "Invalid JSON";
            res.set_content(err.dump(), "application/json");
            return;
        }

        std::string password = body.value("password", "");
        if (password.size() < 4) {
            res.status = 400;
            json err;
            err["error"] = "Password must be at least 4 characters";
            res.set_content(err.dump(), "application/json");
            return;
        }

        if (!g_auth.set_device_password(password)) {
            res.status = 500;
            json err;
            err["error"] = "Failed to set password";
            res.set_content(err.dump(), "application/json");
            return;
        }

        // Create session and set cookie
        std::string token = g_auth.create_session();
        g_auth.set_session_cookie(res, token);

        json result;
        result["success"] = true;
        result["api_key"] = g_auth.get_api_key();
        result["message"] = "Device password set. Save your API key for programmatic access.";
        res.set_content(result.dump(), "application/json");
    });

    // POST /llamaste/auth/login — Verify password and create session
    svr.Post("/llamaste/auth/login", [](const httplib::Request& req, httplib::Response& res) {
        // Check rate limiting
        std::string client_ip = req.remote_addr;
        if (g_auth.is_rate_limited(client_ip)) {
            res.status = 429;
            json err;
            err["error"] = "Too many failed attempts. Try again in 30 seconds.";
            res.set_content(err.dump(), "application/json");
            return;
        }

        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.status = 400;
            json err;
            err["error"] = "Invalid JSON";
            res.set_content(err.dump(), "application/json");
            return;
        }

        std::string password = body.value("password", "");
        if (!g_auth.verify_device_password(password)) {
            bool rate_limited = g_auth.record_failed_login(client_ip);
            res.status = 401;
            json err;
            err["error"] = "Invalid password";
            if (rate_limited) {
                err["error"] = "Too many failed attempts. Locked for 30 seconds.";
            }
            res.set_content(err.dump(), "application/json");
            return;
        }

        // Create session and set cookie
        std::string token = g_auth.create_session();
        g_auth.set_session_cookie(res, token);

        json result;
        result["success"] = true;
        res.set_content(result.dump(), "application/json");
    });

    // POST /llamaste/auth/logout — Invalidate session
    svr.Post("/llamaste/auth/logout", [](const httplib::Request& req, httplib::Response& res) {
        std::string sid = AuthManager::extract_session_cookie(req);
        if (!sid.empty()) {
            g_auth.invalidate_session(sid);
        }
        AuthManager::clear_session_cookie(res);

        json result;
        result["success"] = true;
        res.set_content(result.dump(), "application/json");
    });

    // GET /llamaste/auth/status — Check auth state
    svr.Get("/llamaste/auth/status", [](const httplib::Request& req, httplib::Response& res) {
        json result;
        result["setup_complete"] = g_auth.is_setup_complete();
        result["authenticated"] = g_auth.is_authenticated(req);
        res.set_content(result.dump(), "application/json");
    });

    // GET /llamaste/debug/resize-log — Read init resize diagnostic log
    svr.Get("/llamaste/debug/resize-log", [](const httplib::Request& req, httplib::Response& res) {
        std::ifstream f("/tmp/init-resize.log");
        if (f.is_open()) {
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            res.set_content(content, "text/plain");
        } else {
            res.set_content("No resize log found (init did not run resize or not PID 1)\n", "text/plain");
        }
    });

    // --- Power control routes (protected) ---
    // POST /llamaste/shutdown — Clean shutdown via supervisor (SIGTERM to PID 1)
    svr.Post("/llamaste/shutdown", require_auth([](const httplib::Request& req, httplib::Response& res) {
        json result;
        result["status"] = "shutdown initiated";
        res.set_content(result.dump(), "application/json");
        // Signal supervisor (PID 1) to shut down after response is sent
        kill(getppid(), SIGTERM);
    }));

    // POST /llamaste/reboot — Clean reboot via supervisor (SIGUSR1 to PID 1)
    svr.Post("/llamaste/reboot", require_auth([](const httplib::Request& req, httplib::Response& res) {
        json result;
        result["status"] = "reboot initiated";
        res.set_content(result.dump(), "application/json");
        // Signal supervisor (PID 1) to reboot after response is sent
        kill(getppid(), SIGUSR1);
    }));

    // --- API routes (protected) ---
    svr.Post("/llamaste/chat", require_auth(handle_chat));

    svr.Get("/llamaste/system",
        require_auth([&config](const httplib::Request& req, httplib::Response& res) {
            handle_system(req, res, config);
        })
    );

    svr.Get("/llamaste/tools", require_auth(handle_tools));
    svr.Get("/llamaste/conversations", require_auth(handle_conversations));
    svr.Post("/v1/chat/completions", require_auth(handle_openai_completions));
    svr.Get("/health", handle_health);  // Health check always accessible

#ifdef LLAMASTE_TEST_API
    // Direct tool dispatch — TEST BUILDS ONLY, not compiled into production.
    // Allows calling any tool by name without going through the LLM agent loop.
    // Usage: POST /llamaste/tool {"name":"tool.name","arguments":{...}}
    svr.Post("/llamaste/tool", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("name")) {
            res.status = 400;
            res.set_content(R"json({"error":"JSON body with 'name' required"})json",
                            "application/json");
            return;
        }
        std::string name = body.value("name", "");
        std::string args = "{}";
        if (body.contains("arguments")) {
            args = body["arguments"].dump();
        }
        std::string result = g_tools.dispatch(name, args);
        res.set_content(result, "application/json");
    }));
    fprintf(stderr, "[child] TEST API: POST /llamaste/tool enabled\n");
#endif

    // --- Files API route (protected) ---
    svr.Get("/llamaste/files", require_auth([](const httplib::Request& req, httplib::Response& res) {
        std::string path = req.get_param_value("path");
        std::string action = req.get_param_value("action");
        if (path.empty()) path = "/data";

        json args;
        args["path"] = path;

        std::string result;
        if (action == "read") {
            result = g_tools.dispatch("fs.read_file", args.dump());
        } else if (action.empty() || action == "list") {
            result = g_tools.dispatch("fs.list_directory", args.dump());
        } else {
            json err;
            err["error"] = "Unknown action: " + action;
            res.status = 400;
            res.set_content(err.dump(), "application/json");
            return;
        }
        res.set_content(result, "application/json");
    }));

    // --- Model download route (protected) ---
    svr.Post("/llamaste/model/download-recommended", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        // Step 1: Get recommended model
        std::string rec_result = g_tools.dispatch("model.recommended", "{}");
        json rec = json::parse(rec_result, nullptr, false);

        if (rec.is_discarded() || !rec.value("recommended", false)) {
            res.set_content(rec_result, "application/json");
            return;
        }

        // Check if already downloaded
        if (rec.value("already_downloaded", false)) {
            json out;
            out["status"] = "already_exists";
            out["model_name"] = rec.value("model_name", "");
            out["filename"] = rec.value("filename", "");
            out["message"] = "Recommended model is already downloaded.";
            res.set_content(out.dump(2), "application/json");
            return;
        }

        // Step 2: Start download
        json dl_args;
        dl_args["repo_id"] = rec.value("repo_id", "");
        dl_args["filename"] = rec.value("filename", "");
        std::string dl_result = g_tools.dispatch("model.download", dl_args.dump());

        res.set_content(dl_result, "application/json");
    }));

    svr.Get("/llamaste/model/recommended", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string result = g_tools.dispatch("model.recommended", "{}");
        res.set_content(result, "application/json");
    }));

    // GET /llamaste/model/list — List all downloaded models
    svr.Get("/llamaste/model/list", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string result = g_tools.dispatch("model.list", "{}");
        res.set_content(result, "application/json");
    }));

    // GET /llamaste/model/current — Currently loaded model info
    svr.Get("/llamaste/model/current", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string result = g_tools.dispatch("model.current", "{}");
        res.set_content(result, "application/json");
    }));

    // POST /llamaste/model/select — Set model override (takes effect on reboot)
    svr.Post("/llamaste/model/select", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("path")) {
            res.status = 400;
            res.set_content(R"json({"error":"JSON body with 'path' required"})json",
                            "application/json");
            return;
        }
        std::string model_path = body.value("path", "");

        // Validate: empty = clear override, otherwise must be under /data/models/
        if (!model_path.empty() && model_path.find("/data/models/") != 0) {
            res.status = 400;
            res.set_content(R"json({"error":"Model path must be under /data/models/"})json",
                            "application/json");
            return;
        }

        // Save via config.set
        json args;
        args["key"] = "model.path";
        args["value"] = model_path;  // empty clears the override
        g_tools.dispatch("config.set", args.dump());

        json out;
        out["status"] = "ok";
        out["model_path"] = model_path;
        out["message"] = model_path.empty()
            ? "Model override cleared. Auto-select will be used on next boot."
            : "Model selected. Reboot to load: " + model_path;
        res.set_content(out.dump(), "application/json");
    }));

    // POST /llamaste/model/download — Download any model from HuggingFace
    svr.Post("/llamaste/model/download", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("repo_id") || !body.contains("filename")) {
            res.status = 400;
            res.set_content(R"json({"error":"JSON body with 'repo_id' and 'filename' required"})json",
                            "application/json");
            return;
        }
        std::string result = g_tools.dispatch("model.download", req.body);
        res.set_content(result, "application/json");
    }));

    // GET /llamaste/model/usb/scan — Scan USB drives for GGUF files
    svr.Get("/llamaste/model/usb/scan", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string result = g_tools.dispatch("model.usb_import",
            R"json({"action":"scan"})json");
        res.set_content(result, "application/json");
    }));

    // POST /llamaste/model/usb/import — Import GGUF file from USB
    svr.Post("/llamaste/model/usb/import", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("device") || !body.contains("filename")) {
            res.status = 400;
            res.set_content(R"json({"error":"JSON body with 'device' and 'filename' required"})json",
                            "application/json");
            return;
        }
        json args;
        args["action"] = "import";
        args["device"] = body.value("device", "");
        args["filename"] = body.value("filename", "");
        std::string result = g_tools.dispatch("model.usb_import", args.dump());
        res.set_content(result, "application/json");
    }));

    // --- Network config routes (protected) ---
    svr.Get("/llamaste/network/config", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string result = g_tools.dispatch("network.get_ip", "{}");
        res.set_content(result, "application/json");
    }));

    svr.Post("/llamaste/network/config", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.status = 400;
            res.set_content(R"json({"error":"invalid JSON"})json", "application/json");
            return;
        }
        std::string result = g_tools.dispatch("network.set_ip", req.body);
        res.set_content(result, "application/json");
    }));

    // --- Voice I/O audio endpoints ---

    // Helper: encode int16 PCM samples as in-memory WAV bytes
    // Used by /llamaste/audio/tts to return audio/wav to the browser.
    // (Defined as a lambda so it can capture nothing and be self-contained.)
    auto encode_wav_for_http = [](const std::vector<int16_t>& samples, int sample_rate) -> std::string {
        uint32_t data_size = (uint32_t)(samples.size() * 2);
        std::string wav(44 + data_size, '\0');
        char* p = &wav[0];
        auto put32 = [&](uint32_t v) { memcpy(p, &v, 4); p += 4; };
        auto put16 = [&](uint16_t v) { memcpy(p, &v, 2); p += 2; };
        memcpy(p, "RIFF", 4); p += 4;
        put32(36 + data_size);
        memcpy(p, "WAVE", 4); p += 4;
        memcpy(p, "fmt ", 4); p += 4;
        put32(16);                              // fmt chunk size
        put16(1);                               // PCM
        put16(1);                               // mono
        put32((uint32_t)sample_rate);           // sample rate
        put32((uint32_t)sample_rate * 2);       // byte rate (rate * channels * bps/8)
        put16(2);                               // block align
        put16(16);                              // bits per sample
        memcpy(p, "data", 4); p += 4;
        put32(data_size);
        memcpy(p, samples.data(), data_size);
        return wav;
    };


    svr.Post("/llamaste/audio/transcribe", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
            if (req.has_file("audio")) {
                // Multipart upload
                const auto& file = req.get_file_value("audio");
                std::string tmp_path = "/data/tmp/audio_upload_" +
                    std::to_string(time(nullptr)) + ".wav";
#ifndef _WIN32
                {
                    std::ofstream ofs(tmp_path, std::ios::binary);
                    ofs.write(file.content.data(), file.content.size());
                }
                json args;
                args["audio_file"] = tmp_path;
                std::string result = g_tools.dispatch("audio.transcribe", args.dump());
                unlink(tmp_path.c_str());
                res.set_content(result, "application/json");
#else
                res.status = 501;
                res.set_content(R"json({"error":"not available on Windows"})json", "application/json");
#endif
            } else if (!req.body.empty()) {
                // Raw WAV body
                std::string tmp_path = "/data/tmp/audio_upload_" +
                    std::to_string(time(nullptr)) + ".wav";
#ifndef _WIN32
                {
                    std::ofstream ofs(tmp_path, std::ios::binary);
                    ofs.write(req.body.data(), req.body.size());
                }
                json args;
                args["audio_file"] = tmp_path;
                std::string result = g_tools.dispatch("audio.transcribe", args.dump());
                unlink(tmp_path.c_str());
                res.set_content(result, "application/json");
#else
                res.status = 501;
                res.set_content(R"json({"error":"not available on Windows"})json", "application/json");
#endif
            } else {
                res.status = 400;
                res.set_content(R"json({"error":"No audio data provided"})json", "application/json");
            }
        }
    ));

    svr.Post("/llamaste/audio/speak", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded()) {
                res.status = 400;
                res.set_content(R"json({"error":"Invalid JSON"})json", "application/json");
                return;
            }
            std::string result = g_tools.dispatch("audio.speak", req.body);
            res.set_content(result, "application/json");
        }
    ));

    // POST /llamaste/audio/tts — synthesize text and return WAV binary for browser playback
    svr.Post("/llamaste/audio/tts", require_auth(
        [encode_wav_for_http](const httplib::Request& req, httplib::Response& res) {
#ifndef _WIN32
            extern VoicePipeline* g_voice;
            if (!g_voice) {
                res.status = 503;
                res.set_content(R"json({"error":"voice pipeline not available (desktop mode required)"})json",
                                "application/json");
                return;
            }
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.contains("text")) {
                res.status = 400;
                res.set_content(R"json({"error":"text field required"})json", "application/json");
                return;
            }
            std::string text = body["text"].get<std::string>();
            if (text.empty()) {
                res.status = 400;
                res.set_content(R"json({"error":"text must not be empty"})json", "application/json");
                return;
            }
            // Cap synthesis length to avoid very long audio
            if (text.size() > 1000) text = text.substr(0, 1000);

            int sample_rate = 8000; // default; overwritten by flite actual rate
            auto pcm = g_voice->speak(text, &sample_rate);
            if (pcm.empty()) {
                res.status = 500;
                res.set_content(R"json({"error":"TTS synthesis failed or TTS not configured"})json",
                                "application/json");
                return;
            }
            std::string wav = encode_wav_for_http(pcm, sample_rate);
            res.set_content(wav, "audio/wav");
#else
            res.status = 501;
            res.set_content(R"json({"error":"not available on Windows"})json", "application/json");
#endif
        }
    ));

    svr.Get("/llamaste/audio/status", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
            std::string result = g_tools.dispatch("audio.status", "{}");
            res.set_content(result, "application/json");
        }
    ));

    svr.Get("/llamaste/audio/config", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
            std::string result = g_tools.dispatch("audio.config", "{}");
            res.set_content(result, "application/json");
        }
    ));

    svr.Post("/llamaste/audio/config", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
            std::string result = g_tools.dispatch("audio.config", req.body);
            res.set_content(result, "application/json");
        }
    ));

    // --- Installer routes (live mode only) ---
    if (g_boot_mode == "live") {
        svr.Get("/install/disks", [](const httplib::Request& /*req*/, httplib::Response& res) {
            std::string result = g_tools.dispatch("install.detect_disks", "{}");
            res.set_content(result, "application/json");
        });

        svr.Post("/install/start", [](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded() || !body.contains("device")) {
                res.status = 400;
                json err;
                err["error"] = "Missing 'device' field";
                res.set_content(err.dump(), "application/json");
                return;
            }
            // Force confirm for HTTP API
            body["confirm"] = true;
            std::string result = g_tools.dispatch("install.to_disk", body.dump());
            res.set_content(result, "application/json");
        });

        svr.Get("/install/progress", [](const httplib::Request& /*req*/, httplib::Response& res) {
            std::string result = g_tools.dispatch("install.progress", "{}");
            res.set_content(result, "application/json");
        });

        // SSE endpoint for streaming progress updates
        svr.Get("/install/progress/stream",
            [](const httplib::Request& /*req*/, httplib::Response& res) {
                res.set_header("Content-Type", "text/event-stream");
                res.set_header("Cache-Control", "no-cache");
                res.set_header("Connection", "keep-alive");

                res.set_chunked_content_provider(
                    "text/event-stream",
                    [](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                        int last_pct = -1;
                        while (true) {
                            std::string progress = g_tools.dispatch("install.progress", "{}");
                            auto pdata = json::parse(progress, nullptr, false);
                            int pct = pdata.value("percent", 0);

                            if (pct != last_pct) {
                                std::string event = "data: " + progress + "\n\n";
                                sink.write(event.c_str(), event.size());
                                last_pct = pct;
                            }

                            if (pdata.value("finished", false)) {
                                std::string done = "data: [DONE]\n\n";
                                sink.write(done.c_str(), done.size());
                                sink.done();
                                return true;
                            }

                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        }
                    }
                );
            }
        );

        fprintf(stderr, "[child] Installer routes enabled: /install/disks, /install/start, /install/progress\n");
    }

    // Serve install.js always (it self-detects live mode via /health)
    svr.Get("/install.js", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
        extern const unsigned char WEB_INSTALL_JS[];
        extern const unsigned int WEB_INSTALL_JS_LEN;
        serve_static_file(req, res, "install.js", WEB_INSTALL_JS, WEB_INSTALL_JS_LEN);
#else
        serve_static_file(req, res, "install.js", nullptr, 0);
#endif
    });

    // --- Notification SSE endpoint ---
    svr.Get("/llamaste/notifications", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        res.set_header("X-Accel-Buffering", "no");

        res.set_chunked_content_provider(
            "text/event-stream",
            [](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                auto last_hb = std::chrono::steady_clock::now();
                while (g_running) {
                    auto notifs = g_scheduler.drain_notifications();
                    for (const auto& n : notifs) {
                        json data;
                        data["type"] = n.type;
                        data["title"] = n.title;
                        data["body"] = n.body;
                        data["time"] = static_cast<long>(n.time);
                        data["id"] = n.id;
                        std::string line = "data: " + data.dump() + "\n\n";
                        if (!sink.write(line.c_str(), line.size())) return false;
                    }
                    // Send heartbeat every ~5s to keep connection alive
                    auto now = std::chrono::steady_clock::now();
                    if (now - last_hb >= std::chrono::seconds(5)) {
                        std::string heartbeat = ": heartbeat\n\n";
                        if (!sink.write(heartbeat.c_str(), heartbeat.size())) return false;
                        last_hb = now;
                    }
                    // Poll every 500ms for responsive notification delivery
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
                sink.done();
                return true;
            }
        );
    });

    // --- Schedule REST endpoints (protected) ---
    svr.Get("/llamaste/schedules", require_auth([](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(g_scheduler.list_tasks(), "application/json");
    }));

    svr.Post("/llamaste/schedules", require_auth([](const httplib::Request& req, httplib::Response& res) {
        auto body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.status = 400;
            json err;
            err["error"] = "Invalid JSON";
            res.set_content(err.dump(), "application/json");
            return;
        }
        res.set_content(g_scheduler.create_task(body), "application/json");
    }));

    svr.Delete(R"(/llamaste/schedules/(.+))", require_auth([](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        res.set_content(g_scheduler.delete_task(id), "application/json");
    }));

    // --- Cluster endpoints (protected) ---
    svr.Get("/llamaste/cluster/status", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(g_tools.dispatch("cluster.status", "{}"),
                        "application/json");
    }));

    svr.Get("/llamaste/cluster/capacity", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("cluster.capacity", "{}"),
                        "application/json");
    }));

    svr.Get("/llamaste/cluster/models", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("cluster.models", "{}"),
                        "application/json");
    }));

    svr.Get("/llamaste/cluster/peers", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("cluster.peers", "{}"),
                        "application/json");
    }));

    svr.Post("/llamaste/cluster/reload", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("cluster.reload", "{}"),
                        "application/json");
    }));

    // Test endpoint: manually add a peer (for integration testing without mDNS)
    svr.Post("/llamaste/cluster/add-peer", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        try {
            json args = json::parse(req.body);
            PeerInfo peer;
            peer.hostname = args.value("hostname", "unknown");
            peer.ip = args.value("ip", "");
            peer.rpc_port = args.value("rpc_port", 50052);
            peer.ram_mb = args.value("ram_mb", 0);
            peer.cpu_cores = args.value("cpu_cores", 0);
            peer.model = args.value("model", "none");

            if (peer.ip.empty()) {
                json err;
                err["error"] = "ip is required";
                res.set_content(err.dump(), "application/json");
                return;
            }

            g_cluster.add_peer(peer);

            // Run election in background to avoid blocking HTTP response
            // (topology callback may try to restart llama-server, taking 120s+)
            std::thread([](){ g_cluster.run_election(); }).detach();

            // Small delay to let election start
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            json result;
            result["added"] = peer.ip;
            result["role"] = g_cluster.role_name();
            result["peer_count"] = g_cluster.peer_count();
            res.set_content(result.dump(), "application/json");
        } catch (const std::exception& e) {
            json err;
            err["error"] = e.what();
            res.set_content(err.dump(), "application/json");
        }
    }));

    // --- Update endpoints ---
    svr.Get("/llamaste/update/status", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        std::string result = g_tools.dispatch("update.status", "{}");
        res.set_content(result, "application/json");
    }));

    svr.Post("/llamaste/update/check", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        std::string result = g_tools.dispatch("update.check", "{}");
        res.set_content(result, "application/json");
    }));

    svr.Post("/llamaste/update/rollback", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        std::string result = g_tools.dispatch("update.rollback", "{}");
        res.set_content(result, "application/json");
    }));

    // --- WiFi endpoints ---
    svr.Get("/llamaste/wifi/status", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("wifi.status", "{}"), "application/json");
    }));

    svr.Post("/llamaste/wifi/scan", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("wifi.scan", "{}"), "application/json");
    }));

    svr.Post("/llamaste/wifi/connect", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(g_tools.dispatch("wifi.connect", req.body),
                        "application/json");
    }));

    svr.Post("/llamaste/wifi/disconnect", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("wifi.disconnect", "{}"), "application/json");
    }));

    svr.Get("/llamaste/wifi/list", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        res.set_content(g_tools.dispatch("wifi.list", "{}"), "application/json");
    }));

    svr.Post("/llamaste/wifi/forget", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(g_tools.dispatch("wifi.forget", req.body), "application/json");
    }));

    svr.Post("/llamaste/wifi/enable", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(g_tools.dispatch("wifi.enable", req.body), "application/json");
    }));

    // --- MCP server (Model Context Protocol, spec 2025-03-26) ---
    // Exposes all Llamaste tools to Claude Desktop and other MCP clients.
    // Auth: Bearer token (preferred, headless) OR session cookie (web UI users).
    // Endpoint: POST /mcp (Streamable HTTP transport)
    // Claude Desktop config:
    //   { "mcpServers": { "llamaste": { "type": "http", "url": "http://llamaste.local/mcp",
    //       "headers": { "Authorization": "Bearer <api-key>" } } } }
    // API key shown in System panel → MCP Server card.
    g_mcp = new McpServer(g_tools);
    g_mcp->add_routes(svr, [](const httplib::Request& req) -> bool {
        return g_auth.is_authenticated(req);
    });

    // MCP key management (cookie-auth only — used by System panel)
    svr.Get("/llamaste/mcp/key", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
            if (!g_mcp) {
                res.status = 503;
                res.set_content(R"json({"error":"MCP not running"})json", "application/json");
                return;
            }
            res.set_content(g_mcp->get_api_key_info().dump(), "application/json");
        }
    ));

    svr.Post("/llamaste/mcp/key/regenerate", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
            if (!g_mcp) {
                res.status = 503;
                res.set_content(R"json({"error":"MCP not running"})json", "application/json");
                return;
            }
            res.set_content(g_mcp->regenerate_api_key().dump(), "application/json");
        }
    ));

    // --- Error handler ---
    // Only sets a default body for responses where the handler didn't set one.
    // This preserves custom error bodies from API endpoints (e.g. 503 from /tts).
    svr.set_error_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
        if (!res.body.empty()) return; // handler already set a body — preserve it
        json err;
        err["error"] = (res.status == 404) ? "Not found" : "Error";
        err["status"] = res.status;
        res.set_content(err.dump(), "application/json");
    });

    // --- Shutdown hook: stop server when signal received ---
    // Run a background thread that watches g_running and stops the server
    std::thread shutdown_watcher([&svr]() {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        fprintf(stderr, "[child] Stopping HTTP server...\n");
        svr.stop();
    });
    shutdown_watcher.detach();

    // --- Start server ---
    fprintf(stderr, "[child] HTTP server listening on 0.0.0.0:%d\n", port);
    fprintf(stderr, "[child] Web UI: http://localhost:%d/\n", port);
    fprintf(stderr, "[child] Health: http://localhost:%d/health\n", port);
    fprintf(stderr, "[child] MCP:    http://localhost:%d/mcp  (Claude Desktop)\n", port);
    fprintf(stderr, "[child] Running in %s mode\n",
            config.model_path.empty() ? "stub (no model)" : "inference");

    // Mark boot as successful after a short delay (post-update health check)
    // mark_boot_success() is a no-op if boot_counter is not set in grubenv
    std::thread boot_success_thread([]() {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        mark_boot_success();
        fprintf(stderr, "[update] Boot health check complete\n");
    });
    boot_success_thread.detach();

    // Push one-time startup notification — shown as a toast when the first
    // SSE client connects.  Queued here so it's waiting before any browser opens.
    {
        std::string local_ip = MdnsResponder::get_local_ip();
        std::string url = "http://" + local_ip +
                          (port == 80 ? "" : ":" + std::to_string(port)) + "/";
        Notification startup_notif;
        startup_notif.id    = "startup";
        startup_notif.type  = "info";
        startup_notif.title = "Llamaste Ready";
        startup_notif.body  = "Server running at " + url +
                              (config.model_path.empty()
                               ? " \u2014 no model loaded yet"
                               : " \u2014 AI ready");
        startup_notif.time  = time(nullptr);
        g_scheduler.push_notification(std::move(startup_notif));
    }

    bool ok = svr.listen("0.0.0.0", port);
    if (!ok && g_running) {
        fprintf(stderr, "[child] Failed to start HTTP server on port %d\n", port);
        mdns.stop();
        return 1;
    }

    // Clean shutdown
    g_scheduler.stop();
    mdns.stop();
    fprintf(stderr, "[child] HTTP server stopped\n");

    // Stop llama-rpc-server
#ifndef _WIN32
    {
        pid_t rpc_pid = g_rpc_pid.load();
        if (rpc_pid > 0) {
            fprintf(stderr, "[child] Stopping llama-rpc-server (PID %d)\n", rpc_pid);
            kill(rpc_pid, SIGTERM);
            for (int i = 0; i < 50; i++) {
                int status;
                if (waitpid(rpc_pid, &status, WNOHANG) != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            kill(rpc_pid, SIGKILL);
            g_rpc_pid.store(0);
        }
    }
#endif

    // Stop llama-server
#ifndef _WIN32
    {
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
    }
#endif

    return 0;
}
