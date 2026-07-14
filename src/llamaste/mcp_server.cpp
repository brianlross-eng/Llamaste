#include "version.h"
// mcp_server.cpp — Model Context Protocol server for Llamaste
//
// MCP Streamable HTTP transport (2025-03-26):
//   POST /mcp   — All JSON-RPC 2.0 MCP requests
//   GET  /mcp   — Discovery / health check
//
// Auth (two accepted methods, checked on every /mcp request):
//   1. Session cookie  — same as web UI (for browser users)
//   2. Bearer token    — Authorization: Bearer <api-key>
//                        Key stored in /data/llamaste/mcp_key.txt
//                        Generated on first use, persists across reboots.
//
// Key management routes (cookie-auth only, in child_main.cpp):
//   GET  /llamaste/mcp/key          → masked key info (show in System panel)
//   POST /llamaste/mcp/key/regenerate → generate + return new key

#include "mcp_server.h"

#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#endif

#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

#ifdef __linux__
#include <sys/random.h>   // getrandom() syscall
#endif

using json = nlohmann::json;

// Global instance (created and owned by child_main.cpp)
McpServer* g_mcp = nullptr;

// Default key file path
static constexpr const char* MCP_KEY_FILE = "/data/llamaste/mcp_key.txt";

// JSON-RPC 2.0 error codes
static constexpr int JSONRPC_PARSE_ERROR      = -32700;
static constexpr int JSONRPC_INVALID_REQUEST  = -32600;
static constexpr int JSONRPC_METHOD_NOT_FOUND = -32601;
static constexpr int JSONRPC_INVALID_PARAMS   = -32602;
static constexpr int JSONRPC_INTERNAL_ERROR   = -32603;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
McpServer::McpServer(const ToolRegistry& tools)
    : tools_(tools), api_key_file_(MCP_KEY_FILE) {}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------
void McpServer::add_routes(httplib::Server& svr, McpAuthCheck auth_check) {
    auth_check_ = auth_check;

    // Eagerly load or create the API key so it's ready before first request
    load_or_create_api_key();

    auto self = this;

    // Custom auth wrapper: accepts EITHER session cookie OR Bearer token
    auto mcp_auth = [self](McpHandler handler) -> McpHandler {
        return [self, handler](const httplib::Request& req, httplib::Response& res) {
            // --- Check 1: Bearer token ---
            bool bearer_ok = false;
            if (req.has_header("Authorization")) {
                std::string auth_hdr = req.get_header_value("Authorization");
                // "Bearer <token>" — must be exactly 64 hex chars
                if (auth_hdr.size() > 7 &&
                    auth_hdr.substr(0, 7) == "Bearer ") {
                    std::string token = auth_hdr.substr(7);
                    std::lock_guard<std::mutex> lock(self->api_key_mu_);
                    bearer_ok = (!token.empty() && token == self->api_key_);
                }
            }

            // --- Check 2: Session cookie ---
            bool cookie_ok = self->auth_check_ && self->auth_check_(req);

            if (!bearer_ok && !cookie_ok) {
                res.status = 401;
                res.set_header("Access-Control-Allow-Origin", "*");
                json err;
                err["error"] = "Unauthorized";
                err["hint"]  = "Use Authorization: Bearer <api-key> or a valid session cookie";
                res.set_content(err.dump(), "application/json");
                return;
            }

            handler(req, res);
        };
    };

    // ----- POST /mcp -----
    svr.Post("/mcp", mcp_auth([self](const httplib::Request& req, httplib::Response& res) {
        self->handle_post(req, res);
    }));

    // ----- GET /mcp -----
    svr.Get("/mcp", mcp_auth([](const httplib::Request& /*req*/, httplib::Response& res) {
        json info;
        info["server"]    = "llamaste";
        info["version"]   = "0.2.0";
        info["protocol"]  = MCP_PROTOCOL_VERSION;
        info["transport"] = "streamable-http";
        info["endpoint"]  = "/mcp";
        info["auth"]      = "Bearer token or session cookie";
        info["docs"]      = "POST JSON-RPC 2.0 to /mcp";
        res.set_content(info.dump(), "application/json");
    }));

    // ----- OPTIONS /mcp -----
    svr.Options("/mcp", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin",  "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers",
                       "Content-Type, Mcp-Session-Id, Authorization, Accept");
        res.status = 204;
    });
}

