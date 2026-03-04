// tools_auth.cpp -- Authentication management tools
//
// Provides LLM-accessible tools for managing device authentication:
//   auth.change_password   — Change the device password
//   auth.get_api_key       — Retrieve the API key for programmatic access
//   auth.set_session_timeout — Configure session timeout duration

#include "tools.h"
#include "auth.h"
#include "json.hpp"

using json = nlohmann::json;

static AuthManager* g_auth_ptr = nullptr;

// auth.change_password — Change device password (requires current password)
static std::string handle_auth_change_password(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        json err;
        err["error"] = "Invalid JSON arguments";
        return err.dump();
    }

    if (!g_auth_ptr) {
        json err;
        err["error"] = "Auth system not initialized";
        return err.dump();
    }

    std::string current = args.value("current_password", "");
    std::string new_pass = args.value("new_password", "");

    if (current.empty() || new_pass.empty()) {
        json err;
        err["error"] = "Both 'current_password' and 'new_password' are required";
        return err.dump();
    }

    if (new_pass.size() < 4) {
        json err;
        err["error"] = "New password must be at least 4 characters";
        return err.dump();
    }

    // Verify current password
    if (!g_auth_ptr->verify_device_password(current)) {
        json err;
        err["error"] = "Current password is incorrect";
        return err.dump();
    }

    // Set new password
    if (!g_auth_ptr->set_device_password(new_pass)) {
        json err;
        err["error"] = "Failed to set new password";
        return err.dump();
    }

    json result;
    result["success"] = true;
    result["message"] = "Device password changed successfully";
    return result.dump();
}

// auth.get_api_key — Retrieve the auto-generated API key
static std::string handle_auth_get_api_key(const std::string& /*args_json*/) {
    if (!g_auth_ptr) {
        json err;
        err["error"] = "Auth system not initialized";
        return err.dump();
    }

    std::string key = g_auth_ptr->get_api_key();
    if (key.empty()) {
        json err;
        err["error"] = "No API key available. Set a device password first.";
        return err.dump();
    }

    json result;
    result["api_key"] = key;
    result["usage"] = "Use as Bearer token: Authorization: Bearer " + key;
    return result.dump();
}

// auth.set_session_timeout — Configure how long sessions last
static std::string handle_auth_set_session_timeout(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        json err;
        err["error"] = "Invalid JSON arguments";
        return err.dump();
    }

    if (!g_auth_ptr) {
        json err;
        err["error"] = "Auth system not initialized";
        return err.dump();
    }

    int seconds = args.value("seconds", 0);
    if (seconds <= 0) {
        // Try parsing as a human-readable duration
        std::string duration = args.value("duration", "");
        if (duration == "1h" || duration == "1 hour") seconds = 3600;
        else if (duration == "1d" || duration == "1 day") seconds = 86400;
        else if (duration == "7d" || duration == "1 week") seconds = 604800;
        else if (duration == "30d" || duration == "1 month") seconds = 2592000;
        else if (duration == "1y" || duration == "1 year") seconds = 31536000;
        else {
            json err;
            err["error"] = "Provide 'seconds' (int) or 'duration' (e.g. '7d', '1 week')";
            return err.dump();
        }
    }

    g_auth_ptr->set_session_timeout(seconds);

    json result;
    result["success"] = true;
    result["session_timeout_seconds"] = g_auth_ptr->session_timeout_seconds();
    return result.dump();
}

void register_auth_tools(ToolRegistry& reg, AuthManager& auth) {
    g_auth_ptr = &auth;

    reg.register_tool({
        .name = "auth.change_password",
        .description = "Change the device password. Requires the current password for verification.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "current_password": {
                    "type": "string",
                    "description": "The current device password"
                },
                "new_password": {
                    "type": "string",
                    "description": "The new password (minimum 4 characters)"
                }
            },
            "required": ["current_password", "new_password"]
        })json",
        .handler = handle_auth_change_password,
        .requires_confirmation = true
    });

    reg.register_tool({
        .name = "auth.get_api_key",
        .description = "Retrieve the API key for programmatic access via Bearer token authentication.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_auth_get_api_key,
        .requires_confirmation = false
    });

    reg.register_tool({
        .name = "auth.set_session_timeout",
        .description = "Configure how long login sessions last before requiring re-authentication.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "seconds": {
                    "type": "integer",
                    "description": "Session timeout in seconds (minimum 60)"
                },
                "duration": {
                    "type": "string",
                    "description": "Human-readable duration: '1h', '1d', '7d', '30d', '1y'"
                }
            }
        })json",
        .handler = handle_auth_set_session_timeout,
        .requires_confirmation = false
    });
}
