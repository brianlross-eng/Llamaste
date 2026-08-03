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
#include "netlink_route.h"
#include "wifi.h"
#include "tools_model_download.h"
#include "json.hpp"

// httplib must be included in exactly one translation unit with implementation.
// OpenSSL support is controlled by the build system (CPPHTTPLIB_OPENSSL_SUPPORT
// defined in CMakeLists.txt when OpenSSL is found). When enabled, httplib
// handles HTTPS URLs and TLS connections.
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
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/route.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/sysmacros.h>
#endif

#include <spawn.h>

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

// Stderr tee thread — joinable so we can clean up on shutdown.
static std::thread g_tee_thread;
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
static std::atomic<double> g_tokens_per_sec{0.0};
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
    // Use topology-aware recommendation if available
    if (g_hwinfo.topology.recommended_threads > 0)
        return g_hwinfo.topology.recommended_threads;
    // Fallback: physical cores (estimate as half of logical for HT)
    int phys = g_hwinfo.physical_cores > 0 ? g_hwinfo.physical_cores : cpu_cores;
    return phys < 1 ? 1 : phys;
}

// Batch thread count: all P-core threads (HT helps compute-bound prompt processing)
int compute_batch_thread_count(int cpu_cores) {
    if (g_hwinfo.topology.recommended_batch_threads > 0)
        return g_hwinfo.topology.recommended_batch_threads;
    // Fallback: all logical cores
    return cpu_cores < 1 ? 1 : cpu_cores;
}

// Context window size based on free RAM after model loading.
// With tool calling enabled, the system prompt now includes compact parameter
// signatures for all 62 tools (~400-600 extra tokens vs the plain name list).
// Total system prompt is now ~1200-1400 tokens, leaving ~6800 for conversation
// at 8192 context. Qwen2.5 1.5B KV cache for 8192 ctx ≈ 75MB — very manageable.
// Use 8192 for machines with ≥2GB free RAM (1.5B model is ~950MB loaded).
int compute_context_size(int free_ram_mb) {
    // Guard against negative or near-zero free RAM (e.g. from
    // compositor reserve subtracting from a small total).  Returning
    // 2048 on a RAM-starved system is a guaranteed OOM.
    if (free_ram_mb < 256) return 512;
    if (free_ram_mb >= 2048) return 8192;
    if (free_ram_mb >= 1024) return 4096;
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
    // Development mode: read from filesystem.
    // Build a search list starting with paths relative to the executable,
    // then fall back to CWD-relative paths for legacy compatibility.
    std::vector<std::string> search_paths;

#ifndef _WIN32
    // Resolve the directory containing the running binary via /proc/self/exe.
    // This makes the lookup CWD-independent (fixes tests/build/ context).
    char exe_buf[4096] = {};
    ssize_t exe_len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (exe_len > 0) {
        std::string exe_dir(exe_buf, exe_len);
        auto slash = exe_dir.rfind('/');
        if (slash != std::string::npos) exe_dir.resize(slash + 1);
        // Typical dev layouts relative to the binary location:
        //   bin is at src/llamaste/build-host/llamaste → web at ../../web/
        //   bin is at tests/build/test_*              → web at ../../src/llamaste/web/
        search_paths.push_back(exe_dir + "../../src/llamaste/web/" + filename);
        search_paths.push_back(exe_dir + "../src/llamaste/web/"   + filename);
        search_paths.push_back(exe_dir + "web/"                   + filename);
    }
#endif

    // CWD-relative fallbacks (original behaviour)
    search_paths.push_back("src/llamaste/web/" + filename);
    search_paths.push_back("../src/llamaste/web/" + filename);
    search_paths.push_back("web/" + filename);

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

// parse_qwen_tool_calls is defined in agent.cpp / declared in agent.h.
// It parses <tool_call>...</tool_call> XML blocks from Qwen2.5 model output
// and returns an OpenAI-compatible tool_calls JSON array.

// Build a chatml-formatted prompt string from OpenAI-format messages.
// Used by llama_inference to bypass the broken chat template in llama-server.
// nlohmann json::value(key, default) THROWS type_error.302 when the key EXISTS
// but is null — it only substitutes the default for MISSING keys. Assistant
// tool-call messages carry "content": null (see msg_obj["content"] = nullptr),
// so a plain .value("content","") throws mid-conversation. jstr() is null-safe:
// missing/null -> default, string -> the string, object/other (e.g. a tool_call
// "arguments" object) -> its serialized JSON.
static std::string jstr(const json& o, const char* key, const char* dflt) {
    auto it = o.find(key);
    if (it == o.end() || it->is_null()) return dflt;
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

// True if `ip` belongs to ANY local interface. mDNS peer discovery must reject
// self-echo: a multi-homed box (eth + wifi) announces on every interface and
// then discovers its OWN announcement via a different interface's IP, which
// doesn't match a single self.ip — so it adds ITSELF as a phantom peer. That
// fake 2-node cluster makes it spawn llama-server with an RPC/tensor-split to a
// "peer" that is really itself, which wedges the engine (persistent HTTP 503).
static bool is_local_ip(const std::string& ip) {
    if (ip.empty()) return false;
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (struct ifaddrs* p = ifa; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        char buf[INET_ADDRSTRLEN];
        auto* sin = (struct sockaddr_in*)p->ifa_addr;
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) && ip == buf) {
            found = true; break;
        }
    }
    freeifaddrs(ifa);
    return found;
}

