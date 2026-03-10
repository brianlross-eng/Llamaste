// prompt_builder.cpp — Dynamic system prompt builder for Llamaste LLM-OS
//
// Constructs the system prompt that gives the LLM its identity, knowledge
// of the hardware it's running on, available tools grouped by category,
// and behavioral rules (confirmation for destructive ops, path restrictions).

#include "prompt_builder.h"
#include "json.hpp"
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>

using json = nlohmann::json;

// Group tool names by their category prefix (e.g., "fs", "system", "network")
static std::map<std::string, std::vector<std::string>> group_tools_by_category(
    const ToolRegistry& tools
) {
    std::map<std::string, std::vector<std::string>> groups;
    for (const auto& name : tools.tool_names()) {
        auto dot = name.find('.');
        std::string category = (dot != std::string::npos) ? name.substr(0, dot) : "other";
        groups[category].push_back(name);
    }
    return groups;
}

// Build a compact function signature from an OpenAI tool JSON entry.
// Output: "tool.name(req_param, [opt_param])"
// Used in the system prompt so the model knows parameter names.
static std::string build_signature(const json& tool_entry) {
    std::string name;
    try {
        name = tool_entry.at("function").at("name").get<std::string>();
    } catch (...) {
        return "";
    }

    std::string sig = name + "(";
    bool first = true;

    try {
        const auto& params = tool_entry.at("function").at("parameters");
        if (!params.contains("properties") || !params["properties"].is_object()) {
            return name + "()";
        }

        // Collect required and optional parameter names
        std::vector<std::string> required;
        if (params.contains("required") && params["required"].is_array()) {
            for (const auto& r : params["required"]) {
                if (r.is_string()) required.push_back(r.get<std::string>());
            }
        }

        // Required params first (alphabetical within each group for determinism)
        std::vector<std::string> req_names;
        std::vector<std::string> opt_names;
        for (const auto& [pname, _] : params["properties"].items()) {
            bool is_req = std::find(required.begin(), required.end(), pname) != required.end();
            if (is_req) req_names.push_back(pname);
            else         opt_names.push_back(pname);
        }
        std::sort(req_names.begin(), req_names.end());
        std::sort(opt_names.begin(), opt_names.end());

        for (const auto& p : req_names) {
            if (!first) sig += ", ";
            sig += p;
            first = false;
        }
        for (const auto& p : opt_names) {
            if (!first) sig += ", ";
            sig += "[" + p + "]";
            first = false;
        }
    } catch (...) {
        // Fallback: just the name
        return name + "(...)";
    }

    sig += ")";
    return sig;
}

std::string build_system_prompt(
    const HardwareInfo& hw,
    const ToolRegistry& tools,
    const std::string& boot_mode
) {
    std::ostringstream ss;

    // Identity
    ss << "You are Llamaste, an AI that IS the operating system. ";
    ss << "You are not running on an operating system — you ARE the operating system. ";
    ss << "The Linux kernel handles hardware; you handle everything else: ";
    ss << "file management, system configuration, networking, and user interaction.\n\n";

    // Boot mode
    ss << "## Boot Mode\n";
    if (boot_mode == "desktop") {
        ss << "Running in DESKTOP mode with graphical web UI.\n\n";
    } else {
        ss << "Running in SERVER mode (headless, network-accessible).\n\n";
    }

    // Hardware context
    ss << "## Hardware\n";
    ss << "- CPU: " << hw.cpu_model << " (" << hw.cpu_cores << " cores)\n";
    ss << "- RAM: " << hw.ram_total_mb << " MB total, " << hw.ram_free_mb << " MB free\n";
    if (hw.gpu_detected) {
        ss << "- GPU: " << hw.gpu_name << " (detected)\n";
    }
    if (hw.has_avx2) ss << "- AVX2: supported\n";
    if (hw.has_avx512) ss << "- AVX-512: supported\n";
    ss << "\n";

    // -----------------------------------------------------------------------
    // Tool calling format — Qwen2.5 native <tool_call> XML format.
    // The model was trained to emit tool calls in this exact format.
    // The agent loop parses <tool_call> tags and dispatches them.
    // -----------------------------------------------------------------------
    ss << "## Tool Calling\n";
    ss << "To use a tool, output exactly this format (JSON object inside XML tags):\n";
    ss << "<tool_call>\n";
    ss << "{\"name\": \"category.tool_name\", \"arguments\": {\"param\": \"value\"}}\n";
    ss << "</tool_call>\n\n";
    ss << "Rules:\n";
    ss << "- Call tools ONLY when you need live system data or to perform an action.\n";
    ss << "- Answer from knowledge when possible (no tool call needed).\n";
    ss << "- You can chain multiple tool calls in one response.\n";
    ss << "- The system executes each tool and returns results; you then reply.\n\n";

    // -----------------------------------------------------------------------
    // Available tools by category with compact parameter signatures.
    // Build compact signatures from the OpenAI tools JSON.
    // Format: tool.name(required_param, [optional_param])
    // -----------------------------------------------------------------------
    auto groups = group_tools_by_category(tools);

    // Parse full tool JSON once so we can extract parameter signatures
    json tools_json = json::array();
    {
        std::string tools_str = tools.to_openai_tools_json();
        auto parsed = json::parse(tools_str, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_array()) {
            tools_json = std::move(parsed);
        }
    }

    // Build name→signature map
    std::map<std::string, std::string> sig_map;
    for (const auto& entry : tools_json) {
        try {
            std::string n = entry.at("function").at("name").get<std::string>();
            sig_map[n] = build_signature(entry);
        } catch (...) {}
    }

    ss << "## Available Tools\n";
    for (const auto& [category, tool_names] : groups) {
        ss << "### " << category << "\n";
        for (const auto& name : tool_names) {
            auto it = sig_map.find(name);
            ss << "- " << (it != sig_map.end() ? it->second : name) << "\n";
        }
        ss << "\n";
    }

    // Rules
    ss << "## Rules\n";
    ss << "1. **Confirmation required**: Always ask for user confirmation before executing "
       << "destructive operations: system.shutdown, system.reboot, fs.delete_file. "
       << "Explain what will happen and wait for explicit approval.\n";
    ss << "2. **Path restriction**: All file operations are restricted to /data/. "
       << "Never attempt to read, write, or delete files outside /data/.\n";
    ss << "3. **Be concise**: Give short, direct answers. Use tool calls when the user "
       << "asks about system state rather than guessing.\n";
    ss << "4. **Be helpful**: You are the user's interface to their computer. "
       << "Help them manage files, check system status, configure settings, and understand their system.\n";
    ss << "5. **Error handling**: If a tool call fails, explain the error clearly "
       << "and suggest alternatives.\n";
    ss << "6. **No fabrication**: Do not invent file contents, system statistics, or process information. "
       << "Always use the appropriate tool to get real data.\n";

    return ss.str();
}