// ---------------------------------------------------------------------------
// API key management
// ---------------------------------------------------------------------------
std::string McpServer::load_or_create_api_key() {
    std::lock_guard<std::mutex> lock(api_key_mu_);
    if (!api_key_.empty()) return api_key_;

    // Try to read existing key from file
    std::ifstream f(api_key_file_);
    if (f.good()) {
        std::string key;
        f >> key;
        // Valid key: exactly 64 lowercase hex chars
        if (key.size() == 64 &&
            key.find_first_not_of("0123456789abcdef") == std::string::npos) {
            api_key_ = key;
            return api_key_;
        }
    }

    // File missing or invalid — generate and persist a new key
    std::string new_key = gen_random_hex(32); // 32 bytes = 64 hex chars

#ifndef _WIN32
    // Ensure /data/llamaste/ directory exists
    mkdir("/data/llamaste", 0700);
#endif

    std::ofstream out(api_key_file_);
    if (out.good()) {
        out << new_key << "\n";
        out.flush();
    } else {
        fprintf(stderr, "[mcp] Warning: could not write API key to %s\n",
                api_key_file_.c_str());
    }

    api_key_ = new_key;
    fprintf(stderr, "[mcp] API key generated and saved to %s\n", api_key_file_.c_str());
    return api_key_;
}

json McpServer::key_info_locked() const {
    json info;
    if (api_key_.empty()) {
        info["active"] = false;
        info["key_prefix"] = "";
        info["key"] = "";
    } else {
        info["active"]     = true;
        info["key_prefix"] = api_key_.substr(0, 8) + "...";
        info["key"]        = api_key_;  // full key — shown once to user, they copy it
    }
    return info;
}

json McpServer::get_api_key_info() {
    load_or_create_api_key();
    std::lock_guard<std::mutex> lock(api_key_mu_);
    return key_info_locked();
}

json McpServer::regenerate_api_key() {
    std::string new_key = gen_random_hex(32);

    {
        std::lock_guard<std::mutex> lock(api_key_mu_);
        api_key_ = new_key;

#ifndef _WIN32
        mkdir("/data/llamaste", 0700);
#endif
        std::ofstream out(api_key_file_);
        if (out.good()) {
            out << new_key << "\n";
            out.flush();
        } else {
            fprintf(stderr, "[mcp] Warning: could not write regenerated API key to %s\n",
                    api_key_file_.c_str());
        }

        fprintf(stderr, "[mcp] API key regenerated\n");
        return key_info_locked();
    }
}