static std::string build_chatml_prompt(const json& messages) {
    std::string prompt;
    for (const auto& msg : messages) {
        const std::string role = jstr(msg, "role", "user");
        std::string content = jstr(msg, "content", "");

        // Assistant messages with tool_calls: serialize tool calls as Qwen2.5 native format
        if (role == "assistant" && content.empty() && msg.contains("tool_calls")
            && msg["tool_calls"].is_array()) {
            for (const auto& tc : msg["tool_calls"]) {
                if (tc.contains("function")) {
                    std::string name = jstr(tc["function"], "name", "");
                    std::string args = jstr(tc["function"], "arguments", "{}");
                    content += "<tool_call>\n{\"name\": \"" + name +
                               "\", \"arguments\": " + args + "}\n</tool_call>\n";
                }
            }
        }

        // Tool result messages → "tool" role (Qwen2.5 training format)
        if (role == "tool") {
            std::string name = jstr(msg, "name", "");
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

// ---------------------------------------------------------------------------
// build_tool_call_gbnf — GBNF grammar for Qwen2.5 tool call output
// ---------------------------------------------------------------------------
// Constrains output to one or more <tool_call> blocks with valid JSON and
// known tool names.  Used for retry when the first inference attempt produces
// malformed tool-call JSON (common on 3B models).
//
// llama-server's /completion endpoint accepts a "grammar" parameter — the
// sampler masks out tokens that would violate the grammar at each step,
// guaranteeing syntactically valid output with near-zero latency overhead.

static std::string build_tool_call_gbnf(const json& tools_array) {
    // Extract tool names from the OpenAI-format tools array
    std::vector<std::string> names;
    if (tools_array.is_array()) {
        for (const auto& tool : tools_array) {
            if (tool.contains("function") && tool["function"].contains("name")
                && tool["function"]["name"].is_string()) {
                names.push_back(tool["function"]["name"].get<std::string>());
            }
        }
    }
    if (names.empty()) return "";  // no tools → no grammar

    // --- Build GBNF grammar ---
    // Root: one or more tool call blocks (matches Qwen2.5 native format)
    std::string g;
    g += R"gbnf(root ::= toolcall (ws toolcall)*
toolcall ::= "<tool_call>\n" toolobj "\n</tool_call>" ws
toolobj ::= "{" ws "\"name\"" ws ":" ws toolname ws "," ws "\"arguments\"" ws ":" ws object ws "}"
)gbnf";

    // Enumerate valid tool names — model can ONLY call tools that exist
    g += "toolname ::= ";
    for (size_t i = 0; i < names.size(); i++) {
        if (i > 0) g += " | ";
        g += "\"\\\"" + names[i] + "\\\"\"";
    }
    g += "\n";

    // Standard JSON value grammar (RFC 8259 subset)
    g += R"gbnf(object ::= "{" ws "}" | "{" ws members ws "}"
members ::= pair ("," ws pair)*
pair ::= string ws ":" ws value
array ::= "[" ws "]" | "[" ws values ws "]"
values ::= value ("," ws value)*
value ::= string | number | object | array | "true" | "false" | "null"
string ::= "\"" chars "\""
chars ::= char*
char ::= [^"\\\x00-\x1f] | "\\" escape
escape ::= ["\\\/bfnrt] | "u" hex hex hex hex
hex ::= [0-9a-fA-F]
number ::= "-"? int frac? exp?
int ::= "0" | [1-9] [0-9]*
frac ::= "." [0-9]+
exp ::= [eE] [+-]? [0-9]+
ws ::= [ \t\n\r]*
)gbnf";

    return g;
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
    if (!comp_resp.is_discarded() && comp_resp.contains("content") && comp_resp["content"].is_string()) {
        content = comp_resp["content"].get<std::string>();
    } else if (!comp_resp.is_discarded()) {
        // Log what we actually got — helps debug 3B model null content issues
        std::string content_type = comp_resp.contains("content")
            ? comp_resp["content"].type_name() : "missing";
        fprintf(stderr, "[inference] WARNING: /completion content is %s (not string), "
                "full response keys:", content_type);
        for (auto it = comp_resp.begin(); it != comp_resp.end(); ++it)
            fprintf(stderr, " %s(%s)", it.key().c_str(), it.value().type_name());
        fprintf(stderr, "\n");
    }

    // --- Parse Qwen2.5 native <tool_call> tags from the generated text ---
    // The model emits tool calls in this XML format when it needs to call a tool.
    // Convert to OpenAI tool_calls array format so the agent loop can dispatch them.
    json tool_calls_arr = parse_qwen_tool_calls(content);

    // --- Grammar-constrained retry for malformed tool calls ---
    // If the model attempted a tool call (emitted <tool_call> tags) but the JSON
    // inside was malformed, retry with GBNF grammar that forces valid structure.
    // This primarily helps 3B models which struggle with complex JSON output.
    if (tool_calls_arr.empty() && content.find("<tool_call>") != std::string::npos) {
        fprintf(stderr, "[inference] Detected malformed tool call — retrying with GBNF grammar\n");

        json tools_arr = req_obj.value("tools", json::array());
        std::string grammar = build_tool_call_gbnf(tools_arr);

        if (!grammar.empty()) {
            // Build retry request — same prompt, lower temperature, grammar constraint
            json retry_req;
            retry_req["prompt"]      = prompt;
            retry_req["n_predict"]   = max_tokens;
            retry_req["temperature"] = std::max(0.1f, temperature - 0.2f);
            retry_req["stop"]        = json::array({"<|im_end|>", "<|endoftext|>", "<|im_start|>"});
            retry_req["stream"]      = false;
            retry_req["grammar"]     = grammar;
            std::string retry_str = retry_req.dump();

            fprintf(stderr, "[inference] Grammar retry: grammar_len=%zu temp=%.2f\n",
                    grammar.size(), std::max(0.1f, temperature - 0.2f));

            httplib::Client retry_cli("127.0.0.1", g_llama_port);
            retry_cli.set_connection_timeout(5);
            retry_cli.set_read_timeout(300);
            retry_cli.set_keep_alive(false);
            auto retry_result = retry_cli.Post("/completion", retry_str, "application/json");

            if (retry_result && retry_result->status == 200) {
                auto retry_resp = json::parse(retry_result->body, nullptr, false);
                if (!retry_resp.is_discarded() && retry_resp.contains("content")
                    && retry_resp["content"].is_string()) {
                    std::string retry_content = retry_resp["content"].get<std::string>();
                    json retry_calls = parse_qwen_tool_calls(retry_content);
                    if (!retry_calls.empty()) {
                        fprintf(stderr, "[inference] Grammar retry succeeded: %d tool call(s)\n",
                                (int)retry_calls.size());
                        content = retry_content;
                        tool_calls_arr = retry_calls;
                        // Update tokens/sec from retry timings
                        if (retry_resp.contains("timings")) {
                            auto& rt = retry_resp["timings"];
                            double pps = rt.value("predicted_per_second", 0.0);
                            if (pps > 0.0) g_tokens_per_sec.store(pps);
                        }
                    } else {
                        fprintf(stderr, "[inference] Grammar retry: still failed to parse tool calls\n");
                    }
                }
            } else {
                fprintf(stderr, "[inference] Grammar retry HTTP failed: status=%d\n",
                        retry_result ? retry_result->status : 0);
            }
        }
    }

    bool has_tool_calls = !tool_calls_arr.empty();

    json response;
    response["id"]     = "chatcmpl-" + std::to_string(std::time(nullptr));
    response["object"] = "chat.completion";
    response["model"]  = "llamaste";
    json usage;
    usage["prompt_tokens"]     = comp_resp.is_discarded() ? 0 : comp_resp.value("tokens_evaluated", 0);
    usage["completion_tokens"] = comp_resp.is_discarded() ? 0 : comp_resp.value("tokens_predicted", 0);
    usage["total_tokens"]      = usage["prompt_tokens"].get<int>() + usage["completion_tokens"].get<int>();
    response["usage"] = usage;

    // Extract tokens/sec from llama-server timings
    if (!comp_resp.is_discarded() && comp_resp.contains("timings")) {
        auto& t = comp_resp["timings"];
        double pps = t.value("predicted_per_second", 0.0);
        if (pps > 0.0) g_tokens_per_sec.store(pps);
    }

    json choice;
    choice["index"] = 0;
    if (has_tool_calls) {
        // Tool call response: assistant message with tool_calls array, content = null
        json msg_obj;
        msg_obj["role"]       = "assistant";
        msg_obj["content"]    = nullptr;
        msg_obj["tool_calls"] = tool_calls_arr;
        choice["message"]      = msg_obj;
        choice["finish_reason"] = "tool_calls";
        fprintf(stderr, "[inference] tool_calls: %d call(s) dispatched\n",
                (int)tool_calls_arr.size());
    } else {
        // Plain text response
        json msg_obj;
        msg_obj["role"]    = "assistant";
        msg_obj["content"] = content;
        choice["message"]      = msg_obj;
        choice["finish_reason"] = "stop";
        fprintf(stderr, "[inference] generated %d tokens: %.100s\n",
                usage["completion_tokens"].get<int>(), content.c_str());
    }
    response["choices"] = json::array({choice});

    std::string response_str = response.dump();

    // --- Semantic cache store (text-only responses; tool calls depend on system state) ---
    if (!has_tool_calls) {
        semantic_cache_store(request_json, response_str);
    }

    return response_str;
}

#ifndef _WIN32

// ---------------------------------------------------------------------------
// llama-server process lifecycle
// ---------------------------------------------------------------------------

// Thread-safe process spawn using posix_spawnp — avoids fork() deadlocks in
// multi-threaded HTTP server contexts.  fork() in a threaded process only
// duplicates the calling thread; other threads' mutexes remain locked, causing
// deadlocks if the child touches heap or locked resources before exec().
//
// Returns child PID on success, -1 on error.
// file_actions and attrp are optional; pass nullptr to use defaults.
static pid_t safe_spawn(const char* path, char* const argv[],
                        posix_spawn_file_actions_t* file_actions = nullptr,
                        posix_spawnattr_t* attrp = nullptr) {
    pid_t pid = -1;
    int ret = posix_spawnp(&pid, path, file_actions, attrp, argv, environ);
    if (ret != 0) {
        fprintf(stderr, "[child] safe_spawn('%s') failed: %s\n",
                path, strerror(ret));
        return -1;
    }
    return pid;
}

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

    // Build args vector for variable-length command
    // Build before spawn since args reference stack strings (port_str, c_str, etc.)
    std::vector<const char*> cargs;
    cargs.push_back("llama-server");
    cargs.push_back("-m"); cargs.push_back(model_path.c_str());
    cargs.push_back("--host"); cargs.push_back("127.0.0.1");
    cargs.push_back("--port"); cargs.push_back(port_str.c_str());
    cargs.push_back("--no-webui");
    cargs.push_back("-c"); cargs.push_back(c_str.c_str());
    cargs.push_back("-t"); cargs.push_back(t_str.c_str());
    cargs.push_back("-tb"); cargs.push_back(tb_str.c_str());
    // Flash attention: re-enabled — previous hang was likely a version-specific bug.
    // Flash attention reduces memory usage and improves speed. Safe on all hardware.
    cargs.push_back("-fa");

    // CPU pinning: on hybrid CPUs (Intel Alder Lake+), pin to P-cores only.
    // E-cores are 2-3x slower and drag barrier sync, causing massive slowdowns.
    // On homogeneous CPUs this is a no-op (p_core_range is empty).
    std::string cpu_range = g_hwinfo.topology.p_core_range;
    if (!cpu_range.empty()) {
        cargs.push_back("--cpu-range"); cargs.push_back(cpu_range.c_str());
        cargs.push_back("--cpu-strict"); cargs.push_back("1");
        fprintf(stderr, "[child] Hybrid CPU: pinning to P-cores: %s\n", cpu_range.c_str());
    }

    // GPU layer offloading: use Vulkan to push layers to GPU.
    // On unified memory (iGPU/APU/Strix Halo), offload all layers since
    // GPU and CPU share the same physical RAM.  On discrete GPUs, compute
    // how many layers fit in VRAM (~1.2GB/layer for 7B Q4_K_M).
    std::string ngl_str;
    if (g_hwinfo.gpu_detected && g_hwinfo.gpu_vram_mb > 0) {
        // Discrete GPU: layers = VRAM / 1.2GB per layer, minimum 1
        int ngl = std::max(1, (int)(g_hwinfo.gpu_vram_mb / 1200));
        ngl_str = std::to_string(ngl);
        cargs.push_back("-ngl"); cargs.push_back(ngl_str.c_str());
        fprintf(stderr, "[child] GPU offload: %d layers to %s (%llu MB VRAM)\n",
                ngl, g_hwinfo.gpu_name.c_str(),
                (unsigned long long)g_hwinfo.gpu_vram_mb);
    } else if (g_hwinfo.gpu_detected && g_hwinfo.gpu_is_unified) {
        // Unified memory (iGPU/APU): offload everything — GPU shares RAM
        ngl_str = "99";
        cargs.push_back("-ngl"); cargs.push_back(ngl_str.c_str());
        fprintf(stderr, "[child] GPU offload: all layers to %s (unified memory)\n",
                g_hwinfo.gpu_name.c_str());
    }

    // --log-disable REMOVED for debugging (logs go to /tmp/llama-server.log)
    // args.push_back("--log-disable");

    if (!rpc_endpoints.empty()) {
        cargs.push_back("--rpc");
        cargs.push_back(rpc_endpoints.c_str());
        fprintf(stderr, "[child] Using RPC endpoints: %s\n", rpc_endpoints.c_str());
    }

    if (!tensor_split.empty()) {
        cargs.push_back("--tensor-split");
        cargs.push_back(tensor_split.c_str());
        fprintf(stderr, "[child] Using tensor split: %s\n", tensor_split.c_str());
    }

    cargs.push_back(nullptr);

    // Build non-const argv for posix_spawnp. cargs is nullptr-terminated;
    // strdup(nullptr) calls strlen(nullptr) and segfaults, so preserve the
    // terminator instead of duplicating it.
    std::vector<char*> argv;
    for (const char* a : cargs) {
        argv.push_back(a ? strdup(a) : nullptr);
    }

    // Redirect stdout/stderr to a log file using posix_spawn_file_actions.
    // Without this, llama-server's verbose logs flood the serial port buffer
    // (/dev/console), causing all stderr writes to block — including the
    // supervisor's watchdog kick loop, which then misses kicks and triggers
    // a softdog reboot.
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO,
        "/tmp/llama-server.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    // Full path: llama-server installs to /opt/llamaste (not on PATH in server/
    // live mode), so posix_spawnp by bare name fails with ENOENT.
    pid_t pid = safe_spawn("/opt/llamaste/llama-server", argv.data(), &fa);
    posix_spawn_file_actions_destroy(&fa);

    // Free strdup'd argv
    for (char* a : argv) free(a);

    if (pid < 0) {
        return false;
    }

    // Parent: store PID
    g_llama_pid.store(pid);
    fprintf(stderr, "[child] llama-server spawned with PID %d\n", pid);
    return true;
}

static bool spawn_rpc_server() {
    std::string port_str = std::to_string(g_rpc_port);
    char* const argv[] = {
        const_cast<char*>("llama-rpc-server"),
        const_cast<char*>("--host"), const_cast<char*>("0.0.0.0"),
        const_cast<char*>("--port"), const_cast<char*>(port_str.c_str()),
        const_cast<char*>("-c"),  // Enable tensor caching for faster model reloads
        nullptr
    };

    // Full path: installed to /opt/llamaste, not on PATH (see spawn_llama_server).
    pid_t pid = safe_spawn("/opt/llamaste/llama-rpc-server", argv);
    if (pid < 0) {
        return false;
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
        // Write minimal wpa_supplicant.conf skeleton.
        // country=US: set regulatory domain so 5GHz channels are enabled at runtime.
        // cfg80211 fails to load regulatory.db from the filesystem before the
        // squashfs pivot completes, so wpa_supplicant setting country via nl80211
        // is the reliable path to get proper channel access.
        FILE* f = fopen(conf, "w");
        if (f) {
            fprintf(f, "ctrl_interface=/run/wpa_supplicant\n");
            fprintf(f, "ctrl_interface_group=0\n");
            fprintf(f, "update_config=1\n");
            fprintf(f, "country=US\n");
            fclose(f);
        }
    } else {
        // Config exists — patch in country=US if it's missing (configs from older
        // builds lack this line and stay in world regulatory domain).
        FILE* fr = fopen(conf, "r");
        if (fr) {
            bool has_country = false;
            char line[512];
            while (fgets(line, sizeof(line), fr)) {
                if (strncmp(line, "country=", 8) == 0) { has_country = true; break; }
            }
            fclose(fr);
            if (!has_country) {
                fr = fopen(conf, "r");
                FILE* fw = fopen("/tmp/wpa_patch.conf", "w");
                if (fr && fw) {
                    bool inserted = false;
                    while (fgets(line, sizeof(line), fr)) {
                        fputs(line, fw);
                        // Insert country= right after update_config= line
                        if (!inserted && strncmp(line, "update_config=", 14) == 0) {
                            fputs("country=US\n", fw);
                            inserted = true;
                        }
                    }
                    if (!inserted) fputs("country=US\n", fw);
                }
                if (fr) fclose(fr);
                if (fw) fclose(fw);
                rename("/tmp/wpa_patch.conf", conf);
                fprintf(stderr, "[child] Patched wpa.conf with country=US\n");
            }
        }
    }

    // Ensure control directory exists
    mkdir("/run/wpa_supplicant", 0755);

    // Pre-flight: verify binary exists and is executable
    if (access("/usr/sbin/wpa_supplicant", X_OK) != 0) {
        fprintf(stderr, "[child] ERROR: /usr/sbin/wpa_supplicant not found "
                "or not executable: %s\n", strerror(errno));
        return;
    }

    // Pre-flight: verify config file is readable and dump it for diagnostics
    {
        FILE* diag = fopen(conf, "r");
        if (!diag) {
            fprintf(stderr, "[child] ERROR: cannot read wpa.conf at %s: %s\n",
                    conf, strerror(errno));
            return;
        }
        fprintf(stderr, "[child] wpa.conf contents:\n");
        char dline[256];
        while (fgets(dline, sizeof(dline), diag)) {
            // Mask PSK values in output
            if (strncmp(dline, "    psk=", 8) == 0 || strncmp(dline, "\tpsk=", 5) == 0)
                fprintf(stderr, "  psk=********\n");
            else
                fprintf(stderr, "  %s", dline);
        }
        fclose(diag);
    }

    // Run wpa_supplicant in foreground (NO -B flag).
    // Use posix_spawn with POSIX_SPAWN_SETSID to avoid fork() in
    // multi-threaded context.  The spawn attributes create a new session
    // (replacing setsid()) and file actions redirect stdio.
    static const char* wpa_log = "/tmp/wpa_supplicant.log";

    char* const argv[] = {
        const_cast<char*>("wpa_supplicant"),
        const_cast<char*>("-dd"),          // max debug verbosity
        const_cast<char*>("-i"), const_cast<char*>(iface.c_str()),
        const_cast<char*>("-c"), const_cast<char*>(conf),
        const_cast<char*>("-D"), const_cast<char*>("nl80211,wext"),
        const_cast<char*>("-f"), const_cast<char*>(wpa_log),  // log to file
        nullptr
    };

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // stdin → /dev/null (avoid blocking on terminal input)
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    // stdout → wpa log file
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, wpa_log,
        O_WRONLY | O_CREAT | O_TRUNC, 0644);
    // stderr → same log file (capture dynamic linker errors)
    posix_spawn_file_actions_adddup2(&fa, STDOUT_FILENO, STDERR_FILENO);

    pid_t pid = safe_spawn("wpa_supplicant", argv, &fa, &attr);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);

    if (pid < 0) {
        return;
    }

    // Parent: poll for ctrl socket.  wpa_supplicant creates it during init
    // (before entering event loop), so it should appear within seconds.
    fprintf(stderr, "[child] wpa_supplicant forked as PID %d on %s (foreground)\n",
            pid, iface.c_str());
    std::string sock_path = "/run/wpa_supplicant/" + iface;
    bool daemon_ok = false;
    for (int i = 0; i < 10; i++) {  // 5 seconds max
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        struct stat sst;
        if (stat(sock_path.c_str(), &sst) == 0) { daemon_ok = true; break; }
        // Check if child is still alive
        int wstatus;
        pid_t w = waitpid(pid, &wstatus, WNOHANG);
        if (w == pid) {
            // Child exited or was killed — something went wrong
            if (WIFEXITED(wstatus))
                fprintf(stderr, "[child] wpa_supplicant exited with code %d "
                        "(check errors above)\n", WEXITSTATUS(wstatus));
            else if (WIFSIGNALED(wstatus))
                fprintf(stderr, "[child] wpa_supplicant killed by signal %d\n",
                        WTERMSIG(wstatus));
            break;
        }
    }
    if (daemon_ok) {
        fprintf(stderr, "[child] wpa_supplicant running (PID %d) on %s\n",
                pid, iface.c_str());
        // Clear stale BSSID hints from persisted wpa.conf.
        // On installed systems, wpa.conf survives reboots and may contain
        // BSSIDs from a previous boot where the AP was on a different channel.
        // Stale BSSIDs cause 4-way handshake timeouts (wpa_supplicant targets
        // a specific BSSID instead of scanning for the best one).
        g_wifi.clear_all_bssids();
    } else {
        fprintf(stderr, "[child] ERROR: wpa_supplicant ctrl socket not found "
                "at %s after 5s\n", sock_path.c_str());
        // Dump the wpa_supplicant log file to console for diagnosis.
        // This captures output even when CONFIG_NO_STDOUT_DEBUG routes
        // wpa_printf to syslog (which we don't have).
        FILE* lf = fopen(wpa_log, "r");
        if (lf) {
            fprintf(stderr, "[child] === wpa_supplicant log (%s) ===\n", wpa_log);
            char lbuf[512];
            int lines = 0;
            while (fgets(lbuf, sizeof(lbuf), lf) && lines < 80) {
                fprintf(stderr, "  %s", lbuf);
                lines++;
            }
            if (lines == 0) fprintf(stderr, "  (empty log file)\n");
            if (lines >= 80) fprintf(stderr, "  ... (truncated at 80 lines)\n");
            fprintf(stderr, "[child] === end wpa_supplicant log ===\n");
            fclose(lf);
        } else {
            fprintf(stderr, "[child] No log file at %s\n", wpa_log);
        }
        // List what IS in /run/wpa_supplicant/ for diagnosis
        DIR* d = opendir("/run/wpa_supplicant");
        if (d) {
            struct dirent* ent;
            fprintf(stderr, "[child] /run/wpa_supplicant/ contents:");
            bool any = false;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                fprintf(stderr, " %s", ent->d_name);
                any = true;
            }
            if (!any) fprintf(stderr, " (empty)");
            fprintf(stderr, "\n");
            closedir(d);
        } else {
            fprintf(stderr, "[child] /run/wpa_supplicant/ does not exist!\n");
        }
    }
}

