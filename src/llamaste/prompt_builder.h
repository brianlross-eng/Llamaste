// prompt_builder.h — Dynamic system prompt builder for Llamaste LLM-OS
//
// Builds a system prompt that tells the LLM about itself, the hardware
// it's running on, available tools, and behavioral rules.

#pragma once
#include "hwdetect.h"
#include "tools.h"
#include <string>

// Build the system prompt with hardware context, boot mode, and available tools.
// This is called once at startup and set on the ConversationState.
std::string build_system_prompt(
    const HardwareInfo& hw,
    const ToolRegistry& tools,
    const std::string& boot_mode
);