// ---------------------------------------------------------------------------
// Core POST handler
// ---------------------------------------------------------------------------
void McpServer::handle_post(const httplib::Request& req, httplib::Response& res) {
    ++request_count_;

    res.set_header("Access-Control-Allow-Origin",   "*");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");

    if (req.body.empty()) {
        auto err = rpc_error(nullptr, JSONRPC_INVALID_REQUEST, "Empty request body");
        res.set_content(err.dump(), "application/json");
        return;
    }

    json body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) {
        auto err = rpc_error(nullptr, JSONRPC_PARSE_ERROR, "JSON parse error");
        res.set_content(err.dump(), "application/json");
        return;
    }

    std::string session_id;
    if (req.has_header("Mcp-Session-Id")) {
        session_id = req.get_header_value("Mcp-Session-Id");
        touch_session(session_id);
    }

    // Batch requests
    if (body.is_array()) {
        json batch_response = json::array();
        for (const auto& rpc : body) {
            if (!rpc.is_object()) continue;
            if (!rpc.contains("id")) {
                handle_notification(rpc, session_id);
                continue;
            }
            std::string new_sid;
            json resp = dispatch_rpc(rpc, session_id, new_sid);
            if (!new_sid.empty()) {
                session_id = new_sid;
                res.set_header("Mcp-Session-Id", new_sid);
            }
            batch_response.push_back(resp);
        }
        res.set_content(batch_response.dump(), "application/json");
        return;
    }

    if (!body.is_object()) {
        auto err = rpc_error(nullptr, JSONRPC_INVALID_REQUEST, "Expected JSON object or array");
        res.set_content(err.dump(), "application/json");
        return;
    }

    // Notification
    if (!body.contains("id")) {
        handle_notification(body, session_id);
        res.status = 202;
        return;
    }

    std::string new_session_id;
    json response = dispatch_rpc(body, session_id, new_session_id);
    if (!new_session_id.empty()) {
        res.set_header("Mcp-Session-Id", new_session_id);
    }
    res.set_content(response.dump(), "application/json");
}

// ---------------------------------------------------------------------------
// JSON-RPC dispatch
// ---------------------------------------------------------------------------
json McpServer::dispatch_rpc(const json& request,
                              const std::string& session_id,
                              std::string& out_session_id) {
    if (!request.contains("jsonrpc") || request["jsonrpc"] != "2.0") {
        return rpc_error(request.value("id", json(nullptr)),
                         JSONRPC_INVALID_REQUEST, "Not JSON-RPC 2.0");
    }

    json id     = request.value("id",     json(nullptr));
    json params = request.value("params", json::object());
    std::string method = request.value("method", "");

    if (method.empty()) {
        return rpc_error(id, JSONRPC_INVALID_REQUEST, "Missing 'method' field");
    }

    try {
        if (method == "initialize") {
            return rpc_result(id, handle_initialize(params, out_session_id));
        }
        if (method == "ping") {
            return rpc_result(id, handle_ping(params));
        }
        if (method == "tools/list") {
            return rpc_result(id, handle_tools_list(params));
        }
        if (method == "tools/call") {
            return rpc_result(id, handle_tools_call(params));
        }
        if (method == "resources/list") {
            return rpc_result(id, handle_resources_list(params));
        }
        if (method == "resources/read") {
            return rpc_result(id, handle_resources_read(params));
        }
        if (method == "resources/templates/list") {
            json r;
            r["resourceTemplates"] = json::array();
            return rpc_result(id, r);
        }
        if (method == "prompts/list") {
            return rpc_result(id, handle_prompts_list(params));
        }
        return rpc_error(id, JSONRPC_METHOD_NOT_FOUND,
                         "Method not found: " + method);

    } catch (const std::invalid_argument& e) {
        return rpc_error(id, JSONRPC_INVALID_PARAMS, e.what());
    } catch (const std::exception& e) {
        return rpc_error(id, JSONRPC_INTERNAL_ERROR,
                         std::string("Internal error: ") + e.what());
    } catch (...) {
        return rpc_error(id, JSONRPC_INTERNAL_ERROR, "Unknown internal error");
    }

    (void)session_id;
}

void McpServer::handle_notification(const json& /*request*/,
                                    const std::string& /*session_id*/) {
    // Notifications are fire-and-forget (e.g. notifications/initialized)
}