// Manually add default gateway route and DNS after DHCP lease.
// dhcpcd hooks need /bin/sh which doesn't exist (BR2_SYSTEM_BIN_SH_NONE=y),
// so dhcpcd gets the IP but silently fails to add the route or write DNS.
static void apply_dhcp_route_and_dns(const std::string& iface) {
#ifndef _WIN32
    // Read gateway from dhcpcd lease file
    // dhcpcd writes lease info to /var/lib/dhcpcd/<iface>.lease (binary)
    // Easier approach: read the gateway from the routing table or /proc
    // Actually, the kernel ip=dhcp should handle this. But as a fallback,
    // try to get the gateway from the interface's network and add a route.

    int sk = socket(AF_INET, SOCK_DGRAM, 0);
    if (sk < 0) return;

    // Get our IP and netmask to derive likely gateway
    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);

    struct in_addr our_ip = {}, our_mask = {};
    if (ioctl(sk, SIOCGIFADDR, &ifr) == 0)
        our_ip = ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr;
    if (ioctl(sk, SIOCGIFNETMASK, &ifr) == 0)
        our_mask = ((struct sockaddr_in*)&ifr.ifr_netmask)->sin_addr;

    if (our_ip.s_addr != 0 && our_mask.s_addr != 0) {
        // Check if default route already exists
        FILE* rf = fopen("/proc/net/route", "r");
        bool has_default = false;
        if (rf) {
            char line[256];
            while (fgets(line, sizeof(line), rf)) {
                char riface[32];
                unsigned long dest;
                if (sscanf(line, "%31s %lx", riface, &dest) == 2) {
                    if (dest == 0 && strcmp(riface, "lo") != 0) {
                        has_default = true;
                        break;
                    }
                }
            }
            fclose(rf);
        }

        if (!has_default) {
            // Read actual gateway from dhcpcd lease info (not guess .1)
            std::string gw_str;

            // Try dhcpcd's info file first (plain text key=value)
            {
                std::string info_path = "/var/lib/dhcpcd/" + iface + ".info";
                FILE* info = fopen(info_path.c_str(), "r");
                if (info) {
                    char line[256];
                    while (fgets(line, sizeof(line), info)) {
                        if (strncmp(line, "routers=", 8) == 0) {
                            gw_str = std::string(line + 8);
                            // Trim newline and take first router only
                            gw_str.erase(gw_str.find_last_not_of("\r\n") + 1);
                            auto sp = gw_str.find(' ');
                            if (sp != std::string::npos) gw_str = gw_str.substr(0, sp);
                            break;
                        }
                    }
                    fclose(info);
                }
            }

            // Fallback: read from /proc/net/route (kernel routing table)
            if (gw_str.empty()) {
                FILE* rt2 = fopen("/proc/net/route", "r");
                if (rt2) {
                    char rtline[256], riface[32];
                    unsigned long dest2, gw_hex;
                    while (fgets(rtline, sizeof(rtline), rt2)) {
                        if (sscanf(rtline, "%31s %lx %lx", riface, &dest2, &gw_hex) == 3
                            && dest2 == 0 && gw_hex != 0
                            && strcmp(riface, iface.c_str()) == 0) {
                            struct in_addr gw_addr;
                            gw_addr.s_addr = (uint32_t)gw_hex;
                            char buf[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &gw_addr, buf, sizeof(buf));
                            gw_str = buf;
                            break;
                        }
                    }
                    fclose(rt2);
                }
            }

            // Last resort: guess .1 on subnet
            if (gw_str.empty()) {
                struct in_addr gw;
                gw.s_addr = (our_ip.s_addr & our_mask.s_addr) | htonl(1);
                char buf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &gw, buf, sizeof(buf));
                gw_str = buf;
                fprintf(stderr, "[net] %s: no gateway in lease/route, guessing %s\n",
                        iface.c_str(), gw_str.c_str());
            }

            // Add default route via netlink (replaces fork/exec ip route)
            netlink_dump_routes("pre-dhcp-gw");
            netlink_del_default_route();
            int nlret = netlink_add_default_route(gw_str.c_str(), iface.c_str());
            if (nlret != 0) {
                fprintf(stderr, "[net] netlink gateway failed on %s\n", iface.c_str());
            }
            netlink_dump_routes("post-dhcp-gw");
        }
    }
    close(sk);

    // Write DNS if resolv.conf is missing or empty
    struct stat rst;
    bool need_dns = (stat("/etc/resolv.conf", &rst) != 0 || rst.st_size < 10);
    if (need_dns) {
        FILE* dns = fopen("/etc/resolv.conf", "w");
        if (dns) {
            fprintf(dns, "nameserver 8.8.8.8\nnameserver 8.8.4.4\n");
            fclose(dns);
            fprintf(stderr, "[net] %s: wrote fallback DNS to /etc/resolv.conf\n", iface.c_str());
        }
    }
