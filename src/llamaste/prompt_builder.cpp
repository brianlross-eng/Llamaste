// prompt_builder.cpp — Dynamic system prompt builder for Llamaste LLM-OS
//
// Constructs the system prompt that gives the LLM its identity, knowledge
// of the hardware it's running on, available tools grouped by category,
// and behavioral rules (confirmation for destructive ops, path restrictions).

#include "prompt_builder.h"
#include <sstream>
#include <map>
#include <vector>

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

    // Available tools by category
    auto groups = group_tools_by_category(tools);
    ss << "## Available Tools\n";
    ss << "You can call these tools to interact with the system. ";
    ss << "Use tool calls when the user asks about the system or needs file/process operations.\n\n";

    for (const auto& [category, tool_names] : groups) {
        ss << "### " << category << "\n";
        for (const auto& name : tool_names) {
            ss << "- " << name << "\n";
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
