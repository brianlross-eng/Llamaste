// tools_update.cpp — A/B update tools for Llamaste
//
// Tools: update.status, update.check, update.install, update.rollback
// Provides the LLM and HTTP API with system update visibility and control.
//
// update.check: fetches latest.json from GitHub to discover new versions
// update.install: parses .update file, verifies Ed25519 + SHA-256, writes partition

#include "tools.h"
#include "updater.h"
#include "version.h"
#include "json.hpp"

#include <fstream>
#include <cstring>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

using json = nlohmann::json;

// Default update URL — points to raw latest.json in the public repo
static const char* DEFAULT_UPDATE_URL =
    "https://raw.githubusercontent.com/brianlross-eng/Llamaste/main/latest.json";

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

// --- libcurl write callback ---
#ifdef HAVE_LIBCURL
static size_t curl_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(static_cast<char*>(ptr), size * nmemb);
    return size * nmemb;
}
#endif

// --- Tool handlers ---

static std::string handle_update_status(const std::string& /*args_json*/) {
    json result;
    std::string active = detect_current_slot();
    result["version"] = LLAMASTE_VERSION;
    result["active_slot"] = active;
    result["inactive_slot"] = inactive_slot(active);
    result["update_state"] = "idle";

    // Check if inactive slot has metadata
    std::string inact = inactive_slot(active);
    std::string meta_path = "/data/llamaste/slots/" + inact + ".json";
    std::ifstream mf(meta_path);
    if (mf.is_open()) {
        try {
            json meta = json::parse(mf);
            result["inactive_version"] = meta.value("version", "");
        } catch (...) {}
    }

    return result.dump();
}

static std::string handle_update_check(const std::string& /*args_json*/) {
#ifndef HAVE_LIBCURL
    json result;
    result["current_version"] = LLAMASTE_VERSION;
    result["available"] = false;
    result["message"] = "libcurl not available — cannot check for updates online. "
                        "Upload a .update file manually.";
    return result.dump();
#else
    // Fetch latest.json from GitHub
    CURL* curl = curl_easy_init();
    if (!curl) {
        return json_error("failed to initialize curl");
    }

    std::string response_body;
    curl_easy_setopt(curl, CURLOPT_URL, DEFAULT_UPDATE_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "llamaste-updater/1.0");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    json result;
    result["current_version"] = LLAMASTE_VERSION;

    if (res != CURLE_OK) {
        result["available"] = false;
        result["error"] = std::string("network error: ") + curl_easy_strerror(res);
        return result.dump();
    }

    if (http_code != 200) {
        result["available"] = false;
        result["error"] = "server returned HTTP " + std::to_string(http_code);
        return result.dump();
    }

    // Parse latest.json
    try {
        auto latest = json::parse(response_body);
        std::string latest_ver = latest.value("version", "");
        if (latest_ver.empty()) {
            result["available"] = false;
            result["error"] = "invalid latest.json: no version field";
            return result.dump();
        }

        result["latest_version"] = latest_ver;

        if (version_compare(latest_ver, LLAMASTE_VERSION) > 0) {
            result["available"] = true;
            result["url"] = latest.value("url", "");
            result["size"] = latest.value("size", 0);
            result["sha256"] = latest.value("sha256", "");
            result["message"] = "Update available: " + latest_ver +
                                " (current: " + std::string(LLAMASTE_VERSION) + ")";
        } else {
            result["available"] = false;
            result["message"] = "System is up to date (version " +
                                std::string(LLAMASTE_VERSION) + ")";
        }
    } catch (const std::exception& e) {
        result["available"] = false;
        result["error"] = std::string("failed to parse latest.json: ") + e.what();
    }

    return result.dump();
#endif
}

static std::string handle_update_install(const std::string& args_json) {
    // Parse path argument
    std::string path;
    try {
        auto args = json::parse(args_json);
        path = args.value("path", "");
    } catch (...) {
        return json_error("invalid arguments — expected {\"path\": \"...\"}");
    }

    if (path.empty()) {
        return json_error("path parameter is required");
    }

    // Check file exists
    std::ifstream f(path);
    if (!f.is_open()) {
        return json_error("file not found: " + path);
    }
    f.close();

    // Delegate to the full install_update() in updater.cpp
    return install_update(path);
}

static std::string handle_update_rollback(const std::string& /*args_json*/) {
    std::string grubenv_path = find_grubenv_path();
    if (grubenv_path.empty()) {
        return json_error("grubenv not found — cannot switch slot. Is the ESP mounted?");
    }

    std::string current = detect_current_slot();
    std::string new_slot = inactive_slot(current);

    auto vars = grubenv_read(grubenv_path);
    vars["active_slot"] = new_slot;
    vars["boot_success"] = "0";
    vars["boot_counter"] = "3";

    if (grubenv_write(grubenv_path, vars)) {
        json result;
        result["success"] = true;
        result["previous_slot"] = current;
        result["new_slot"] = new_slot;
        result["message"] = "Switched to slot " + new_slot + ". Reboot to activate.";
        return result.dump();
    } else {
        return json_error("Failed to write grubenv");
    }
}

void register_update_tools(ToolRegistry& reg) {
    reg.register_tool(ToolDef{
        .name = "update.status",
        .description = "Get system update status: current version, active/inactive boot slot, "
                       "update state, and version on inactive slot if available.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_update_status
    });

    reg.register_tool(ToolDef{
        .name = "update.check",
        .description = "Check for available system updates. "
                       "Fetches latest version info from the update server and compares "
                       "with the currently installed version.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_update_check
    });

    reg.register_tool(ToolDef{
        .name = "update.install",
        .description = "Install a system update from a .update file. "
                       "Validates the file exists, verifies the Ed25519 signature and SHA-256 "
                       "integrity, then writes the update to the inactive partition. "
                       "Requires a reboot to activate.",
        .parameters = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the .update file"}},"required":["path"]})json",
        .handler = handle_update_install,
        .requires_confirmation = true
    });

    reg.register_tool(ToolDef{
        .name = "update.rollback",
        .description = "Roll back to the previous system version by switching the active boot slot. "
                       "Writes boot_counter=3 and boot_success=0 to grubenv, then requires a reboot.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_update_rollback
    });
}
