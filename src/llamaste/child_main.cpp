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

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
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

// llama-rpc-server process state (mesh clustering)
#ifndef _WIN32
static std::atomic<pid_t> g_rpc_pid{0};
#endif
static int g_rpc_port = 50052;

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

// Context window size based on free RAM after model loading.
// Smaller contexts = faster inference on CPU. 4096 is plenty for most
// single-turn conversations; 8192 for multi-turn agent loops.
int compute_context_size(int free_ram_mb) {
    if (free_ram_mb >= 4096) return 8192;
    if (free_ram_mb >= 2048) return 4096;
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
            fprintf(stderr, "[inference] llama-server unreachable (no result)\n");
            msg["content"] = "[inference error: llama-server unreachable]";
        } else {
            fprintf(stderr, "[inference] llama-server HTTP %d: %s\n",
                    result->status, result->body.substr(0, 500).c_str());
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
        args.push_back("--mlock");
        args.push_back("-fa");
        args.push_back("--jinja");
        args.push_back("--chat-template"); args.push_back("chatml");
        args.push_back("--log-disable");

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

    // Create a minimal conversation for the stub
    ConversationState conv;
    conv.system_prompt = g_system_prompt;
    if (!user_msg.empty()) {
        conv.add_user_message(user_msg);
    }

    // Use current inference function to get the response
    std::string request = build_inference_request(conv, g_tools);
    std::string response = g_inference_fn(request);

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
// child_main — HTTP server entry point (called from supervisor fork)
// ---------------------------------------------------------------------------

int child_main(const SupervisorConfig& config) {
    signal(SIGTERM, child_signal);
    signal(SIGINT, child_signal);

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

    // Initialize voice pipeline — TTS in all modes, full STT+listen in desktop only
    VoicePipeline voice_pipeline;
    {
        extern VoicePipeline* g_voice;  // defined in tools_audio.cpp
#ifndef _WIN32
        VoiceConfig vcfg;

        if (voice_pipeline.init(vcfg)) {
            g_voice = &voice_pipeline;
            fprintf(stderr, "[child] Voice pipeline initialized (tts=%s)\n",
                    voice_pipeline.tts_engine_name().c_str());

            // Desktop mode: enable always-listening (ALSA capture + VAD)
            if (g_boot_mode == "desktop" &&
                access(vcfg.whisper_model.c_str(), R_OK) == 0) {
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
                fprintf(stderr, "[child] Server mode: TTS available via API, STT disabled\n");
            }
        } else {
            fprintf(stderr, "[child] Voice pipeline init failed: %s\n",
                    voice_pipeline.last_error().c_str());
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
                return;
            }

            // Build RPC endpoints and tensor split
            std::string rpc = g_cluster.rpc_endpoint_list();
            std::string tensor_split = g_cluster.compute_tensor_split();

            int free_ram_estimate = g_hwinfo.ram_free_mb - 512;
            if (spawn_llama_server(model_path, g_hwinfo.cpu_cores, free_ram_estimate, rpc,
                                   tensor_split)) {
                if (wait_for_llama_server(120)) {
                    g_model_loaded.store(true);
                    g_inference_fn = llama_inference;
                    fprintf(stderr, "[cluster] llama-server restarted: rpc=%s split=%s\n",
                            rpc.c_str(), tensor_split.c_str());
                    if (cap.upgrade_available) {
                        fprintf(stderr, "[cluster] NOTE: A larger model could fit this cluster. "
                                "Download it to /data/models/ for automatic upgrade.\n");
                    }
                }
            }
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

    // --- Desktop mode: launch Wayland kiosk compositor ---
#ifndef _WIN32
    if (g_boot_mode == "desktop") {
        fprintf(stderr, "[child] Desktop mode: will launch compositor after HTTP server starts\n");

        // Set up Wayland environment
        mkdir("/run/user", 0755);
        mkdir("/run/user/0", 0700);
        setenv("XDG_RUNTIME_DIR", "/run/user/0", 1);
        // Allow cage to start even without physical input devices (QEMU, VM)
        setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);

        // Launch compositor in a thread — polls for HTTP readiness first
        std::thread([]() {
            // Poll for HTTP server to be listening (up to 15 seconds)
            bool ready = false;
            for (int i = 0; i < 75 && g_running; i++) {
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock >= 0) {
                    struct sockaddr_in addr = {};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(80);
                    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                        close(sock);
                        ready = true;
                        break;
                    }
                    close(sock);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (!ready) {
                fprintf(stderr, "[child] Compositor: HTTP server not ready after 15s, launching anyway\n");
            }

            // Probe for available browser binary before forking
            // cage is a single-window Wayland compositor that runs one client
            // We need to find which browser client is available
            const char* browser = nullptr;
            if (access("/usr/bin/cog", X_OK) == 0) {
                browser = "cog";
            } else if (access("/usr/bin/midori", X_OK) == 0) {
                browser = "midori";
            } else if (access("/usr/bin/chromium", X_OK) == 0) {
                browser = "chromium";
            }

            pid_t pid = fork();
            if (pid == 0) {
                // Set PATH for child processes (cage needs it to find browser)
                setenv("PATH", "/usr/bin:/usr/sbin:/bin:/sbin", 1);

                // Child: exec compositor with detected browser
                // Use execl with full paths — PID 1 has no PATH
                if (browser && access("/usr/bin/cage", X_OK) == 0) {
                    if (strcmp(browser, "cog") == 0) {
                        execl("/usr/bin/cage", "cage", "-s", "--",
                              "/usr/bin/cog", "http://localhost", nullptr);
                    } else if (strcmp(browser, "midori") == 0) {
                        execl("/usr/bin/cage", "cage", "-s", "--",
                              "/usr/bin/midori", "-e", "Fullscreen", "-a",
                              "http://localhost", nullptr);
                    } else if (strcmp(browser, "chromium") == 0) {
                        execl("/usr/bin/cage", "cage", "-s", "--",
                              "/usr/bin/chromium", "--no-sandbox", "--kiosk",
                              "http://localhost", nullptr);
                    }
                }
                // Last resort: weston kiosk mode (no separate browser needed)
                execl("/usr/bin/weston", "weston", "--shell=kiosk",
                      "--continue-without-input", nullptr);
                // All options failed
                fprintf(stderr, "[child] No compositor available (errno=%d: %s)\n",
                        errno, strerror(errno));
                _exit(1);
            } else if (pid > 0) {
                g_cage_pid.store(pid);
                fprintf(stderr, "[child] Desktop compositor launched (pid %d, browser=%s)\n",
                        pid, browser ? browser : "weston-kiosk");
                // Wait for compositor to exit, log it
                int status = 0;
                waitpid(pid, &status, 0);
                g_cage_pid.store(0);
                fprintf(stderr, "[child] Desktop compositor exited (status %d)\n", status);
            } else {
                fprintf(stderr, "[child] Failed to fork compositor: %s\n", strerror(errno));
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

    // --- Update endpoints ---
    svr.Get("/llamaste/update/status", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        json status;
        status["version"] = LLAMASTE_VERSION;
        status["active_slot"] = detect_current_slot();
        status["inactive_slot"] = inactive_slot(detect_current_slot());
        // Check if inactive slot has metadata
        std::string inact = inactive_slot(detect_current_slot());
        std::string meta_path = "/data/llamaste/slots/" + inact + ".json";
        std::ifstream mf(meta_path);
        if (mf.is_open()) {
            try {
                json meta = json::parse(mf);
                status["inactive_version"] = meta.value("version", "");
            } catch (...) {}
        }
        status["update_state"] = "idle";
        res.set_content(status.dump(), "application/json");
    }));

    svr.Post("/llamaste/update/check", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        json result;
        result["current_version"] = LLAMASTE_VERSION;
        result["available"] = false;
        result["message"] = "Online update checking coming soon. Upload a .update file via the web UI.";
        res.set_content(result.dump(), "application/json");
    }));

    svr.Post("/llamaste/update/rollback", require_auth(
        [](const httplib::Request&, httplib::Response& res) {
        std::string grubenv_path = find_grubenv_path();
        if (grubenv_path.empty()) {
            json err;
            err["error"] = "grubenv not found — cannot switch slot";
            res.status = 500;
            res.set_content(err.dump(), "application/json");
            return;
        }
        std::string current = detect_current_slot();
        std::string new_slot = inactive_slot(current);
        auto vars = grubenv_read(grubenv_path);
        vars["active_slot"] = new_slot;
        vars["boot_success"] = "0";
        vars["boot_counter"] = "3";
        if (grubenv_write(grubenv_path, vars)) {
            json ok;
            ok["success"] = true;
            ok["new_slot"] = new_slot;
            ok["message"] = "Switched to slot " + new_slot + ". Reboot to activate.";
            res.set_content(ok.dump(), "application/json");
        } else {
            json err;
            err["error"] = "Failed to write grubenv";
            res.status = 500;
            res.set_content(err.dump(), "application/json");
        }
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
