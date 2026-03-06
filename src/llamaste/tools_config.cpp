// tools_config.cpp — Configuration tools for Llamaste LLM-OS
//
// Configuration is stored as individual files under /data/llamaste/config/.
// Each config key maps to a file. Values are stored as plain text.
// This is a simple, reliable, filesystem-based config store.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

using json = nlohmann::json;

static const char* CONFIG_DIR = "/data/llamaste/config";

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

// Ensure the config directory exists, creating it recursively if needed.
static bool ensure_config_dir() {
    struct stat st;
    if (stat(CONFIG_DIR, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }

    // Create /data/llamaste/ first
    mkdir("/data/llamaste", 0755);
    // Create /data/llamaste/config/
    return mkdir(CONFIG_DIR, 0755) == 0 || errno == EEXIST;
}

// Validate config key: alphanumeric, dots, dashes, underscores only.
// No path separators, no spaces, reasonable length.
static bool validate_key(const std::string& key) {
    if (key.empty() || key.size() > 128) return false;
    for (char c : key) {
        if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    }
    return true;
}

static std::string key_to_path(const std::string& key) {
    return std::string(CONFIG_DIR) + "/" + key;
}

// ---------------------------------------------------------------------------
// Default configuration values
// ---------------------------------------------------------------------------
struct ConfigDefault {
    const char* key;
    const char* value;
    const char* description;
};

static const ConfigDefault defaults[] = {
    {"system.hostname", "llamaste", "System hostname"},
    {"system.timezone", "UTC", "System timezone"},
    {"model.path", "", "Override model path (empty = auto-select by RAM)"},
    {"model.context_size", "2048", "LLM context window size"},
    {"model.temperature", "0.7", "LLM sampling temperature"},
    {"model.max_tokens", "512", "Maximum tokens per response"},
    {"server.port", "8080", "HTTP server port"},
    {"server.host", "0.0.0.0", "HTTP server bind address"},
    {"ui.theme", "dark", "Web UI theme (dark/light)"},
    {"log.level", "info", "Logging level (debug/info/warn/error)"},
    {"privacy.retention_days", "90", "Conversation retention in days"},
    {nullptr, nullptr, nullptr}
};

// ---------------------------------------------------------------------------
// config.get — read a config value
// ---------------------------------------------------------------------------
static std::string handle_config_get(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string key = args.value("key", "");
    if (key.empty()) return json_error("key is required");
    if (!validate_key(key)) return json_error("invalid key: only alphanumeric, dots, dashes, underscores allowed");

    std::string path = key_to_path(key);
    std::ifstream f(path);

    json result;
    result["key"] = key;

    if (f.is_open()) {
        std::string value;
        std::getline(f, value);
        result["value"] = value;
        result["source"] = "user";
    } else {
        // Check defaults
        for (const auto* d = defaults; d->key != nullptr; d++) {
            if (key == d->key) {
                result["value"] = std::string(d->value);
                result["source"] = "default";
                result["description"] = std::string(d->description);
                return result.dump();
            }
        }
        result["value"] = nullptr;
        result["source"] = "not_found";
    }

    return result.dump();
}

// ---------------------------------------------------------------------------
// config.set — write a config value
// ---------------------------------------------------------------------------
static std::string handle_config_set(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string key = args.value("key", "");
    if (key.empty()) return json_error("key is required");
    if (!validate_key(key)) return json_error("invalid key: only alphanumeric, dots, dashes, underscores allowed");

    // Value can be string, number, or boolean — convert to string for storage
    std::string value;
    if (args.contains("value")) {
        if (args["value"].is_string()) {
            value = args["value"].get<std::string>();
        } else {
            value = args["value"].dump();
        }
    } else {
        return json_error("value is required");
    }

    // Value length limit
    if (value.size() > 4096) {
        return json_error("value too long (max 4096 bytes)");
    }

    if (!ensure_config_dir()) {
        return json_error("cannot create config directory");
    }

    std::string path = key_to_path(key);
    std::ofstream f(path);
    if (!f.is_open()) {
        return json_error("cannot write config: " + key + " (" + strerror(errno) + ")");
    }
    f << value;
    f.close();

    json result;
    result["key"] = key;
    result["value"] = value;
    result["saved"] = true;
    return result.dump();
}

// ---------------------------------------------------------------------------
// config.list — list all config values
// ---------------------------------------------------------------------------
static std::string handle_config_list(const std::string& args_json) {
    (void)args_json;

    json entries = json::array();

    // First, add defaults
    for (const auto* d = defaults; d->key != nullptr; d++) {
        json entry;
        entry["key"] = std::string(d->key);
        entry["default_value"] = std::string(d->value);
        entry["description"] = std::string(d->description);
        entry["source"] = "default";

        // Check if overridden by user
        std::string path = key_to_path(d->key);
        std::ifstream f(path);
        if (f.is_open()) {
            std::string value;
            std::getline(f, value);
            entry["value"] = value;
            entry["source"] = "user";
        } else {
            entry["value"] = std::string(d->value);
        }
        entries.push_back(entry);
    }

    // Then add any user-defined keys not in defaults
    DIR* dir = opendir(CONFIG_DIR);
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            if (!validate_key(name)) continue;

            // Check if already in defaults
            bool in_defaults = false;
            for (const auto* d = defaults; d->key != nullptr; d++) {
                if (name == d->key) { in_defaults = true; break; }
            }
            if (in_defaults) continue;

            std::string path = key_to_path(name);
            std::ifstream f(path);
            if (f.is_open()) {
                std::string value;
                std::getline(f, value);
                json entry;
                entry["key"] = name;
                entry["value"] = value;
                entry["source"] = "user";
                entries.push_back(entry);
            }
        }
        closedir(dir);
    }

    json result;
    result["config"] = entries;
    result["count"] = entries.size();
    return result.dump();
}

