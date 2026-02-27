// tools_model.cpp — Model management tools for Llamaste LLM-OS
//
// Manages LLM model files stored in /data/models/. Provides listing,
// info (file size, quantization detection), and current model status.
// Model loading/switching is handled by the supervisor, not here.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

using json = nlohmann::json;

static const char* MODELS_DIR = "/data/models";

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

// Detect quantization type from GGUF filename conventions
static std::string detect_quantization(const std::string& filename) {
    // Common GGUF quantization labels in filenames
    static const char* quant_types[] = {
        "Q2_K", "Q3_K_S", "Q3_K_M", "Q3_K_L",
        "Q4_0", "Q4_1", "Q4_K_S", "Q4_K_M",
        "Q5_0", "Q5_1", "Q5_K_S", "Q5_K_M",
        "Q6_K", "Q8_0", "F16", "F32",
        "IQ1_S", "IQ1_M", "IQ2_XXS", "IQ2_XS", "IQ2_S", "IQ2_M",
        "IQ3_XXS", "IQ3_XS", "IQ3_S",
        "IQ4_NL", "IQ4_XS",
        nullptr
    };

    // Convert filename to uppercase for comparison
    std::string upper = filename;
    for (char& c : upper) c = static_cast<char>(std::toupper(c));

    for (const char** q = quant_types; *q != nullptr; q++) {
        if (upper.find(*q) != std::string::npos) {
            return std::string(*q);
        }
    }
    return "unknown";
}

// Format bytes into human-readable size
static std::string human_size(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ULL * 1024 * 1024) {
        double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f MB", mb);
        return std::string(buf);
    }
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f GB", gb);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// model.list — list available models in /data/models/
// ---------------------------------------------------------------------------
static std::string handle_model_list(const std::string& args_json) {
    (void)args_json;

    DIR* dir = opendir(MODELS_DIR);
    if (!dir) {
        json result;
        result["models"] = json::array();
        result["count"] = 0;
        result["note"] = "models directory not found: " + std::string(MODELS_DIR);
        return result.dump();
    }

    json models = json::array();
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        // Only show GGUF files (the standard llama.cpp format)
        bool is_gguf = false;
        if (name.size() > 5) {
            std::string ext = name.substr(name.size() - 5);
            for (char& c : ext) c = static_cast<char>(std::tolower(c));
            if (ext == ".gguf") is_gguf = true;
        }

        std::string full_path = std::string(MODELS_DIR) + "/" + name;
        struct stat st;

        json model;
        model["filename"] = name;
        model["path"] = full_path;

        if (stat(full_path.c_str(), &st) == 0) {
            model["size_bytes"] = static_cast<uint64_t>(st.st_size);
            model["size_human"] = human_size(static_cast<uint64_t>(st.st_size));
            model["modified"] = static_cast<int64_t>(st.st_mtime);
        }

        if (is_gguf) {
            model["format"] = "GGUF";
            model["quantization"] = detect_quantization(name);
        } else {
            model["format"] = "other";
        }

        models.push_back(model);
    }
    closedir(dir);

    json result;
    result["models"] = models;
    result["count"] = models.size();
    result["directory"] = MODELS_DIR;
    return result.dump();
}

// ---------------------------------------------------------------------------
// model.info — get model file size and metadata
// ---------------------------------------------------------------------------
static std::string handle_model_info(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string filename = args.value("filename", "");
    if (filename.empty()) return json_error("filename is required");

    // Security: prevent path traversal
    if (filename.find('/') != std::string::npos || filename.find("..") != std::string::npos) {
        return json_error("invalid filename: must not contain path separators");
    }

    std::string path = std::string(MODELS_DIR) + "/" + filename;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return json_error("model not found: " + filename);
    }

    json result;
    result["filename"] = filename;
    result["path"] = path;
    result["size_bytes"] = static_cast<uint64_t>(st.st_size);
    result["size_human"] = human_size(static_cast<uint64_t>(st.st_size));
    result["modified"] = static_cast<int64_t>(st.st_mtime);

    // Detect format and quantization
    std::string lower_name = filename;
    for (char& c : lower_name) c = static_cast<char>(std::tolower(c));
    if (lower_name.size() > 5 && lower_name.substr(lower_name.size() - 5) == ".gguf") {
        result["format"] = "GGUF";
        result["quantization"] = detect_quantization(filename);

        // Try to read GGUF magic bytes to verify
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            char magic[4];
            f.read(magic, 4);
            if (f.gcount() == 4) {
                // GGUF magic: "GGUF" = 0x46475547
                if (magic[0] == 'G' && magic[1] == 'G' && magic[2] == 'U' && magic[3] == 'F') {
                    result["valid_gguf"] = true;

                    // Read version (uint32_t little-endian)
                    uint32_t version = 0;
                    f.read(reinterpret_cast<char*>(&version), 4);
                    if (f.gcount() == 4) {
                        result["gguf_version"] = version;
                    }
                } else {
                    result["valid_gguf"] = false;
                }
            }
        }
    } else {
        result["format"] = "other";
    }

    // Estimate RAM required (rough: file size * 1.2 for KV cache overhead)
    double ram_estimate_gb = (static_cast<double>(st.st_size) * 1.2) / (1024.0 * 1024.0 * 1024.0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f GB", ram_estimate_gb);
    result["estimated_ram"] = std::string(buf);

    return result.dump();
}

// ---------------------------------------------------------------------------
// model.current — show currently loaded model
// ---------------------------------------------------------------------------

// Global pointer to current model path, set by the supervisor when loading.
// This is extern and will be defined in the supervisor module.
namespace llamaste {
    extern const char* current_model_path;
}

// Provide a default definition for testing / standalone compilation.
// The supervisor will define the real one.
namespace llamaste {
    __attribute__((weak)) const char* current_model_path = nullptr;
}

static std::string handle_model_current(const std::string& args_json) {
    (void)args_json;

    json result;
    if (llamaste::current_model_path && llamaste::current_model_path[0] != '\0') {
        std::string path = llamaste::current_model_path;
        result["path"] = path;
        result["loaded"] = true;

        // Extract filename from path
        auto slash = path.rfind('/');
        if (slash != std::string::npos) {
            result["filename"] = path.substr(slash + 1);
        } else {
            result["filename"] = path;
        }

        // Get file size
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            result["size_bytes"] = static_cast<uint64_t>(st.st_size);
            result["size_human"] = human_size(static_cast<uint64_t>(st.st_size));
        }

        result["quantization"] = detect_quantization(result.value("filename", ""));
    } else {
        result["loaded"] = false;
        result["message"] = "No model currently loaded";
    }

    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_model_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "model.list",
        .description = "List all available LLM model files in /data/models/. Shows "
                       "filename, size, format, and quantization type for each model.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_model_list
    });

    reg.register_tool({
        .name = "model.info",
        .description = "Get detailed information about a specific model file including "
                       "size, format, quantization, GGUF version, and estimated RAM requirement.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "filename": {
                    "type": "string",
                    "description": "Model filename (e.g., 'qwen2.5-7b-instruct-q4_k_m.gguf')"
                }
            },
            "required": ["filename"]
        })json",
        .handler = handle_model_info
    });

    reg.register_tool({
        .name = "model.current",
        .description = "Show information about the currently loaded LLM model, "
                       "including path, filename, size, and quantization.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_model_current
    });
}
