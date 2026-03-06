// tools_update.cpp — A/B update tools for Llamaste
//
// Tools: update.status, update.check, update.install, update.rollback
// Provides the LLM and HTTP API with system update visibility and control.

#include "tools.h"
#include "updater.h"
#include "version.h"
#include "json.hpp"

#include <fstream>

using json = nlohmann::json;

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

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
    json result;
    result["current_version"] = LLAMASTE_VERSION;
    result["available"] = false;
    result["message"] = "Online update checking coming soon. Upload a .update file via the web UI.";
    return result.dump();
}

static std::string handle_update_install(const std::string& args_json) {
    // Validate path argument exists
    try {
        auto args = json::parse(args_json);
        std::string path = args.value("path", "");
        if (path.empty()) {
            return json_error("path parameter is required");
        }
        std::ifstream f(path);
        if (!f.is_open()) {
            return json_error("file not found: " + path);
        }
    } catch (...) {
        return json_error("invalid arguments — expected {\"path\": \"...\"}");
    }

    json result;
    result["status"] = "not yet implemented";
    result["message"] = "Update installation will be available in a future release.";
    return result.dump();
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
        .description = "Check for available system updates. Currently returns a stub; "
                       "online update checking will be added in a future release.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_update_check
    });

    reg.register_tool(ToolDef{
        .name = "update.install",
        .description = "Install a system update from a .update file. Validates the file exists. "
                       "Full update installation will be available in a future release.",
        .parameters = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the .update file"}},"required":["path"]})json",
        .handler = handle_update_install
    });

    reg.register_tool(ToolDef{
        .name = "update.rollback",
        .description = "Roll back to the previous system version by switching the active boot slot. "
                       "Writes boot_counter=3 and boot_success=0 to grubenv, then requires a reboot.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_update_rollback
    });
}
