// tools_model_download.h — Public interface for model download helpers
//
// Exposes the built-in model table and URL builders used by both
// the MCP tools and the cluster auto-upgrade logic.

#pragma once
#include <string>

// Record in the built-in Qwen2.5-Instruct model table.
struct ModelInfo {
    const char* name;
    const char* filename;
    const char* repo_id;
    int required_mb;       // Min RAM in MB to run this model
    int approx_size_mb;    // Approximate download size in MB
};

// Returns a pointer to the static model table (null-terminated).
const ModelInfo* get_model_table();

// Returns the best model entry for available_mb of RAM, or nullptr.
const ModelInfo* recommend_model(int available_mb);

// Build the direct download URL for a HuggingFace GGUF file.
std::string build_hf_download_url(const std::string& repo_id,
                                   const std::string& filename);
