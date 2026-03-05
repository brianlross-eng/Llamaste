#pragma once
// mcp_server.h — Model Context Protocol server for Llamaste
//
// Implements MCP Streamable HTTP transport (spec 2025-03-26).
// Exposes all registered Llamaste tools as MCP tools, allowing
// Claude Desktop and other MCP clients to invoke system management
// capabilities directly via the MCP protocol.
//
// Endpoint: POST /mcp  (on the main HTTP server, port 80)
//
// Configure in Claude Desktop's config:
//   ~/.config/Claude/claude_desktop_config.json (Linux/Mac)
//   %APPDATA%\Claude\claude_desktop_config.json (Windows)
//
//   {
//     "mcpServers": {
//       "llamaste": {
//         "type": "http",
//         "url": "http://llamaste.local/mcp",
//         "headers": { "Cookie": "session=<your-session-token>" }
//       }
//     }
//   }
//
// Session tokens can be found in the browser DevTools after logging in.
// A dedicated MCP API key (future feature) will simplify headless auth.

#include "tools.h"
#include "httplib.h"
#include "json.hpp"

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>

using json = nlohmann::json;

// MCP protocol version this server speaks
static constexpr const char* MCP_PROTOCOL_VERSION = "2025-03-26";

// Handler type aliases (matching httplib conventions)
using McpHandler     = std::function<void(const httplib::Request&, httplib::Response&)>;
using McpAuthWrapper = std::function<McpHandler(McpHandler)>;

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------
// Stateful MCP server.  A single instance is created in child_main and
// kept alive for the lifetime of the process.  All member functions are
// thread-safe (httplib serves requests concurrently).
class McpServer {
public:
    // tools: reference to the global ToolRegistry (must outlive McpServer)
    explicit McpServer(const ToolRegistry& tools);

    // Register /mcp (POST + GET + OPTIONS) on svr.
    // require_auth: an auth-wrapping callable — the same lambda used for
    //   all other protected routes in child_main.cpp.
    void add_routes(httplib::Server& svr, McpAuthWrapper require_auth);

    // Diagnostic: total POST /mcp requests handled
    uint64_t request_count() const { return request_count_.load(); }

private:
    const ToolRegistry& tools_;
    std::atomic<uint64_t> request_count_{0};

    // ---------------------------------------------------------------------------
    // Session management
    // ---------------------------------------------------------------------------
    struct McpSession {
        std::string id;
        bool initialized = false;
        std::chrono::steady_clock::time_point last_access;
        std::string client_name;    // from clientInfo.name
        std::string client_version; // from clientInfo.version
    };

    std::unordered_map<std::string, McpSession> sessions_;
    mutable std::mutex sessions_mu_;

    // ---------------------------------------------------------------------------
    // Request handling
    // ---------------------------------------------------------------------------
    void handle_post(const httplib::Request& req, httplib::Response& res);

    // Dispatch a single JSON-RPC request.
    // out_session_id is set by handle_initialize() so the caller can include
    // the Mcp-Session-Id header in the HTTP response.
    json dispatch_rpc(const json& request,
                      const std::string& session_id,
                      std::string& out_session_id);

    // ---------------------------------------------------------------------------
    // MCP method handlers
    // ---------------------------------------------------------------------------
    json handle_initialize(const json& params, std::string& out_session_id);
    json handle_ping(const json& params);
    json handle_tools_list(const json& params);
    json handle_tools_call(const json& params);
    json handle_resources_list(const json& params);
    json handle_resources_read(const json& params);
    json handle_prompts_list(const json& params);
    void handle_notification(const json& request, const std::string& session_id);

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    // Build the MCP tools array from the ToolRegistry.
    // Parses the OpenAI-format tool schemas and reformats for MCP.
    json build_tools_array() const;

    // Session helpers
    std::string create_session(const json& client_info);
    void touch_session(const std::string& id);
    void expire_old_sessions();

    // JSON-RPC response builders
    static json rpc_result(const json& id, const json& result);
    static json rpc_error(const json& id, int code, const std::string& message);

    // Generate a random 32-hex-char session ID
    static std::string gen_session_id();
};

// Global MCP server instance — created in child_main.cpp, nullptr until
// the HTTP server starts.
extern McpServer* g_mcp;
