#pragma once
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

// Tool definition: maps a tool name to its metadata and handler function.
// The handler receives a JSON string of arguments and returns a JSON string result.
struct ToolDef {
    std::string name;
    std::string description;
    std::string parameters;  // JSON Schema string
    std::function<std::string(const std::string&)> handler;
    bool requires_confirmation = false;
};

// Central registry for all tools available in the Llamaste LLM-OS.
// Tools are registered at startup and dispatched by the agent loop
// when the LLM generates tool_call responses.
class ToolRegistry {
public:
    // Register a tool definition. Overwrites if name already exists.
    void register_tool(ToolDef tool);

    // Number of registered tools.
    int count() const;

    // Dispatch a tool call by name. Returns JSON result string.
    // Returns {"error":"unknown tool: <name>"} if tool not found.
    std::string dispatch(const std::string& name, const std::string& args_json) const;

    // Generate the OpenAI-compatible tools array for the chat completions API.
    // Each entry has type:"function" with function.name, function.description,
    // function.parameters matching the tool's schema.
    std::string to_openai_tools_json() const;

    // Return sorted list of all registered tool names.
    std::vector<std::string> tool_names() const;

    // Check if a tool requires user confirmation before execution.
    bool needs_confirmation(const std::string& name) const;

private:
    std::unordered_map<std::string, ToolDef> tools_;
};

// Registration functions for each tool category.
// Each function registers its tools into the provided registry.
void register_fs_tools(ToolRegistry& reg);
void register_process_tools(ToolRegistry& reg);
void register_network_tools(ToolRegistry& reg);
void register_system_tools(ToolRegistry& reg);
void register_config_tools(ToolRegistry& reg);
void register_model_tools(ToolRegistry& reg);
void register_install_tools(ToolRegistry& reg);
void register_model_download_tools(ToolRegistry& reg);
void register_audio_tools(ToolRegistry& reg);

void register_update_tools(ToolRegistry& reg);

class ClusterManager;  // forward declaration
void register_cluster_tools(ToolRegistry& reg, ClusterManager& cluster);

class Scheduler;
void register_schedule_tools(ToolRegistry& reg, Scheduler& sched);

class AuthManager;
void register_auth_tools(ToolRegistry& reg, AuthManager& auth);

class WiFiManager;
void register_wifi_tools(ToolRegistry& reg, WiFiManager& wifi);

// Convenience: register all tool categories at once.
void register_all_tools(ToolRegistry& reg);
