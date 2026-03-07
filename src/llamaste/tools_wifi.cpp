// tools_wifi.cpp — WiFi management tools for Llamaste
//
// Tools: wifi.status, wifi.scan, wifi.connect, wifi.disconnect,
//        wifi.list, wifi.forget, wifi.enable
//
// All tools are safe to call when WiFi hardware is absent — they return
// a clear error rather than crashing.

#include "tools.h"
#include "wifi.h"
#include "json.hpp"

using json = nlohmann::json;

static WiFiManager* g_wifi = nullptr;

static std::string json_ok(json& result) {
    result["success"] = true;
    return result.dump();
}

static std::string json_error(const std::string& msg) {
    json err;
    err["success"] = false;
    err["error"]   = msg;
    return err.dump();
}

// ---------------------------------------------------------------------------
// wifi.status — current connection state
// ---------------------------------------------------------------------------

static std::string handle_wifi_status(const std::string& /*args_json*/) {
    if (!g_wifi) return json_error("wifi not initialized");

    WiFiStatus s = g_wifi->status();

    json result;
    result["available"]       = s.available;
    result["daemon_running"]  = s.daemon_running;
    result["iface"]           = s.iface;
    result["state"]           = s.state.empty() ? "DISCONNECTED" : s.state;
    result["connected"]       = (s.state == "COMPLETED");
    result["ssid"]            = s.ssid;
    result["bssid"]           = s.bssid;
    result["ip_addr"]         = s.ip_addr;
    result["signal_dbm"]      = s.signal_dbm;
    return result.dump();
}

// ---------------------------------------------------------------------------
// wifi.scan — passive scan for visible networks
// ---------------------------------------------------------------------------

static std::string handle_wifi_scan(const std::string& /*args_json*/) {
    if (!g_wifi) return json_error("wifi not initialized");
    if (!g_wifi->has_wifi()) return json_error("No WiFi interface detected");

    auto nets = g_wifi->scan();

    json result;
    result["success"] = true;
    result["count"]   = (int)nets.size();
    json arr = json::array();
    for (const auto& n : nets) {
        json net;
        net["ssid"]       = n.ssid;
        net["bssid"]      = n.bssid;
        net["signal_dbm"] = n.signal_dbm;
        net["freq_mhz"]   = n.freq_mhz;
        net["security"]   = n.security;
        net["saved"]      = n.saved;
        arr.push_back(net);
    }
    result["networks"] = arr;
    return result.dump();
}

// ---------------------------------------------------------------------------
// wifi.connect — connect to a network
// ---------------------------------------------------------------------------

static std::string handle_wifi_connect(const std::string& args_json) {
    if (!g_wifi) return json_error("wifi not initialized");
    if (!g_wifi->has_wifi()) return json_error("No WiFi interface detected");

    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string ssid = args.value("ssid", "");
    std::string psk  = args.value("psk",  "");

    if (ssid.empty()) return json_error("ssid is required");

    std::string err = g_wifi->connect(ssid, psk);
    if (!err.empty()) return json_error(err);

    json result;
    result["success"] = true;
    result["message"] = "Connected to " + ssid;
    // Return the IP address we got
    WiFiStatus s = g_wifi->status();
    result["ip_addr"] = s.ip_addr;
    return result.dump();
}

// ---------------------------------------------------------------------------
// wifi.disconnect — disconnect from current network
// ---------------------------------------------------------------------------

static std::string handle_wifi_disconnect(const std::string& /*args_json*/) {
    if (!g_wifi) return json_error("wifi not initialized");
    if (!g_wifi->has_wifi()) return json_error("No WiFi interface detected");

    std::string err = g_wifi->disconnect();
    if (!err.empty()) return json_error(err);

    json result;
    result["success"] = true;
    result["message"] = "Disconnected";
    return result.dump();
}

// ---------------------------------------------------------------------------
// wifi.list — list saved networks
// ---------------------------------------------------------------------------

