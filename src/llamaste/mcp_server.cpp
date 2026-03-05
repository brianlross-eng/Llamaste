// mcp_server.cpp — Model Context Protocol server for Llamaste
//
// Implements the MCP Streamable HTTP transport (spec 2025-03-26).
// All Llamaste tools are exposed as MCP tools.
//
// Protocol flow (Streamable HTTP):
//   1. MCP client sends POST /mcp with {"method":"initialize",...}
//   2. Server responds with capabilities + Mcp-Session-Id header
//   3. Client sends subsequent requests with the session ID header
//   4. tools/list   → list of all Llamaste tools
//   5. tools/call   → dispatch to ToolRegistry.dispatch()
//
// See mcp_server.h for configuration instructions.

#include "mcp_server.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <random>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

using json = nlohmann::json;

// Global instance (created and owned by child_main.cpp)
McpServer* g_mcp = nullptr;

// ---------------------------------------------------------------------------
// JSON-RPC 2.0 standard error codes
// ---------------------------------------------------------------------------
static constexpr int JSONRPC_PARSE_ERROR      = -32700;
static constexpr int JSONRPC_INVALID_REQUEST  = -32600;
static constexpr int JSONRPC_METHOD_NOT_FOUND = -32601;
static constexpr int JSONRPC_INVALID_PARAMS   = -32602;
static constexpr int JSONRPC_INTERNAL_ERROR   = -32603;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
McpServer::McpServer(const ToolRegistry& tools) : tools_(tools) {}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------
void McpServer::add_routes(httplib::Server& svr, McpAuthWrapper require_auth) {
    auto self = this;

    // ----- POST /mcp — main MCP endpoint -----
    svr.Post("/mcp", require_auth([self](const httplib::Request& req, httplib::Response& res) {
        self->handle_post(req, res);
    }));

    // ----- GET /mcp — discovery / health -----
    svr.Get("/mcp", require_auth([](const httplib::Request& /*req*/, httplib::Response& res) {
        json info;
        info["server"]    = "llamaste";
        info["version"]   = "0.1.0";
        info["protocol"]  = MCP_PROTOCOL_VERSION;
        info["transport"] = "streamable-http";
        info["endpoint"]  = "/mcp";
        info["docs"]      = "POST JSON-RPC 2.0 to /mcp to connect an MCP client";
        res.set_content(info.dump(), "application/json");
    }));

    // ----- OPTIONS /mcp — CORS preflight (browser-based MCP clients) -----
    svr.Options("/mcp", [](const httplib::Request& /*req*/, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin",  "*");
        res.set_header("Access-Control-Allow-Methods", "POST, GET, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers",
                       "Content-Type, Mcp-Session-Id, Authorization, Accept");
        res.status = 204;
    });
}

// ---------------------------------------------------------------------------
// Core POST handler
// ---------------------------------------------------------------------------
void McpServer::handle_post(const httplib::Request& req, httplib::Response& res) {
    ++request_count_;

    // CORS headers so browser clients can reach us
    res.set_header("Access-Control-Allow-Origin",   "*");
    res.set_header("Access-Control-Expose-Headers", "Mcp-Session-Id");

    // Parse incoming JSON-RPC body
    if (req.body.empty()) {
        auto err = rpc_error(nullptr, JSONRPC_INVALID_REQUEST, "Empty request body");
        res.set_content(err.dump(), "application/json");
        return;
    }

    json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (body.is_discarded()) {
        auto err = rpc_error(nullptr, JSONRPC_PARSE_ERROR, "JSON parse error");
        res.set_content(err.dump(), "application/json");
        return;
    }

    // Session ID from request header (may be empty for initialize)
    std::string session_id;
    if (req.has_header("Mcp-Session-Id")) {
        session_id = req.get_header_value("Mcp-Session-Id");
        touch_session(session_id);
    }

    // Handle batch requests (array of JSON-RPC objects)
    if (body.is_array()) {
        json batch_response = json::array();
        for (const auto& rpc : body) {
            if (!rpc.is_object()) continue;

            if (!rpc.contains("id")) {
                // Notification — no response
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

    // Single request
    if (!body.is_object()) {
        auto err = rpc_error(nullptr, JSONRPC_INVALID_REQUEST, "Expected JSON object or array");
        res.set_content(err.dump(), "application/json");
        return;
    }

    // Notification (no id field) — client doesn't expect a response body
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
    // Validate jsonrpc field
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
        // Unrecognised method
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
}

void McpServer::handle_notification(const json& request,
                                    const std::string& session_id) {
    // Notifications are fire-and-forget; we just acknowledge them silently.
    std::string method = request.value("method", "");
    (void)method;
    (void)session_id;
    // Future: handle notifications/cancelled to abort long-running tool calls
}

// ---------------------------------------------------------------------------
// MCP method: initialize
// ---------------------------------------------------------------------------
json McpServer::handle_initialize(const json& params, std::string& out_session_id) {
    // Create a new session for this client
    json client_info = params.value("clientInfo", json::object());
    out_session_id = create_session(client_info);

    // Build the initialize response
    json result;
    result["protocolVersion"] = MCP_PROTOCOL_VERSION;

    // Server capabilities
    json caps;
    caps["tools"]     = json::object();  // tools/list + tools/call
    caps["resources"] = json::object();  // resources/list + resources/read
    // No logging, sampling, or experimental features
    result["capabilities"] = caps;

    // Server identity
    json server_info;
    server_info["name"]    = "llamaste";
    server_info["version"] = "0.1.0";
    result["serverInfo"] = server_info;

    // Instructions shown to the LLM using this server
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
    return json::object();  // Empty object per spec
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

    // Dispatch to the ToolRegistry
    std::string result_str = tools_.dispatch(tool_name, args.dump());

    // Check whether the result is an error JSON
    json result_json = json::parse(result_str, nullptr, false);
    bool is_error = !result_json.is_discarded()
                 && result_json.is_object()
                 && result_json.contains("error");

    // Return MCP content array
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

    // System status snapshot
    json sys;
    sys["uri"]         = "llamaste://system/status";
    sys["name"]        = "System Status";
    sys["description"] = "Current CPU, RAM, disk, model, and uptime";
    sys["mimeType"]    = "application/json";
    resources.push_back(sys);

    // Tool catalog
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
        // Use system.info tool
        std::string info = tools_.dispatch("system.info", "{}");
        content_text = info;
    } else if (uri == "llamaste://tools/catalog") {
        // Return OpenAI-format tool schemas for inspection
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
    // Parse OpenAI tool schemas from the registry and reformat for MCP.
    // OpenAI format: [{"type":"function","function":{"name":..., "description":..., "parameters":...}}]
    // MCP format:    [{"name":..., "description":..., "inputSchema":...}]
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
    sess.id            = gen_session_id();
    sess.initialized   = true;
    sess.last_access   = std::chrono::steady_clock::now();
    sess.client_name   = client_info.value("name",    "unknown");
    sess.client_version= client_info.value("version", "");

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
    json error_obj;
    error_obj["code"]    = code;
    error_obj["message"] = message;

    json response;
    response["jsonrpc"] = "2.0";
    response["id"]      = id;
    response["error"]   = error_obj;
    return response;
}

std::string McpServer::gen_session_id() {
    // 32 random hex chars = 128 bits of entropy
    static std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    static std::mutex rng_mu;

    std::lock_guard<std::mutex> lock(rng_mu);
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(16) << dist(rng)
        << std::setw(16) << dist(rng);
    return oss.str();
}
