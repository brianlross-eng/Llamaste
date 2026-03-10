// test_stubs.cpp -- Minimal linker stubs for unit test binaries.
//
// Provides empty implementations of ToolRegistry methods and all
// register_*_tools() symbols so that tests that only exercise agent.cpp
// or prompt_builder.cpp functions don't need to link the full tool suite.
//
// Include this file alongside agent.cpp (and optionally prompt_builder.cpp)
// in test compile commands instead of all tools_*.cpp files.

#include "tools.h"

// ToolRegistry members not implemented in tools.h header
std::string ToolRegistry::to_openai_tools_json() const {
    return "[]";
}

std::string ToolRegistry::dispatch(const std::string& /*name*/,
                                   const std::string& /*args_json*/) const {
    return "{\"error\": \"stub\"}";
}

// Suite registration stubs
void register_fs_tools(ToolRegistry&)            {}
void register_process_tools(ToolRegistry&)       {}
void register_network_tools(ToolRegistry&)       {}
void register_system_tools(ToolRegistry&)        {}
void register_config_tools(ToolRegistry&)        {}
void register_model_tools(ToolRegistry&)         {}
void register_install_tools(ToolRegistry&)       {}
void register_model_download_tools(ToolRegistry&){}
void register_audio_tools(ToolRegistry&)         {}
void register_update_tools(ToolRegistry&)        {}
void register_all_tools(ToolRegistry&)           {}