static std::string handle_wifi_list(const std::string& /*args_json*/) {
    if (!g_wifi) return json_error("wifi not initialized");
    if (!g_wifi->has_wifi()) return json_error("No WiFi interface detected");

    auto nets = g_wifi->list_networks();

    json result;
    result["success"] = true;
    result["count"]   = (int)nets.size();
    json arr = json::array();
    for (const auto& n : nets) {
        json net;
        net["network_id"] = n.network_id;
        net["ssid"]       = n.ssid;
        net["connected"]  = n.connected;
        arr.push_back(net);
    }
    result["networks"] = arr;
    return result.dump();
}

// ---------------------------------------------------------------------------
// wifi.forget — remove a saved network
// ---------------------------------------------------------------------------

static std::string handle_wifi_forget(const std::string& args_json) {
    if (!g_wifi) return json_error("wifi not initialized");
    if (!g_wifi->has_wifi()) return json_error("No WiFi interface detected");

    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string ssid = args.value("ssid", "");
    if (ssid.empty()) return json_error("ssid is required");

    std::string err = g_wifi->forget(ssid);
    if (!err.empty()) return json_error(err);

    json result;
    result["success"] = true;
    result["message"] = "Removed saved network: " + ssid;
    return result.dump();
}

// ---------------------------------------------------------------------------
// wifi.enable — bring WiFi interface up or down
// ---------------------------------------------------------------------------

static std::string handle_wifi_enable(const std::string& args_json) {
    if (!g_wifi) return json_error("wifi not initialized");
    if (!g_wifi->has_wifi()) return json_error("No WiFi interface detected");

    auto args = json::parse(args_json, nullptr, false);
    bool enable = true;
    if (!args.is_discarded()) {
        enable = args.value("enable", true);
    }

    std::string err = enable ? g_wifi->enable() : g_wifi->disable();
    if (!err.empty()) return json_error(err);

    json result;
    result["success"] = true;
    result["message"] = enable ? "WiFi enabled" : "WiFi disabled";
    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_wifi_tools(ToolRegistry& reg, WiFiManager& wifi) {
    g_wifi = &wifi;

    reg.register_tool({
        .name        = "wifi.status",
        .description = "Get current WiFi connection status, including state "
                       "(DISCONNECTED/COMPLETED), SSID, IP address, and signal strength.",
        .parameters  = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler     = handle_wifi_status,
    });

    reg.register_tool({
        .name        = "wifi.scan",
        .description = "Scan for visible WiFi networks. Returns a list of SSIDs "
                       "with signal strength and security type. Takes ~3-4 seconds.",
        .parameters  = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler     = handle_wifi_scan,
    });

    reg.register_tool({
        .name        = "wifi.connect",
        .description = "Connect to a WiFi network by SSID and passphrase. "
                       "Leave psk empty for open networks. Saves the network for reconnect.",
        .parameters  = R"json({
            "type": "object",
            "properties": {
                "ssid": {
                    "type": "string",
                    "description": "Network name (SSID)"
                },
                "psk": {
                    "type": "string",
                    "description": "WiFi passphrase (leave empty for open networks)"
                }
            },
            "required": ["ssid"]
        })json",
        .handler     = handle_wifi_connect,
    });

    reg.register_tool({
        .name        = "wifi.disconnect",
        .description = "Disconnect from the current WiFi network.",
        .parameters  = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler     = handle_wifi_disconnect,
    });

    reg.register_tool({
        .name        = "wifi.list",
        .description = "List saved (remembered) WiFi networks.",
        .parameters  = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler     = handle_wifi_list,
    });

    reg.register_tool({
        .name        = "wifi.forget",
        .description = "Remove a saved WiFi network so it will not reconnect automatically.",
        .parameters  = R"json({
            "type": "object",
            "properties": {
                "ssid": {
                    "type": "string",
                    "description": "SSID of the network to forget"
                }
            },
            "required": ["ssid"]
        })json",
        .handler     = handle_wifi_forget,
    });

    reg.register_tool({
        .name        = "wifi.enable",
        .description = "Enable or disable the WiFi interface. "
                       "Set enable=false to turn WiFi off, true (default) to turn it on.",
        .parameters  = R"json({
            "type": "object",
            "properties": {
                "enable": {
                    "type": "boolean",
                    "description": "true to enable WiFi, false to disable"
                }
            }
        })json",
        .handler     = handle_wifi_enable,
    });
}
