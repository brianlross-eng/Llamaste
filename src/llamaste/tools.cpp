// tools.cpp — ToolRegistry implementation for Llamaste LLM-OS
//
// The registry maps tool names to handler functions. The agent loop
// queries to_openai_tools_json() to tell the LLM what tools exist,
// then calls dispatch() when the LLM emits a tool_call.

#include "tools.h"
#include "json.hpp"
#include <algorithm>
#include <stdexcept>

using json = nlohmann::json;

void ToolRegistry::register_tool(ToolDef tool) {
    std::string name = tool.name;
    tools_[name] = std::move(tool);
}

int ToolRegistry::count() const {
    return static_cast<int>(tools_.size());
}

std::string ToolRegistry::dispatch(const std::string& name, const std::string& args_json) const {
    return dispatch(name, args_json, false);
}

std::string ToolRegistry::dispatch(const std::string& name, const std::string& args_json, bool confirmed) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) {
        json err;
        err["error"] = "unknown tool: " + name;
        return err.dump();
    }

    if (!confirmed && it->second.requires_confirmation) {
        json err;
        err["error"] = "confirmation required for tool: " + name;
        err["confirm_required"] = true;
        return err.dump();
    }

    try {
        return it->second.handler(args_json);
    } catch (const std::exception& e) {
        json err;
        err["error"] = std::string("tool execution failed: ") + e.what();
        return err.dump();
    } catch (...) {
        json err;
        err["error"] = "tool execution failed: unknown error";
        return err.dump();
    }
}

std::string ToolRegistry::to_openai_tools_json() const {
    json tools_array = json::array();

    // Sort tool names for deterministic output
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& [name, _] : tools_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        const auto& tool = tools_.at(name);

        json func;
        func["name"] = tool.name;
        func["description"] = tool.description;

        // Parse the parameters schema string into a JSON object
        auto params = json::parse(tool.parameters, nullptr, false);
        if (params.is_discarded()) {
            // Fallback: empty object schema
            params = json::object();
            params["type"] = "object";
            params["properties"] = json::object();
        }
        func["parameters"] = params;

        json entry;
        entry["type"] = "function";
        entry["function"] = func;

        tools_array.push_back(entry);
    }

    return tools_array.dump();
}

std::vector<std::string> ToolRegistry::tool_names() const {
    std::vector<std::string> names;
    names.reserve(tools_.size());
    for (const auto& [name, _] : tools_) {
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool ToolRegistry::needs_confirmation(const std::string& name) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) return false;
    return it->second.requires_confirmation;
}

void register_all_tools(ToolRegistry& reg) {
    register_fs_tools(reg);
    register_process_tools(reg);
    register_network_tools(reg);
    register_system_tools(reg);
    register_config_tools(reg);
    register_model_tools(reg);
    register_model_download_tools(reg);
}