#endif
}

static void spawn_dhcpcd(const std::string& iface) {
    // Buildroot installs dhcpcd to /sbin/dhcpcd (not /usr/sbin/)
    std::vector<const char*> cargs = {"dhcpcd", "-b", iface.c_str(), nullptr};

    std::vector<char*> argv;
    for (const char* a : cargs) {
        argv.push_back(a ? strdup(a) : nullptr);   // was: strdup(a) — strdup(NULL) segfaults on the terminator
    }

    pid_t pid = safe_spawn("dhcpcd", argv.data());
    // Free strdup'd strings (safe after posix_spawn — kernel copies args)
    for (char* a : argv) free(a);
    if (pid < 0) {
        return;
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

    // Network interfaces — enumerate all UP interfaces with their IPs
#ifndef _WIN32
    {
        json nets = json::array();
        DIR* nd = opendir("/sys/class/net");
        if (nd) {
            struct dirent* ne;
            while ((ne = readdir(nd))) {
                if (ne->d_name[0] == '.') continue;
                std::string ifname = ne->d_name;
                if (ifname == "lo") continue;

                // Read operstate
                std::string state = "unknown";
                std::ifstream sf("/sys/class/net/" + ifname + "/operstate");
                if (sf.is_open()) std::getline(sf, state);
                if (state != "up" && state != "unknown") continue; // skip down interfaces

                // Determine type: WiFi vs wired vs other
                std::string iftype = "wired";
                if (access(("/sys/class/net/" + ifname + "/phy80211").c_str(), F_OK) == 0)
                    iftype = "wifi";
                // Check sysfs type for tunnel filtering
                std::ifstream tf("/sys/class/net/" + ifname + "/type");
                if (tf.is_open()) {
                    int t = 0;
                    tf >> t;
                    if (t != 1) continue; // skip non-ethernet (tunnels, etc.)
                }

                // Get IP address via ioctl
                std::string ip_str = "--";
                int sock = socket(AF_INET, SOCK_DGRAM, 0);
                if (sock >= 0) {
                    struct ifreq ifr = {};
                    strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
                    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                        auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
                        char ipbuf[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &sa->sin_addr, ipbuf, sizeof(ipbuf));
                        ip_str = ipbuf;
                    }
                    close(sock);
                }

                json iface;
                iface["name"] = ifname;
                iface["type"] = iftype;
                iface["ip"] = ip_str;
                iface["state"] = state;
                nets.push_back(iface);
            }
            closedir(nd);
        }
        info["networks"] = nets;
    }
#endif

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
    info["cpu_physical_cores"] = g_hwinfo.physical_cores;
    info["cpu_features"] = g_hwinfo.topology.cpu_features;
    info["cpu_hybrid"] = g_hwinfo.topology.is_hybrid;
    if (g_hwinfo.topology.is_hybrid) {
        info["cpu_p_cores"] = g_hwinfo.topology.p_cores;
        info["cpu_e_cores"] = g_hwinfo.topology.e_cores;
        info["cpu_p_threads"] = g_hwinfo.topology.p_threads;
        info["cpu_pinned_range"] = g_hwinfo.topology.p_core_range;
    }
    info["inference_threads"] = g_hwinfo.topology.recommended_threads;
    info["inference_batch_threads"] = g_hwinfo.topology.recommended_batch_threads;
    info["gpu_name"] = g_hwinfo.gpu_detected ? g_hwinfo.gpu_name : "";
    info["gpu_detected"] = g_hwinfo.gpu_detected;
    info["has_avx2"] = g_hwinfo.has_avx2;
    info["has_avx512"] = g_hwinfo.has_avx512;

    // Performance — updated after each inference call
    info["tokens_per_sec"] = g_tokens_per_sec.load();
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
    // Bootstrap the SMALLEST tier (0.5B) rather than the largest that fits.
    // recommend_model(usable_ram_mb) returns 32B on a 128GB box, which never fits
    // the live-ISO tmpfs /data; users upgrade to a bigger model via the model UI.
    const ModelInfo* table = get_model_table();
    int n = 0; while (table[n].name) n++;
    const ModelInfo* mi = (n > 0) ? &table[n - 1] : nullptr;
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
        try {
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
        // Enforce strict TLS verification — curl may silently fall back
        // to unverified if no CA bundle is configured.
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) {
            curl_easy_setopt(curl, CURLOPT_CAINFO,
                             "/etc/ssl/certs/ca-certificates.crt");
        } else {
            fprintf(stderr, "[cluster] WARNING: CA bundle not found at "
                    "/etc/ssl/certs/ca-certificates.crt — "
                    "HTTPS downloads will fail (enforcing strict TLS)\n");
        }
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
        } catch (const std::exception& e) {
            fprintf(stderr, "[child] upgrade_download thread exception: %s\n", e.what());
            g_upgrade_downloading.store(false);
        } catch (...) {
            fprintf(stderr, "[child] upgrade_download thread unknown exception\n");
            g_upgrade_downloading.store(false);
        }
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
            // Save original stderr before the dup2 so error fprintf in
            // the tee thread doesn't feed back into the pipe.
            int orig_stderr = dup(STDERR_FILENO);

            int pipefd[2];
            if (pipe2(pipefd, O_CLOEXEC) == 0) {
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                int read_end = pipefd[0];
                int orig_console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
                g_tee_thread = std::thread([read_end, log_fd, orig_console, orig_stderr]() {
                    try {
                    char buf[512];
                    while (true) {
                        ssize_t n = read(read_end, buf, sizeof(buf));
                        if (n <= 0) break;
                        ssize_t written = write(log_fd, buf, n);
                        if (written < 0) {
                            dprintf(orig_stderr, "[child-tee] log write: %s\n",
                                    strerror(errno));
                        }
                        if (orig_console >= 0) {
                            ssize_t cw = write(orig_console, buf, n);
                            if (cw < 0) {
                                dprintf(orig_stderr, "[child-tee] console write: %s\n",
                                        strerror(errno));
                            }
                        }
                    }
                    close(read_end);
                    close(log_fd);
                    if (orig_console >= 0) close(orig_console);
                    if (orig_stderr >= 0) close(orig_stderr);
                    } catch (const std::exception& e) {
                        dprintf(orig_stderr, "[child] tee thread exception: %s\n", e.what());
                    } catch (...) {
                        dprintf(orig_stderr, "[child] tee thread unknown exception\n");
                    }
                });
            } else {
                close(log_fd);
                if (orig_stderr >= 0) close(orig_stderr);
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
    // Enable installer when booted from USB/ISO (any mode: live, desktop, server).
    // Detection: after squashfs pivot, the original ISO root is bind-mounted at
    // /cdrom.  Check for the /cdrom/llamaste-live-iso marker file.
    // (Previous approach of /run/llamaste-live failed because init_mount_filesystems()
    // mounts a fresh tmpfs on /run after re-exec, destroying the marker.)
    bool is_live_iso = (access("/cdrom/llamaste-live-iso", F_OK) == 0);
    fprintf(stderr, "[child] Live ISO check: /cdrom/llamaste-live-iso %s\n",
            is_live_iso ? "EXISTS — installer enabled" : "NOT FOUND");
    if (is_live_iso) {
        register_install_tools(g_tools);
        fprintf(stderr, "[child] Installer routes: /install/disks, /install/start, /install/progress\n");
    }
    register_schedule_tools(g_tools, g_scheduler);
    register_auth_tools(g_tools, g_auth);
    register_cluster_tools(g_tools, g_cluster);
    register_update_tools(g_tools);
    register_wifi_tools(g_tools, g_wifi);
#ifdef LLAMASTE_TEST_API
    register_debug_tools(g_tools);
    fprintf(stderr, "[child] DEBUG tools registered (test build)\n");
#endif
    fprintf(stderr, "[child] Registered %d tools\n", g_tools.count());

    // Detect hardware
    g_hwinfo = detect_hardware();
    fprintf(stderr, "[child] CPU: %s (%d logical, %d physical), RAM: %d MB, Features: %s\n",
            g_hwinfo.cpu_model.c_str(), g_hwinfo.cpu_cores, g_hwinfo.physical_cores,
            g_hwinfo.ram_total_mb, g_hwinfo.topology.cpu_features.c_str());
    if (g_hwinfo.topology.is_hybrid) {
        fprintf(stderr, "[child] Hybrid CPU: %d P-cores (%d threads), %d E-cores | pinning to: %s\n",
                g_hwinfo.topology.p_cores, g_hwinfo.topology.p_threads,
                g_hwinfo.topology.e_cores, g_hwinfo.topology.p_core_range.c_str());
    }

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
        // Reserve for compositor (cage + cog together use ~100 MB; 128 MB provides headroom)
        if (config.boot_mode == "desktop") free_ram_estimate -= 128;

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
            std::thread monitor([](std::string mp, int cc, int fr) {
                try {
                    llama_monitor_thread(mp, cc, fr);
                } catch (const std::exception& e) {
                    fprintf(stderr, "[child] llama_monitor_thread exception: %s\n", e.what());
                } catch (...) {
                    fprintf(stderr, "[child] llama_monitor_thread unknown exception\n");
                }
            }, config.model_path, config.cpu_cores, free_ram_estimate);
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
            if (d.ip == self.ip || is_local_ip(d.ip)) continue;  // skip self (any local IP)
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
                        std::thread mon([](std::string mp, int cc, int fr) {
                                        try {
                                            llama_monitor_thread(mp, cc, fr);
                                        } catch (const std::exception& e) {
                                            fprintf(stderr, "[child] llama_monitor_thread exception: %s\n", e.what());
                                        } catch (...) {
                                            fprintf(stderr, "[child] llama_monitor_thread unknown exception\n");
                                        }
                                    }, model_path, g_hwinfo.cpu_cores, free_ram_estimate);
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
                                std::thread mon([](std::string mp, int cc, int fr) {
                                                try {
                                                    llama_monitor_thread(mp, cc, fr);
                                                } catch (const std::exception& e) {
                                                    fprintf(stderr, "[child] llama_monitor_thread exception: %s\n", e.what());
                                                } catch (...) {
                                                    fprintf(stderr, "[child] llama_monitor_thread unknown exception\n");
                                                }
                                            }, model_path, g_hwinfo.cpu_cores, free_ram_estimate);
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
        try {
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
                if (d.ip == self.ip || is_local_ip(d.ip)) continue;  // skip self (any local IP)
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
                            std::thread mon([](std::string mp, int cc, int fr) {
                                try {
                                    llama_monitor_thread(mp, cc, fr);
                                } catch (const std::exception& e) {
                                    fprintf(stderr, "[child] llama_monitor_thread exception: %s\n", e.what());
                                } catch (...) {
                                    fprintf(stderr, "[child] llama_monitor_thread unknown exception\n");
                                }
                            }, path, g_hwinfo.cpu_cores, free_ram);
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
        } catch (const std::exception& e) {
            fprintf(stderr, "[child] cluster_heartbeat thread exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[child] cluster_heartbeat thread unknown exception\n");
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

        // 5b. Regulatory database accessible?
        fprintf(stderr, "[wifi-diag] regulatory.db: %s\n",
                access("/lib/firmware/regulatory.db", F_OK) == 0 ? "found" : "MISSING");

        // 6. Kernel version (confirms which kernel is running)
        {
            FILE* kv = fopen("/proc/version", "r");
            if (kv) { char buf[256]; if (fgets(buf, sizeof(buf), kv)) fprintf(stderr, "[wifi-diag] kernel: %s", buf); fclose(kv); }
        }
    }
#endif

    // --- Wired ethernet auto-DHCP (PRIMARY) ---
    // User requirement: ethernet is the primary network interface.
    // WiFi is fallback — only used if wired ethernet fails to obtain an IP.
    // Scan for non-WiFi, non-loopback interfaces with carrier (cable plugged in)
    // and run dhcpcd on them.
    bool ethernet_got_ip = false;
#ifndef _WIN32

    // Scan for non-WiFi, non-loopback interfaces with carrier (cable plugged in)
    // and run dhcpcd on them.  This enables connectivity over ethernet dongles,
    // built-in NICs, etc. without manual configuration.
    {
        fprintf(stderr, "[net] Scanning for wired ethernet interfaces...\n");
        DIR* nd = opendir("/sys/class/net");
        if (nd) {
            struct dirent* ne;
            while ((ne = readdir(nd))) {
                if (ne->d_name[0] == '.') continue;
                const std::string ifname = ne->d_name;
                // Skip loopback, WiFi, and tunnel/virtual interfaces
                if (ifname == "lo") continue;
                // Skip known tunnel/virtual interfaces (sit0 = IPv6-in-IPv4,
                // tunl0 = IPIP, ip6tnl0 = IPv6 tunnel, gre0 = GRE)
                if (ifname == "sit0" || ifname == "tunl0" ||
                    ifname == "ip6tnl0" || ifname == "gre0" ||
                    ifname == "ip_vti0" || ifname == "ip6_vti0") {
                    continue;
                }
                // Skip any interface with type != 1 (ARPHRD_ETHER)
                // This filters out tunnels (776/SIT, 768/IPIP), loopback (772), etc.
                char tpath[256];
                snprintf(tpath, sizeof(tpath), "/sys/class/net/%s/type", ifname.c_str());
                FILE* tf = fopen(tpath, "r");
                if (tf) {
                    int iftype = 0;
                    fscanf(tf, "%d", &iftype);
                    fclose(tf);
                    if (iftype != 1) { // 1 = ARPHRD_ETHER (real ethernet)
                        fprintf(stderr, "[net] %s: not ethernet (type=%d), skipping\n",
                                ifname.c_str(), iftype);
                        continue;
                    }
                }
                char wpath[256];
                snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/phy80211", ifname.c_str());
                if (access(wpath, F_OK) == 0) {
                    fprintf(stderr, "[net] %s: WiFi interface, skipping\n", ifname.c_str());
                    continue;
                }

                fprintf(stderr, "[net] %s: found wired interface\n", ifname.c_str());

                // Bring interface UP first (carrier can only be read when UP)
                // Can't use system() — no /bin/sh. Use ioctl directly.
                int sock = socket(AF_INET, SOCK_DGRAM, 0);
                if (sock < 0) {
                    fprintf(stderr, "[net] %s: socket() failed: %m\n", ifname.c_str());
                    continue;
                }
                struct ifreq ifr = {};
                strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
                if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
                    if (!(ifr.ifr_flags & IFF_UP)) {
                        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
                        if (ioctl(sock, SIOCSIFFLAGS, &ifr) == 0) {
                            fprintf(stderr, "[net] %s: brought UP\n", ifname.c_str());
                        } else {
                            fprintf(stderr, "[net] %s: SIOCSIFFLAGS UP failed: %m\n", ifname.c_str());
                        }
                    }
                }
                close(sock);

                // Wait for carrier detection after UP.
                // RTL8125B (EVO-X2) link negotiation takes 3-5s after IFF_UP.
                // USB ethernet (RTL8153B) needs ~1s. Retry up to 10s.
                char cpath[256];
                snprintf(cpath, sizeof(cpath), "/sys/class/net/%s/carrier", ifname.c_str());
                int carrier = 0;
                for (int attempt = 0; attempt < 10; attempt++) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                    FILE* cf = fopen(cpath, "r");
                    if (!cf) break;
                    carrier = 0;
                    fscanf(cf, "%d", &carrier);
                    fclose(cf);
                    if (carrier == 1) {
                        fprintf(stderr, "[net] %s: carrier detected after %ds\n",
                                ifname.c_str(), attempt + 1);
                        break;
                    }
                    if (attempt == 0)
                        fprintf(stderr, "[net] %s: waiting for carrier...\n", ifname.c_str());
                }
                if (carrier != 1) {
                    fprintf(stderr, "[net] %s: no carrier after 10s (cable not plugged in?)\n", ifname.c_str());
                    continue;
                }

                fprintf(stderr, "[net] %s: carrier detected\n", ifname.c_str());

                // Skip dhcpcd if kernel ip=dhcp already configured this interface
                bool already_has_ip = false;
                {
                    int sk = socket(AF_INET, SOCK_DGRAM, 0);
                    if (sk >= 0) {
                        struct ifreq ifr2 = {};
                        strncpy(ifr2.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
                        if (ioctl(sk, SIOCGIFADDR, &ifr2) == 0) {
                            auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr2.ifr_addr);
                            char ip[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                            if (strcmp(ip, "0.0.0.0") != 0) {
                                already_has_ip = true;
                                fprintf(stderr, "[net] %s: already configured by kernel (%s), skipping dhcpcd\n",
                                        ifname.c_str(), ip);
                            }
                        }
                        close(sk);
                    }
                }

                if (already_has_ip) {
                    // Kernel set the IP via ip=dhcp, but the default route
                    // may be missing. Dump routes and ensure gateway exists.
                    netlink_dump_routes("kernel-dhcp-check");
                    apply_dhcp_route_and_dns(ifname);
                    continue;
                }

                fprintf(stderr, "[net] %s: no IP yet, spawning dhcpcd\n", ifname.c_str());
                spawn_dhcpcd(ifname);

                // Wait up to 10s for DHCP lease, then retry once with
                // a fresh dhcpcd spawn (handles cold DHCP servers like
                // Windows ICS that are slow on first-seen MAC addresses)
                bool got_lease = false;
                for (int attempt = 0; attempt < 2 && !got_lease; attempt++) {
                    int wait_iters = (attempt == 0) ? 20 : 30;  // 10s first, 15s retry
                    for (int w = 0; w < wait_iters; w++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        int sk = socket(AF_INET, SOCK_DGRAM, 0);
                        if (sk >= 0) {
                            struct ifreq ifr2 = {};
                            strncpy(ifr2.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
                            if (ioctl(sk, SIOCGIFADDR, &ifr2) == 0) {
                                auto* sa = reinterpret_cast<struct sockaddr_in*>(&ifr2.ifr_addr);
                                char ip[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                                if (strcmp(ip, "0.0.0.0") != 0) {
                                    fprintf(stderr, "[net] %s: DHCP lease obtained: %s (attempt %d, %dms)\n",
                                            ifname.c_str(), ip, attempt + 1, (w + 1) * 500);
                                    close(sk);
                                    ethernet_got_ip = true;
                                    got_lease = true;
                                    break;
                                }
                            }
                            close(sk);
                        }
                    }
                    if (!got_lease && attempt == 0) {
                        fprintf(stderr, "[net] %s: no DHCP lease after 10s, retrying...\n", ifname.c_str());
                        // Kill stale dhcpcd and respawn fresh
                        // NOTE: system() doesn't work — no /bin/sh (BR2_SYSTEM_BIN_SH_NONE)
                        // Use fork/exec directly
                        // NOTE: fork() called from multi-threaded context — pre-exec code must be async-signal-safe
                        pid_t kpid = fork();
                        if (kpid == 0) {
                            execl("/sbin/dhcpcd", "dhcpcd", "-k", ifname.c_str(), nullptr);
                            _exit(1);
                        } else if (kpid > 0) {
                            int kst = 0;
                            waitpid(kpid, &kst, 0);
                        }
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        spawn_dhcpcd(ifname);
                    }
                }
                if (got_lease) {
                    // dhcpcd hooks need /bin/sh which we don't have.
                    // Manually add default route and DNS.
                    apply_dhcp_route_and_dns(ifname);
                } else {
                    fprintf(stderr, "[net] %s: no DHCP lease after retries (dhcpcd -b will keep trying in background)\n", ifname.c_str());
                }
            }
            closedir(nd);
        } else {
            fprintf(stderr, "[net] cannot open /sys/class/net: %m\n");
        }
    }
#endif

    // --- WiFi fallback ---
    // Only attempt WiFi if wired ethernet did NOT obtain an IP.
    // Ethernet is primary, WiFi is secondary per user requirement.
    if (!ethernet_got_ip) {
    // Initialize WiFi and start wpa_supplicant + dhcpcd if hardware found.
    // In desktop mode, supervisor_preflight_wifi() skips entirely (can't use
    // tty1 — compositor owns the display), so modules may still be probing
    // when we get here.  Retry init() for up to 10s to catch late interfaces.
#ifndef _WIN32
    {
        // Reload regulatory database — cfg80211 (built-in) tried to load
        // regulatory.db during kernel init before squashfs pivot and cached
        // the failure.  Force a retry now that /lib/firmware/ is available.
// NOTE: fork() called from multi-threaded context — pre-exec code must be async-signal-safe
        {
            pid_t rpid = fork();
            if (rpid == 0) { execl("/usr/sbin/iw", "iw", "reg", "reload", (char*)nullptr); _exit(127); }
            if (rpid > 0) waitpid(rpid, nullptr, 0);
            usleep(300000);

            pid_t spid = fork();
            if (spid == 0) { execl("/usr/sbin/iw", "iw", "reg", "set", "US", (char*)nullptr); _exit(127); }
            if (spid > 0) waitpid(spid, nullptr, 0);
            fprintf(stderr, "[wifi] regulatory: reload + set US\n");
        }

        bool wifi_init_ok = g_wifi.init();
        if (!wifi_init_ok || !g_wifi.has_wifi()) {
            // Modules may still be probing — retry (especially in desktop mode
            // where supervisor_preflight_wifi doesn't run its own retry loop).
            // Some drivers (RTL8821CE) need 3-4s after finit_module().
            for (int try_n = 0; try_n < 10 && !g_wifi.has_wifi(); try_n++) {
                if (try_n == 0)
                    fprintf(stderr, "[wifi] no interface yet, waiting for module probe...\n");
                std::this_thread::sleep_for(std::chrono::seconds(1));
                wifi_init_ok = g_wifi.init();
            }
        }
        std::string wifi_iface;
        if (wifi_init_ok && g_wifi.has_wifi())
            wifi_iface = g_wifi.status().iface;
        fprintf(stderr, "[wifi] init=%s has_wifi=%s iface='%s' mode='%s'\n",
                wifi_init_ok ? "ok" : "fail",
                g_wifi.has_wifi() ? "yes" : "no",
                wifi_iface.c_str(),
                g_boot_mode.c_str());
    // Register spawn callback so WiFiManager::connect() can start
    // wpa_supplicant on-demand (desktop mode: user clicks Connect
    // before daemon was running, or daemon silently crashed).
    g_wifi.set_spawn_callback([](const std::string& iface) {
        spawn_wpa_supplicant(iface);
    });

    if (wifi_init_ok) {
        if (!wifi_iface.empty()) {
            spawn_wpa_supplicant(wifi_iface);
            // spawn_wpa_supplicant now includes ctrl socket verification
            // (up to 3s polling), so no extra sleep needed here.
            // Re-open control socket now that daemon is running
            g_wifi.init();

            spawn_dhcpcd(wifi_iface);

            // --- Post-connection verification ---
            // Wait for WPA authentication + DHCP lease.  Log state for
            // diagnostics so we can tell if connectivity failures are at
            // the auth, DHCP, or DNS layer.
            {
                fprintf(stderr, "[wifi] waiting for connection...\n");
                std::string final_state = "UNKNOWN";
                std::string final_ssid;
                std::string final_ip;

                // Poll wpa_supplicant state for up to 20s
                for (int w = 0; w < 40; w++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    auto st = g_wifi.status();
                    final_state = st.state;
                    final_ssid  = st.ssid;
                    final_ip    = st.ip_addr;
                    if (st.state == "COMPLETED" && !st.ip_addr.empty()) {
                        fprintf(stderr, "[wifi] CONNECTED: ssid='%s' ip=%s (took %dms)\n",
                                st.ssid.c_str(), st.ip_addr.c_str(), (w+1)*500);
                        // dhcpcd hooks need /bin/sh — manually add route + DNS
                        apply_dhcp_route_and_dns(wifi_iface);
                        break;
                    }
                    // Log progress at 5s intervals
                    if (w > 0 && w % 10 == 0) {
                        fprintf(stderr, "[wifi] ...still waiting: state=%s ssid='%s' ip='%s'\n",
                                st.state.c_str(), st.ssid.c_str(), st.ip_addr.c_str());
                    }
                }

                // Final diagnostic dump
                if (final_ip.empty()) {
                    fprintf(stderr, "[wifi] WARNING: no IP address after 20s "
                            "(state=%s ssid='%s')\n", final_state.c_str(), final_ssid.c_str());
                    // Check if WPA auth even completed
                    if (final_state != "COMPLETED") {
                        fprintf(stderr, "[wifi] WPA authentication failed or timed out. "
                                "Check password and network security type.\n");
                    } else {
                        fprintf(stderr, "[wifi] WPA auth OK but no DHCP lease. "
                                "Network may have MAC filtering or DHCP server issues.\n");
                    }
                }

                // Log DNS resolver state
                {
                    bool found_ns = false;
                    FILE* rc = fopen("/etc/resolv.conf", "r");
                    if (rc) {
                        char line[256];
                        while (fgets(line, sizeof(line), rc)) {
                            if (strncmp(line, "nameserver", 10) == 0) {
                                // Trim newline
                                char* nl = strchr(line, '\n');
                                if (nl) *nl = '\0';
                                fprintf(stderr, "[wifi] DNS: %s\n", line);
                                found_ns = true;
                            }
                        }
                        fclose(rc);
                        if (!found_ns)
                            fprintf(stderr, "[wifi] WARNING: no nameservers in /etc/resolv.conf\n");
                    } else {
                        fprintf(stderr, "[wifi] WARNING: /etc/resolv.conf not found\n");
                    }

                    // dhcpcd uses shell hook scripts to write resolv.conf,
                    // but we have no /bin/sh (BR2_SYSTEM_BIN_SH_NONE=y).
                    // Write DNS servers directly if resolv.conf is empty.
                    if (!found_ns) {
                        fprintf(stderr, "[wifi] Writing fallback DNS (dhcpcd hooks need /bin/sh)\n");
                        // Try to use the gateway as DNS first (most routers proxy DNS)
                        // Fall back to Google/Cloudflare public DNS
                        FILE* dns = fopen("/etc/resolv.conf", "w");
                        if (dns) {
                            // Read gateway from /proc/net/route for the WiFi interface
                            FILE* rt = fopen("/proc/net/route", "r");
                            char gw_ip[INET_ADDRSTRLEN] = {};
                            if (rt) {
                                char rtline[256];
                                while (fgets(rtline, sizeof(rtline), rt)) {
                                    char iface[32];
                                    unsigned long dest, gateway;
                                    if (sscanf(rtline, "%31s %lx %lx", iface, &dest, &gateway) == 3) {
                                        if (dest == 0 && gateway != 0 &&
                                            strcmp(iface, wifi_iface.c_str()) == 0) {
                                            struct in_addr gw_addr;
                                            gw_addr.s_addr = (uint32_t)gateway;
                                            inet_ntop(AF_INET, &gw_addr, gw_ip, sizeof(gw_ip));
                                            break;
                                        }
                                    }
                                }
                                fclose(rt);
                            }
                            if (gw_ip[0]) {
                                fprintf(dns, "nameserver %s\n", gw_ip);
                                fprintf(stderr, "[wifi] DNS: using gateway %s\n", gw_ip);
                            }
                            fprintf(dns, "nameserver 8.8.8.8\n");
                            fprintf(dns, "nameserver 1.1.1.1\n");
                            fclose(dns);
                            fprintf(stderr, "[wifi] DNS: wrote /etc/resolv.conf\n");
                        }
                    }
                }
            }
        }
    }
    } // end outer wifi block
#endif // _WIN32 (guards the Linux-only WiFi fallback block opened at the #ifndef above)
    } else {
        fprintf(stderr, "[net] Ethernet obtained IP — skipping WiFi fallback\n");
    }

    // Start session expiry thread (runs every 60 seconds)
    std::thread session_expiry_thread([]() {
        try {
        while (g_running) {
            g_auth.expire_sessions();
            for (int i = 0; i < 120 && g_running; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        } catch (const std::exception& e) {
            fprintf(stderr, "[child] session_expiry thread exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[child] session_expiry thread unknown exception\n");
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

        // Force pixman (software) renderer. Mesa GL drivers (iris, radeonsi, nouveau)
        // require matching shared libraries that may be missing or incompatible in our
        // minimal Buildroot rootfs. On the VivoBook (i5-1035G1), cage crashes with:
        //   MESA-LOADER: failed to open iris: Error loading shared library
        //   [cage.c:323] Unable to create the wlroots renderer
        // pixman is slower but works on ALL hardware — simpledrm, i915, amdgpu, etc.
        // TODO: Re-enable GPU-accelerated rendering once Mesa GL is verified working
        // in the Buildroot build (needs mesa3d iris/radeonsi DRI drivers + libdrm).
        if (!getenv("WLR_RENDERER")) {
            fprintf(stderr, "[child] Using pixman renderer (software — universal compat)\n");
            setenv("WLR_RENDERER", "pixman", 1);
        }

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

        // NOTE: WLR_DRM_NO_ATOMIC was previously set here because simpledrm on
        // kernel 6.6 didn't support atomic modesetting (wlroots segfaulted).
        // Kernel 6.12 improved simpledrm's atomic support, and forcing legacy
        // mode now causes black screen (legacy page flips rejected by 6.12
        // simpledrm). Removed — let wlroots use atomic modesetting by default.
        // If atomic fails on specific hardware, set WLR_DRM_NO_ATOMIC=1 in env.

        // PATH for autostart script and child processes
        setenv("PATH", "/usr/bin:/usr/sbin:/bin:/sbin", 1);

        // Force Wayland backend for GTK/Qt apps launched from autostart
        setenv("GDK_BACKEND", "wayland", 1);
        setenv("QT_QPA_PLATFORM", "wayland", 1);

        // NOTE: udevd is now started early in init_load_modules() (init.cpp).
        // It handles coldplug for all subsystems EXCEPT input. The input
        // trigger is still delayed until AFTER the Wayland socket exists
        // (see run_udevadm_trigger lambda below) to prevent the wlroots
        // XKB shm race condition (SIGSEGV on first key press).


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
            try {
            // Helper: run udevadm settle then trigger --action=add for input devices.
            // IMPORTANT: must use --action=add (not the default "change") because
            // libinput only reacts to ADD events to register new input devices.
            // Without --action=add, libinput never sees the keyboard/touchpad and
            // key events are silently dropped even though cage doesn't crash.
            // --subsystem-match=input limits scope to only input devices (faster).
            auto run_udevadm_trigger = []() {
                // First: settle — wait for udevd to process all pending events
                // NOTE: fork() called from multi-threaded context — pre-exec code must be async-signal-safe
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
            // Optional post_ready_fn is called after socket + udev are ready,
            // BEFORE waitpid blocks — used by labwc to spawn cog (since labwc's
            // shell-based autostart doesn't work without /bin/sh).
            // Returns -1 if fork failed.
            static bool udev_triggered = false;
            auto try_compositor = [&](const char* label,
                                      std::function<void()> exec_fn,
                                      std::function<void()> post_ready_fn = nullptr) -> int {
                // NOTE: fork() called from multi-threaded context — pre-exec code must be async-signal-safe
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
                    // Post-ready callback (e.g. spawn cog for labwc)
                    if (post_ready_fn) post_ready_fn();
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

            // 2. labwc (stacking WM).  labwc's autostart is a shell script but
            //    Llamaste has no shell (BR2_SYSTEM_BIN_SH_NONE=y), so autostart
            //    silently fails.  Instead, we spawn cog ourselves via post_ready_fn
            //    after the Wayland socket appears and udev has triggered.
            {
                auto t0 = std::chrono::steady_clock::now();
                int st = try_compositor("labwc", []() {
                    execl("/usr/bin/labwc", "labwc", nullptr);
                }, []() {
                    // Wait briefly for HTTP server to start listening on port 80.
                    // The server starts on the main thread; compositor is on this
                    // detached thread.  Poll with connect() — max 15s.
                    fprintf(stderr, "[child] labwc ready, waiting for HTTP server...\n");
                    for (int i = 0; i < 30; i++) {
                        int s = socket(AF_INET, SOCK_STREAM, 0);
                        if (s >= 0) {
                            struct sockaddr_in addr = {};
                            addr.sin_family = AF_INET;
                            addr.sin_port = htons(80);
                            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                            if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                                close(s);
                                fprintf(stderr, "[child] HTTP server up, spawning cog\n");
                                break;
                            }
                            close(s);
                        }
                        usleep(500000);  // 500ms
                    }
                    // Spawn cog as a separate process (labwc manages its window).
                    // COG_PLATFORM_WL_VIEW_FULLSCREEN must be set here — labwc's
                    // /etc/labwc/environment is parsed by labwc for ITS children,
                    // but our cog is fork+exec'd directly, not via labwc.
                    // NOTE: fork() called from multi-threaded context — pre-exec code must be async-signal-safe
                    pid_t cpid = fork();
                    if (cpid == 0) {
                        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
                        setenv("COG_PLATFORM_WL_VIEW_FULLSCREEN", "1", 1);
                        execl("/usr/bin/cog", "cog", "http://localhost", nullptr);
                        _exit(127);
                    }
                    if (cpid > 0)
                        fprintf(stderr, "[child] cog spawned pid=%d\n", cpid);
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
            } catch (const std::exception& e) {
                fprintf(stderr, "[child] compositor thread exception: %s\n", e.what());
            } catch (...) {
                fprintf(stderr, "[child] compositor thread unknown exception\n");
            }
        }).detach();
    }
#endif

    // Create HTTP server
    httplib::Server svr;

    // Limit request body size to prevent memory exhaustion DoS.
    // 10 MB is enough for audio uploads while keeping memory bounded.
    svr.set_payload_max_length(10 * 1024 * 1024);

    // CORS headers — same-origin only (no wildcard). Combined with
    // cleartext cookies, wildcard CORS enables CSRF from any website.
    // The web UI is served from the same origin, so no CORS is needed.
    svr.set_default_headers({
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
            // Desktop mode: bypass auth only for localhost clients
            // (e.g., cog embedded browser). Remote LAN clients in desktop
            // mode must still authenticate.
            if (g_boot_mode == "desktop" &&
                (req.remote_addr == "127.0.0.1" || req.remote_addr == "::1")) {
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
        // Desktop mode: skip auth/setup gates only for localhost (cog).
        // Remote clients in desktop mode must still pass through setup/login gates.
        if (g_boot_mode == "desktop" &&
            (req.remote_addr == "127.0.0.1" || req.remote_addr == "::1")) {
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

        // Step 2: Start async download (returns immediately)
        bool started = start_async_download(
            rec.value("repo_id", ""),
            rec.value("filename", ""),
            rec.value("model_name", ""));

        json out;
        if (started) {
            out["status"] = "started";
            out["model_name"] = rec.value("model_name", "");
            out["message"] = "Download started. Poll /llamaste/model/download/progress for status.";
        } else {
            out["status"] = "busy";
            out["message"] = "A download is already in progress.";
        }
        res.set_content(out.dump(2), "application/json");
    }));

    // GET /llamaste/model/download/progress — Poll async download progress
    svr.Get("/llamaste/model/download/progress", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(get_download_progress(), "application/json");
    }));

    svr.Get("/llamaste/model/recommended", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string result = g_tools.dispatch("model.recommended", "{}");
        res.set_content(result, "application/json");
    }));

    // GET /llamaste/model/tiers — List all downloadable model sizes
    svr.Get("/llamaste/model/tiers", require_auth(
        [](const httplib::Request& /*req*/, httplib::Response& res) {
        const ModelInfo* table = get_model_table();
        // Get available RAM
        int avail_mb = 0;
        std::ifstream meminfo("/proc/meminfo");
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.find("MemAvailable:") == 0) {
                long kb = 0;
                sscanf(line.c_str(), "MemAvailable: %ld", &kb);
                avail_mb = (int)(kb / 1024);
                break;
            }
        }
        const ModelInfo* rec = recommend_model(avail_mb);

        json tiers = json::array();
        for (int i = 0; table[i].name; i++) {
            json t;
            t["name"] = table[i].name;
            t["filename"] = table[i].filename;
            t["repo_id"] = table[i].repo_id;
            t["required_mb"] = table[i].required_mb;
            t["approx_size_mb"] = table[i].approx_size_mb;
            t["fits_ram"] = (avail_mb >= table[i].required_mb);
            t["recommended"] = (rec && strcmp(rec->name, table[i].name) == 0);
            // Check if already downloaded
            std::string path = std::string("/data/models/") + table[i].filename;
            t["downloaded"] = (access(path.c_str(), R_OK) == 0);
            tiers.push_back(t);
        }
        json out;
        out["tiers"] = tiers;
        out["ram_available_mb"] = avail_mb;
        res.set_content(out.dump(2), "application/json");
    }));

    // POST /llamaste/model/download-tier — Download a specific model tier
    svr.Post("/llamaste/model/download-tier", require_auth(
        [](const httplib::Request& req, httplib::Response& res) {
        json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded()) {
            res.set_content(R"({"error":"invalid JSON"})", "application/json");
            return;
        }
        std::string repo_id = body.value("repo_id", "");
        std::string filename = body.value("filename", "");
        std::string model_name = body.value("name", "");
        if (repo_id.empty() || filename.empty()) {
            res.set_content(R"({"error":"repo_id and filename required"})", "application/json");
            return;
        }
        bool started = start_async_download(repo_id, filename, model_name);
        json out;
        if (started) {
            out["status"] = "started";
            out["model_name"] = model_name;
        } else {
            out["status"] = "busy";
            out["message"] = "A download is already in progress.";
        }
        res.set_content(out.dump(2), "application/json");
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
            if (body.is_discarded() || !body.contains("text") || !body["text"].is_string()) {
                res.status = 400;
                res.set_content(R"json({"error":"text field required (string)"})json", "application/json");
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

    // --- Installer routes (any mode when booted from live ISO) ---
    if (is_live_iso) {
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
            // Force confirm for HTTP API: pass confirmed=true to dispatch. The
            // 3rd arg is the gate that install.to_disk (requires_confirmation)
            // checks — NOT the "confirm" field in args — so the 2-arg form here
            // always returned "confirmation required" and the web installer stalled.
            body["confirm"] = true;
            std::string result = g_tools.dispatch("install.to_disk", body.dump(), true);
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
            std::thread([](){
                try {
                    g_cluster.run_election();
                } catch (const std::exception& e) {
                    fprintf(stderr, "[child] election thread exception: %s\n", e.what());
                } catch (...) {
                    fprintf(stderr, "[child] election thread unknown exception\n");
                }
            }).detach();

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

    // --- Debug endpoints for remote diagnosis ---
#ifndef _WIN32
    // Helper: read last N lines from a file (tail-like)
    auto tail_file = [](const std::string& path, int max_lines) -> std::string {
        std::ifstream f(path, std::ios::ate);
        if (!f.is_open()) return "(file not found: " + path + ")\n";
        auto size = f.tellg();
        if (size == 0) return "(empty file)\n";

        // Scan backwards for newlines
        int newlines = 0;
        std::streamoff pos = size;
        while (pos > 0 && newlines <= max_lines) {
            f.seekg(--pos);
            if (f.get() == '\n') newlines++;
        }
        if (pos > 0) f.seekg(pos + 1); // skip past the newline we stopped on
        else f.seekg(0);

        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };

    // Helper: read entire file contents
    auto read_file = [](const std::string& path) -> std::string {
        std::ifstream f(path);
        if (!f.is_open()) return "(file not found: " + path + ")\n";
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };

    svr.Get("/debug/logs", require_auth([tail_file](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(tail_file("/tmp/llama-server.log", 200), "text/plain");
    }));

    svr.Get("/debug/wpa", require_auth([tail_file](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(tail_file("/tmp/wpa_supplicant.log", 200), "text/plain");
    }));

    svr.Get("/debug/dmesg", require_auth([](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string output;
        int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            res.set_content("(cannot open /dev/kmsg)\n", "text/plain");
            return;
        }
        char buf[4096];
        // Read all available messages until EAGAIN
        for (;;) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            if (n <= 0) break; // EAGAIN or error
            buf[n] = '\0';
            output.append(buf, n);
            // Safety limit: ~1MB
            if (output.size() > 1024 * 1024) break;
        }
        close(fd);
        if (output.empty()) output = "(no kernel messages available)\n";
        res.set_content(output, "text/plain");
    }));

    svr.Get("/debug/modules", require_auth([read_file](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_content(read_file("/proc/modules"), "text/plain");
    }));

    svr.Get("/debug/network", require_auth([read_file](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string output;

        // Interfaces from /sys/class/net/
        output += "=== Interfaces ===\n";
        DIR* d = opendir("/sys/class/net");
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string iface = ent->d_name;
                output += iface + ": ";
                // Read operstate
                std::ifstream state_f("/sys/class/net/" + iface + "/operstate");
                if (state_f.is_open()) {
                    std::string state;
                    std::getline(state_f, state);
                    output += "state=" + state;
                }
                // Read address
                std::ifstream addr_f("/sys/class/net/" + iface + "/address");
                if (addr_f.is_open()) {
                    std::string addr;
                    std::getline(addr_f, addr);
                    output += " mac=" + addr;
                }
                output += "\n";
            }
            closedir(d);
        }

        output += "\n=== Routes (/proc/net/route) ===\n";
        output += read_file("/proc/net/route");

        output += "\n=== DNS (/etc/resolv.conf) ===\n";
        output += read_file("/etc/resolv.conf");

        res.set_content(output, "text/plain");
    }));

    svr.Get("/debug/block-devices", require_auth([read_file](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string output;

        // List all block devices and their partitions
        output += "=== Block Devices (/sys/block/) ===\n";
        DIR* d = opendir("/sys/block");
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string blk = ent->d_name;
                if (blk.find("loop") == 0 || blk.find("ram") == 0) continue;

                // Read size (in 512-byte sectors)
                std::string size_str;
                std::ifstream sf("/sys/block/" + blk + "/size");
                if (sf.is_open()) std::getline(sf, size_str);
                uint64_t sectors = size_str.empty() ? 0 : strtoull(size_str.c_str(), nullptr, 10);
                uint64_t mb = (sectors * 512) / (1024 * 1024);

                output += blk + ": " + std::to_string(mb) + " MB";

                // Read model if available
                std::ifstream mf("/sys/block/" + blk + "/device/model");
                if (mf.is_open()) {
                    std::string model;
                    std::getline(mf, model);
                    output += " model=[" + model + "]";
                }
                output += "\n";

                // List partitions
                DIR* bd = opendir(("/sys/block/" + blk).c_str());
                if (bd) {
                    struct dirent* pe;
                    while ((pe = readdir(bd)) != nullptr) {
                        std::string pn = pe->d_name;
                        if (pn.find(blk) != 0) continue;
                        std::string part_file = "/sys/block/" + blk + "/" + pn + "/partition";
                        struct stat st;
                        if (stat(part_file.c_str(), &st) == 0) {
                            std::string psz;
                            std::ifstream psf("/sys/block/" + blk + "/" + pn + "/size");
                            if (psf.is_open()) std::getline(psf, psz);
                            uint64_t ps = psz.empty() ? 0 : strtoull(psz.c_str(), nullptr, 10);
                            uint64_t pmb = (ps * 512) / (1024 * 1024);
                            // Check if /dev node exists
                            std::string devpath = "/dev/" + pn;
                            bool exists = (stat(devpath.c_str(), &st) == 0);
                            output += "  " + pn + ": " + std::to_string(pmb) + " MB";
                            output += exists ? " [/dev node OK]" : " [/dev node MISSING]";
                            output += "\n";
                        }
                    }
                    closedir(bd);
                }
            }
            closedir(d);
        }

        output += "\n=== Mounts (/proc/mounts) ===\n";
        output += read_file("/proc/mounts");

        output += "\n=== Init Resize Log ===\n";
        std::string rlog = read_file("/tmp/init-resize.log");
        output += rlog.empty() ? "(no log)\n" : rlog;

        res.set_content(output, "text/plain");
    }));

    svr.Get("/debug/dmesg", require_auth([](const httplib::Request& req, httplib::Response& res) {
        std::string output;
        std::string filter = req.has_param("filter") ? req.get_param_value("filter") : "";

        // Read kernel log from /dev/kmsg
        int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            output = "Cannot open /dev/kmsg: " + std::string(strerror(errno)) + "\n";
            // Fallback: try /proc/kmsg
            std::ifstream kmsg("/proc/kmsg");
            if (kmsg.is_open()) {
                output = "(/proc/kmsg not suitable for non-blocking read)\n";
            }
        } else {
            // Seek to end-N messages (read last ~64KB)
            lseek(fd, 0, SEEK_DATA);
            char buf[512];
            int count = 0;
            std::vector<std::string> lines;
            while (count < 2000) {
                ssize_t n = read(fd, buf, sizeof(buf) - 1);
                if (n <= 0) break;
                buf[n] = '\0';
                // /dev/kmsg format: "priority,seq,timestamp;message\n"
                char* msg = strchr(buf, ';');
                if (msg) {
                    msg++;  // skip ';'
                    std::string line(msg);
                    // Strip trailing newline
                    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                        line.pop_back();
                    if (filter.empty() || line.find(filter) != std::string::npos) {
                        lines.push_back(line);
                    }
                }
                count++;
            }
            close(fd);

            // Return last 200 matching lines
            size_t start = lines.size() > 200 ? lines.size() - 200 : 0;
            for (size_t i = start; i < lines.size(); i++) {
                output += lines[i] + "\n";
            }
            if (lines.empty()) {
                output = "(no matching messages)\n";
            }
        }

        res.set_content(output, "text/plain");
    }));

    svr.Get("/debug/drm", require_auth([read_file](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string output;

        // /dev/dri/ contents
        output += "=== DRI Devices (/dev/dri/) ===\n";
        DIR* d = opendir("/dev/dri");
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string path = std::string("/dev/dri/") + ent->d_name;
                struct stat st;
                if (stat(path.c_str(), &st) == 0) {
                    output += "  " + std::string(ent->d_name) +
                              " (major=" + std::to_string(major(st.st_rdev)) +
                              " minor=" + std::to_string(minor(st.st_rdev)) + ")\n";
                } else {
                    output += "  " + std::string(ent->d_name) + "\n";
                }
            }
            closedir(d);
        } else {
            output += "  /dev/dri/ not found\n";
        }

        // /sys/class/drm/ contents
        output += "\n=== DRM Class (/sys/class/drm/) ===\n";
        d = opendir("/sys/class/drm");
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                std::string name = ent->d_name;
                output += "  " + name;
                // Read status if available
                std::ifstream sf("/sys/class/drm/" + name + "/status");
                if (sf.is_open()) {
                    std::string status;
                    std::getline(sf, status);
                    output += " status=" + status;
                }
                // Read enabled
                std::ifstream ef("/sys/class/drm/" + name + "/enabled");
                if (ef.is_open()) {
                    std::string enabled;
                    std::getline(ef, enabled);
                    output += " enabled=" + enabled;
                }
                output += "\n";
            }
            closedir(d);
        } else {
            output += "  /sys/class/drm/ not found\n";
        }

        // Driver info
        output += "\n=== DRM Driver ===\n";
        output += read_file("/sys/class/drm/version");

        // Compositor env
        output += "\n=== Compositor Environment ===\n";
        const char* vars[] = {"WLR_RENDERER", "WLR_DRM_NO_ATOMIC",
                              "WLR_NO_HARDWARE_CURSORS", "WLR_LIBINPUT_NO_DEVICES",
                              "LIBSEAT_BACKEND", "XDG_RUNTIME_DIR", nullptr};
        for (int i = 0; vars[i]; i++) {
            const char* v = getenv(vars[i]);
            output += std::string("  ") + vars[i] + "=" + (v ? v : "(not set)") + "\n";
        }

        // Compositor log
        output += "\n=== Compositor Log (/tmp/compositor.log) ===\n";
        std::string clog = read_file("/tmp/compositor.log");
        output += clog.empty() ? "(no log)\n" : clog;

        // Wayland socket
        output += "\n=== Wayland Socket ===\n";
        struct stat wst;
        if (stat("/run/user/0/wayland-0", &wst) == 0) {
            output += "  /run/user/0/wayland-0 EXISTS\n";
        } else {
            output += "  /run/user/0/wayland-0 MISSING\n";
        }

        res.set_content(output, "text/plain");
    }));

    svr.Get("/debug/audio", require_auth([read_file](const httplib::Request& /*req*/, httplib::Response& res) {
        std::string output;

        // Check ALSA sound devices
        output += "=== Sound Cards (/proc/asound/cards) ===\n";
        output += read_file("/proc/asound/cards");

        output += "\n=== PCM Devices (/proc/asound/pcm) ===\n";
        output += read_file("/proc/asound/pcm");

        output += "\n=== Sound Devices (/dev/snd/) ===\n";
        DIR* d = opendir("/dev/snd");
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != nullptr) {
                if (ent->d_name[0] == '.') continue;
                output += std::string("  ") + ent->d_name + "\n";
            }
            closedir(d);
        } else {
            output += "  /dev/snd/ not found\n";
        }

        // Voice pipeline state
        output += "\n=== Voice Pipeline ===\n";
        extern VoicePipeline* g_voice;
        if (g_voice) {
            output += "Voice pipeline: INITIALIZED\n";
            output += "TTS engine: " + g_voice->tts_engine_name() + "\n";
        } else {
            output += "Voice pipeline: NOT INITIALIZED (only enabled in desktop mode)\n";
        }

        // Check TTS model files
        output += "\n=== TTS Models (/data/models/) ===\n";
        DIR* md = opendir("/data/models");
        if (md) {
            struct dirent* ent;
            while ((ent = readdir(md)) != nullptr) {
                std::string name = ent->d_name;
                if (name.find("vits") != std::string::npos ||
                    name.find("piper") != std::string::npos ||
                    name.find("tts") != std::string::npos ||
                    name.find("onnx") != std::string::npos) {
                    output += "  " + name + "\n";
                }
            }
            closedir(md);
        }

        res.set_content(output, "text/plain");
    }));

    svr.Get("/debug/sysinfo", require_auth([](const httplib::Request& /*req*/, httplib::Response& res) {
        json info;

        // Hostname
        char hostname[256] = {};
        gethostname(hostname, sizeof(hostname));
        info["hostname"] = hostname;

        // Kernel version
        struct utsname uts;
        if (uname(&uts) == 0) {
            info["kernel"] = uts.release;
            info["arch"] = uts.machine;
        }

        // Boot mode
        info["boot_mode"] = g_boot_mode;

        // Uptime
        std::ifstream uptime_f("/proc/uptime");
        if (uptime_f.is_open()) {
            double up_secs = 0;
            uptime_f >> up_secs;
            info["uptime_seconds"] = up_secs;
            int h = (int)(up_secs / 3600);
            int m = (int)((up_secs - h * 3600) / 60);
            int s = (int)(up_secs) % 60;
            char buf[64];
            snprintf(buf, sizeof(buf), "%dh %dm %ds", h, m, s);
            info["uptime_human"] = buf;
        }

        // CPU info (model name + count)
        std::ifstream cpu_f("/proc/cpuinfo");
        if (cpu_f.is_open()) {
            std::string line;
            int cpu_count = 0;
            std::string model;
            while (std::getline(cpu_f, line)) {
                if (line.find("processor") == 0) cpu_count++;
                if (model.empty() && line.find("model name") == 0) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos)
                        model = line.substr(colon + 2);
                }
            }
            info["cpu_model"] = model;
            info["cpu_count"] = cpu_count;
        }

        // Memory from /proc/meminfo
        std::ifstream mem_f("/proc/meminfo");
        if (mem_f.is_open()) {
            json mem;
            std::string line;
            while (std::getline(mem_f, line)) {
                if (line.find("MemTotal:") == 0 ||
                    line.find("MemFree:") == 0 ||
                    line.find("MemAvailable:") == 0 ||
                    line.find("SwapTotal:") == 0 ||
                    line.find("SwapFree:") == 0) {
                    auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        std::string key = line.substr(0, colon);
                        std::string val = line.substr(colon + 1);
                        // Trim leading whitespace
                        auto start = val.find_first_not_of(" \t");
                        if (start != std::string::npos) val = val.substr(start);
                        mem[key] = val;
                    }
                }
            }
            info["memory"] = mem;
        }

        // Disk usage for /
        struct statvfs st;
        if (statvfs("/", &st) == 0) {
            json disk;
            uint64_t total = (uint64_t)st.f_blocks * st.f_frsize;
            uint64_t avail = (uint64_t)st.f_bavail * st.f_frsize;
            uint64_t used  = total - (uint64_t)st.f_bfree * st.f_frsize;
            disk["total_mb"] = total / (1024 * 1024);
            disk["used_mb"]  = used / (1024 * 1024);
            disk["avail_mb"] = avail / (1024 * 1024);
            info["disk_root"] = disk;
        }

        // Llamaste server uptime
        info["server_uptime_seconds"] = (int)(time(nullptr) - g_start_time);

        res.set_content(info.dump(2), "application/json");
    }));
#endif // _WIN32

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
        try {
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        fprintf(stderr, "[child] Stopping HTTP server...\n");
        svr.stop();
        } catch (const std::exception& e) {
            fprintf(stderr, "[child] shutdown_watcher thread exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[child] shutdown_watcher thread unknown exception\n");
        }
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
        try {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        mark_boot_success();
        fprintf(stderr, "[update] Boot health check complete\n");
        } catch (const std::exception& e) {
            fprintf(stderr, "[child] boot_success thread exception: %s\n", e.what());
        } catch (...) {
            fprintf(stderr, "[child] boot_success thread unknown exception\n");
        }
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

    // Join stderr tee thread so it flushes and closes cleanly.
    // Closing the pipe write-end (dup2'd to STDERR_FILENO) causes read()
    // to return 0, which exits the thread.
#ifndef _WIN32
    close(STDERR_FILENO);  // unblock tee thread
    if (g_tee_thread.joinable()) {
        // Wait up to 2s for the thread to finish; detach if stuck.
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_tee_thread.joinable() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_tee_thread.joinable()) {
            g_tee_thread.detach();  // stuck — give up and proceed
        } else {
            g_tee_thread.join();
        }
    }
#endif

    return 0;
}