// ---------------------------------------------------------------------------
// MCP method: initialize
// ---------------------------------------------------------------------------
json McpServer::handle_initialize(const json& params, std::string& out_session_id) {
    json client_info = params.value("clientInfo", json::object());
    out_session_id = create_session(client_info);

    json result;
    result["protocolVersion"] = MCP_PROTOCOL_VERSION;

    json caps;
    caps["tools"]     = json::object();
    caps["resources"] = json::object();
    result["capabilities"] = caps;

    json server_info;
    server_info["name"]    = "llamaste";
    server_info["version"] = LLAMASTE_VERSION;
    result["serverInfo"] = server_info;

    result["instructions"] =
        "Llamaste LLM-OS system management tools. "
        "Manage files, processes, network, configuration, models, and audio on a "
        "self-hosted AI appliance running Llamaste. "
        "All filesystem operations are restricted to /data/. "
        "Destructive operations (delete, shutdown, reboot, factory_reset) require "
        "setting the 'confirm' parameter to true.";

    return result;
}

// ---------------------------------------------------------------------------
// MCP method: ping
// ---------------------------------------------------------------------------
json McpServer::handle_ping(const json& /*params*/) {
    return json::object();
}

// ---------------------------------------------------------------------------
// MCP method: tools/list
// ---------------------------------------------------------------------------
json McpServer::handle_tools_list(const json& /*params*/) {
    json result;
    result["tools"] = build_tools_array();
    return result;
}

// ---------------------------------------------------------------------------
// MCP method: tools/call
// ---------------------------------------------------------------------------
json McpServer::handle_tools_call(const json& params) {
    if (!params.contains("name") || !params["name"].is_string()) {
        throw std::invalid_argument("Missing required parameter: name (string)");
    }

    std::string tool_name = params["name"].get<std::string>();
    json args = params.value("arguments", json::object());

    std::string result_str = tools_.dispatch(tool_name, args.dump());

    json result_json = json::parse(result_str, nullptr, false);
    bool is_error = !result_json.is_discarded()
                 && result_json.is_object()
                 && result_json.contains("error");

    json content_item;
    content_item["type"] = "text";
    content_item["text"] = result_str;

    json result;
    result["content"] = json::array({content_item});
    result["isError"] = is_error;
    return result;
}

// ---------------------------------------------------------------------------
// MCP method: resources/list
// ---------------------------------------------------------------------------
json McpServer::handle_resources_list(const json& /*params*/) {
    json resources = json::array();

    json sys;
    sys["uri"]         = "llamaste://system/status";
    sys["name"]        = "System Status";
    sys["description"] = "Current CPU, RAM, disk, model, and uptime";
    sys["mimeType"]    = "application/json";
    resources.push_back(sys);

    json tools_res;
    tools_res["uri"]         = "llamaste://tools/catalog";
    tools_res["name"]        = "Tool Catalog";
    tools_res["description"] = "Full list of available Llamaste tools with schemas";
    tools_res["mimeType"]    = "application/json";
    resources.push_back(tools_res);

    json result;
    result["resources"] = resources;
    return result;
}

// ---------------------------------------------------------------------------
// MCP method: resources/read
// ---------------------------------------------------------------------------
json McpServer::handle_resources_read(const json& params) {
    if (!params.contains("uri") || !params["uri"].is_string()) {
        throw std::invalid_argument("Missing required parameter: uri (string)");
    }
    std::string uri = params["uri"].get<std::string>();
    std::string content_text;

    if (uri == "llamaste://system/status") {
        content_text = tools_.dispatch("system.info", "{}");
    } else if (uri == "llamaste://tools/catalog") {
        content_text = tools_.to_openai_tools_json();
    } else {
        throw std::invalid_argument("Unknown resource URI: " + uri);
    }

    json content_item;
    content_item["uri"]      = uri;
    content_item["mimeType"] = "application/json";
    content_item["text"]     = content_text;

    json result;
    result["contents"] = json::array({content_item});
    return result;
}

// ---------------------------------------------------------------------------
// MCP method: prompts/list
// ---------------------------------------------------------------------------
json McpServer::handle_prompts_list(const json& /*params*/) {
    json result;
    result["prompts"] = json::array();
    return result;
}

