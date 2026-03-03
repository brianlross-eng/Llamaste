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

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#include <sys/statvfs.h>
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
// System info gathering (for /llamaste/system endpoint)
// ---------------------------------------------------------------------------

static json gather_system_info(const SupervisorConfig& config) {
    json info;

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

    // Placeholder fields for Phase 2 features
    info["tokens_per_sec"] = 0.0;
    info["scheduled_tasks_count"] = 0;
    info["active_alerts"] = json::array();

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

                // Run agent turn with stub inference
                std::string final_text = agent_turn(conv, g_tools, stub_inference);

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

        std::string result = agent_turn(conv, g_tools, stub_inference);

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

    // Use stub inference directly to get the response
    std::string request = build_inference_request(conv, g_tools);
    std::string response = stub_inference(request);

    res.set_content(response, "application/json");
}

// GET /health — Health check
static void handle_health(const httplib::Request& /*req*/, httplib::Response& res) {
    json health;
    health["status"] = "ok";
    health["uptime_seconds"] = static_cast<int>(time(nullptr) - g_start_time);
    health["model_loaded"] = false;  // Phase 1: no real model
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
    fprintf(stderr, "[child] Registered %d tools\n", g_tools.count());

    // Detect hardware
    g_hwinfo = detect_hardware();
    fprintf(stderr, "[child] CPU: %s (%d cores), RAM: %d MB\n",
            g_hwinfo.cpu_model.c_str(), g_hwinfo.cpu_cores, g_hwinfo.ram_total_mb);

    // Build system prompt
    g_system_prompt = build_system_prompt(g_hwinfo, g_tools, config.boot_mode);
    fprintf(stderr, "[child] System prompt: %zu bytes\n", g_system_prompt.size());

    // Start mDNS responder so the box is discoverable as "llamaste.local"
    MdnsResponder mdns;
    if (mdns.start("llamaste")) {
        fprintf(stderr, "[child] mDNS: responding as llamaste.local (%s)\n",
                MdnsResponder::get_local_ip().c_str());
    } else {
        fprintf(stderr, "[child] mDNS: could not start (non-fatal)\n");
    }

    // Create HTTP server
    httplib::Server svr;

    // CORS headers for development
    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
    });

    // Handle OPTIONS preflight requests
    svr.Options(".*", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.status = 204;
    });

    // --- Static file routes ---
    svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
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

    // --- API routes ---
    svr.Post("/llamaste/chat", handle_chat);

    svr.Get("/llamaste/system",
        [&config](const httplib::Request& req, httplib::Response& res) {
            handle_system(req, res, config);
        }
    );

    svr.Get("/llamaste/tools", handle_tools);
    svr.Get("/llamaste/conversations", handle_conversations);
    svr.Post("/v1/chat/completions", handle_openai_completions);
    svr.Get("/health", handle_health);

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

        // Serve install.js
        svr.Get("/install.js", [](const httplib::Request& req, httplib::Response& res) {
#ifdef LLAMASTE_HAS_EMBED
            extern const unsigned char WEB_INSTALL_JS[];
            extern const unsigned int WEB_INSTALL_JS_LEN;
            serve_static_file(req, res, "install.js", WEB_INSTALL_JS, WEB_INSTALL_JS_LEN);
#else
            serve_static_file(req, res, "install.js", nullptr, 0);
#endif
        });

        fprintf(stderr, "[child] Installer routes enabled: /install/disks, /install/start, /install/progress\n");
    }

    // --- Error handler ---
    svr.set_error_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
        json err;
        err["error"] = "Not found";
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
    fprintf(stderr, "[child] Running in %s mode\n",
            config.model_path.empty() ? "stub (no model)" : "inference");

    bool ok = svr.listen("0.0.0.0", port);
    if (!ok && g_running) {
        fprintf(stderr, "[child] Failed to start HTTP server on port %d\n", port);
        mdns.stop();
        return 1;
    }

    // Clean shutdown
    mdns.stop();
    fprintf(stderr, "[child] HTTP server stopped\n");
    return 0;
}