// ---------------------------------------------------------------------------
// config.reset — reset config to defaults (requires confirmation)
// ---------------------------------------------------------------------------
static std::string handle_config_reset(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        // No args = reset all
    }

    std::string key = "";
    if (!args.is_discarded()) {
        key = args.value("key", "");
    }

    if (!key.empty()) {
        // Reset a single key
        if (!validate_key(key)) return json_error("invalid key");
        std::string path = key_to_path(key);
        unlink(path.c_str());

        json result;
        result["key"] = key;
        result["reset"] = true;

        // Report what the default is
        for (const auto* d = defaults; d->key != nullptr; d++) {
            if (key == d->key) {
                result["default_value"] = std::string(d->value);
                break;
            }
        }
        return result.dump();
    }

    // Reset all: delete all files in config dir
    int deleted = 0;
    DIR* dir = opendir(CONFIG_DIR);
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == "..") continue;
            std::string path = std::string(CONFIG_DIR) + "/" + name;
            if (unlink(path.c_str()) == 0) deleted++;
        }
        closedir(dir);
    }

    json result;
    result["reset"] = true;
    result["keys_deleted"] = deleted;
    result["message"] = "All configuration reset to defaults";
    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_config_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "config.get",
        .description = "Read a configuration value by key. Returns the user-set value "
                       "if it exists, otherwise the default value.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "key": {
                    "type": "string",
                    "description": "Configuration key (e.g., 'system.hostname', 'model.temperature')"
                }
            },
            "required": ["key"]
        })json",
        .handler = handle_config_get
    });

    reg.register_tool({
        .name = "config.set",
        .description = "Set a configuration value. The value is persisted to disk "
                       "under /data/llamaste/config/.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "key": {
                    "type": "string",
                    "description": "Configuration key"
                },
                "value": {
                    "type": "string",
                    "description": "Value to set"
                }
            },
            "required": ["key", "value"]
        })json",
        .handler = handle_config_set
    });

    reg.register_tool({
        .name = "config.list",
        .description = "List all configuration values including defaults and "
                       "user-modified settings.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_config_list
    });

    reg.register_tool({
        .name = "config.reset",
        .description = "Reset configuration to defaults. If a key is specified, only "
                       "that key is reset. Without a key, all user config is deleted.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "key": {
                    "type": "string",
                    "description": "Specific key to reset (omit to reset all)"
                }
            }
        })json",
        .handler = handle_config_reset,
        .requires_confirmation = true
    });
}