// ---------------------------------------------------------------------------
// Build MCP tools array from ToolRegistry
// ---------------------------------------------------------------------------
json McpServer::build_tools_array() const {
    std::string openai_json = tools_.to_openai_tools_json();
    json openai_tools = json::parse(openai_json, nullptr, false);

    json mcp_tools = json::array();
    if (!openai_tools.is_discarded() && openai_tools.is_array()) {
        for (const auto& entry : openai_tools) {
            if (!entry.is_object() || !entry.contains("function")) continue;
            const auto& func = entry["function"];

            json tool;
            tool["name"]        = func.value("name",        "");
            tool["description"] = func.value("description", "");
            tool["inputSchema"] = func.value("parameters",  json::object());
            mcp_tools.push_back(std::move(tool));
        }
    }
    return mcp_tools;
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------
std::string McpServer::create_session(const json& client_info) {
    expire_old_sessions();

    McpSession sess;
    sess.id             = gen_random_hex(16); // 32-hex session ID
    sess.initialized    = true;
    sess.last_access    = std::chrono::steady_clock::now();
    sess.client_name    = client_info.value("name",    "unknown");
    sess.client_version = client_info.value("version", "");

    std::lock_guard<std::mutex> lock(sessions_mu_);
    std::string id = sess.id;
    sessions_[id] = std::move(sess);
    return id;
}

void McpServer::touch_session(const std::string& id) {
    if (id.empty()) return;
    std::lock_guard<std::mutex> lock(sessions_mu_);
    auto it = sessions_.find(id);
    if (it != sessions_.end()) {
        it->second.last_access = std::chrono::steady_clock::now();
    }
}

void McpServer::expire_old_sessions() {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(sessions_mu_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        auto age_min = std::chrono::duration_cast<std::chrono::minutes>(
            now - it->second.last_access).count();
        it = (age_min > 30) ? sessions_.erase(it) : std::next(it);
    }
}

// ---------------------------------------------------------------------------
// JSON-RPC helpers
// ---------------------------------------------------------------------------
json McpServer::rpc_result(const json& id, const json& result) {
    json response;
    response["jsonrpc"] = "2.0";
    response["id"]      = id;
    response["result"]  = result;
    return response;
}

json McpServer::rpc_error(const json& id, int code, const std::string& message) {
    json err_obj;
    err_obj["code"]    = code;
    err_obj["message"] = message;

    json response;
    response["jsonrpc"] = "2.0";
    response["id"]      = id;
    response["error"]   = err_obj;
    return response;
}

// ---------------------------------------------------------------------------
// Random hex generation
// ---------------------------------------------------------------------------
std::string McpServer::gen_random_hex(int bytes) {
    // Use kernel CSPRNG directly — no predictable PRNG fallback.
    // getrandom() blocks until the entropy pool is seeded.
    std::vector<unsigned char> buf(bytes);
#ifdef __linux__
    ssize_t got = getrandom(buf.data(), (size_t)bytes, 0);
    if (got == (ssize_t)bytes) {
        // Fast path: kernel filled the whole buffer.
    } else {
        // Fallback to /dev/urandom with partial-read loop.
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd < 0) return "";  // can't generate secure bytes
        size_t total = 0;
        while (total < (size_t)bytes) {
            ssize_t n = read(fd, buf.data() + total, (size_t)bytes - total);
            if (n < 0) {
                if (errno == EINTR) continue;
                close(fd);
                return "";
            }
            total += (size_t)n;
        }
        close(fd);
    }
#else
    // Windows: use CryptGenRandom via /dev/urandom equivalent
    // (not reachable in current build target; placeholder)
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return "";
    size_t total = 0;
    while (total < (size_t)bytes) {
        ssize_t n = read(fd, buf.data() + total, (size_t)bytes - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return "";
        }
        total += (size_t)n;
    }
    close(fd);
#endif

    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < bytes; i++) {
        oss << std::setw(2) << (unsigned)buf[i];
    }
    return oss.str();
}
