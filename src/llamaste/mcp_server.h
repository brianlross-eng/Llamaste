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
// Auth: accepts EITHER a valid session cookie (web UI users) OR
//       an Authorization: Bearer <api-key> header (headless clients).
//
// Configure in Claude Desktop (headless, no browser needed):
//   ~/.config/Claude/claude_desktop_config.json (Linux/Mac)
//   %APPDATA%\Claude\claude_desktop_config.json (Windows)
//
//   {
//     "mcpServers": {
//       "llamaste": {
//         "type": "http",
//         "url": "http://llamaste.local/mcp",
//         "headers": { "Authorization": "Bearer <api-key>" }
//       }
//     }
//   }
//
// The API key is shown in the System panel and can be regenerated there.
// It is stored at /data/llamaste/mcp_key.txt and persists across reboots.

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

// Handler type alias (matching httplib conventions)
using McpHandler = std::function<void(const httplib::Request&, httplib::Response&)>;

// Auth check: returns true if the request is from an authenticated user.
// This is called with each request; the implementation checks the session cookie.
using McpAuthCheck = std::function<bool(const httplib::Request&)>;

// ---------------------------------------------------------------------------
// McpServer
// ---------------------------------------------------------------------------
class McpServer {
public:
    // tools: reference to the global ToolRegistry (must outlive McpServer)
    explicit McpServer(const ToolRegistry& tools);

    // Register /mcp (POST + GET + OPTIONS) on svr.
    // auth_check: callable that returns true if the session cookie is valid.
    //   The server builds its own wrapper that also accepts Bearer token auth.
    void add_routes(httplib::Server& svr, McpAuthCheck auth_check);

    // Diagnostic: total POST /mcp requests handled
    uint64_t request_count() const { return request_count_.load(); }

    // ---------------------------------------------------------------------------
    // API key management (called from HTTP management routes in child_main.cpp)
    // ---------------------------------------------------------------------------

    // Returns JSON: {"key":"<full-key>","key_prefix":"<first8>","active":true}
    // First call generates and persists the key if it doesn't exist.
    json get_api_key_info();

    // Generates a new random key, persists it, returns the full key.
    // Returns JSON: {"key":"<full-key>","key_prefix":"<first8>","active":true}
    json regenerate_api_key();

private:
    const ToolRegistry& tools_;
    std::atomic<uint64_t> request_count_{0};

    // ---------------------------------------------------------------------------
    // API key state
    // ---------------------------------------------------------------------------
    std::string api_key_;          // 64-hex-char bearer token (empty until first use)
    std::string api_key_file_;     // "/data/llamaste/mcp_key.txt"
    mutable std::mutex api_key_mu_;
    McpAuthCheck auth_check_;      // Cookie-based auth function from child_main.cpp

    // Load key from file, or generate + persist a new one if missing/invalid
    std::string load_or_create_api_key();
    // Write key to api_key_file_ and update api_key_
    std::string write_new_key();
    // Build the key info JSON (must be called with api_key_mu_ held)
    json key_info_locked() const;

    // ---------------------------------------------------------------------------
    // Session management
    // ---------------------------------------------------------------------------
    struct McpSession {
        std::string id;
        bool initialized = false;
        std::chrono::steady_clock::time_point last_access;
        std::string client_name;
        std::string client_version;
    };

    std::unordered_map<std::string, McpSession> sessions_;
    mutable std::mutex sessions_mu_;

    // ---------------------------------------------------------------------------
    // Request handling
    // ---------------------------------------------------------------------------
    void handle_post(const httplib::Request& req, httplib::Response& res);

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
    json build_tools_array() const;

    std::string create_session(const json& client_info);
    void touch_session(const std::string& id);
    void expire_old_sessions();

    static json rpc_result(const json& id, const json& result);
    static json rpc_error(const json& id, int code, const std::string& message);

    // Generate a random 64-hex-char key (32 bytes entropy)
    static std::string gen_random_hex(int bytes = 32);
};

// Global MCP server instance — created in child_main.cpp, nullptr until
// the HTTP server starts.
extern McpServer* g_mcp;
