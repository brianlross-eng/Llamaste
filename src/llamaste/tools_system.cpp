// tools_system.cpp — System tools for Llamaste LLM-OS
//
// Provides system information by reading /proc, /sys, and system calls.
// Shutdown and reboot tools require user confirmation and use the
// reboot() syscall (we are PID 1, so this is valid).

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <csignal>

using json = nlohmann::json;

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

static std::string read_file_line(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string line;
    std::getline(f, line);
    return line;
}

static std::string read_file_contents(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------------------
// system.info — CPU, RAM, hostname, kernel version
// ---------------------------------------------------------------------------
static std::string handle_system_info(const std::string& args_json) {
    (void)args_json;

    json result;

    // Kernel info
    struct utsname uts;
    if (uname(&uts) == 0) {
        result["hostname"] = std::string(uts.nodename);
        result["kernel"] = std::string(uts.release);
        result["arch"] = std::string(uts.machine);
        result["os"] = "Llamaste LLM-OS";
    }

    // CPU info from /proc/cpuinfo
    std::string cpuinfo = read_file_contents("/proc/cpuinfo");
    if (!cpuinfo.empty()) {
        // Count processor entries
        int cpu_count = 0;
        std::string cpu_model;
        std::istringstream ss(cpuinfo);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.substr(0, 9) == "processor") {
                cpu_count++;
            } else if (line.substr(0, 10) == "model name" && cpu_model.empty()) {
                auto colon = line.find(':');
                if (colon != std::string::npos) {
                    cpu_model = line.substr(colon + 2);
                }
            }
        }
        if (cpu_count > 0) result["cpu_cores"] = cpu_count;
        if (!cpu_model.empty()) result["cpu_model"] = cpu_model;
    }

    // Memory from /proc/meminfo
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.substr(0, 9) == "MemTotal:") {
                std::istringstream ss(line.substr(10));
                long kb;
                if (ss >> kb) result["ram_total_mb"] = kb / 1024;
            } else if (line.substr(0, 8) == "MemFree:") {
                std::istringstream ss(line.substr(9));
                long kb;
                if (ss >> kb) result["ram_free_mb"] = kb / 1024;
            } else if (line.substr(0, 13) == "MemAvailable:") {
                std::istringstream ss(line.substr(14));
                long kb;
                if (ss >> kb) result["ram_available_mb"] = kb / 1024;
            }
        }
    }

    // Load average
    std::string loadavg = read_file_line("/proc/loadavg");
    if (!loadavg.empty()) {
        result["load_average"] = loadavg;
    }

    return result.dump();
}

// ---------------------------------------------------------------------------
// system.uptime — system uptime
// ---------------------------------------------------------------------------
static std::string handle_system_uptime(const std::string& args_json) {
    (void)args_json;

    struct sysinfo si;
    if (sysinfo(&si) != 0) {
        return json_error("sysinfo failed: " + std::string(strerror(errno)));
    }

    long uptime_secs = si.uptime;
    int days = static_cast<int>(uptime_secs / 86400);
    int hours = static_cast<int>((uptime_secs % 86400) / 3600);
    int minutes = static_cast<int>((uptime_secs % 3600) / 60);
    int seconds = static_cast<int>(uptime_secs % 60);

    json result;
    result["uptime_seconds"] = uptime_secs;
    result["days"] = days;
    result["hours"] = hours;
    result["minutes"] = minutes;
    result["seconds"] = seconds;

    // Human-readable string
    std::string human;
    if (days > 0) human += std::to_string(days) + "d ";
    if (hours > 0) human += std::to_string(hours) + "h ";
    if (minutes > 0) human += std::to_string(minutes) + "m ";
    human += std::to_string(seconds) + "s";
    result["human"] = human;

    return result.dump();
}

// ---------------------------------------------------------------------------
// system.memory — detailed memory usage
// ---------------------------------------------------------------------------
static std::string handle_system_memory(const std::string& args_json) {
    (void)args_json;

    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) {
        return json_error("cannot read /proc/meminfo");
    }

    json result;
    std::string line;
    while (std::getline(f, line)) {
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::istringstream ss(line.substr(colon + 1));
        long value;
        std::string unit;
        if (ss >> value >> unit) {
            // Store in MB for readability
            std::string json_key;
            if (key == "MemTotal") json_key = "total_mb";
            else if (key == "MemFree") json_key = "free_mb";
            else if (key == "MemAvailable") json_key = "available_mb";
            else if (key == "Buffers") json_key = "buffers_mb";
            else if (key == "Cached") json_key = "cached_mb";
            else if (key == "SwapTotal") json_key = "swap_total_mb";
            else if (key == "SwapFree") json_key = "swap_free_mb";
            else if (key == "Shmem") json_key = "shared_mb";
            else if (key == "SReclaimable") json_key = "slab_reclaimable_mb";
            else continue;

            result[json_key] = value / 1024;
        }
    }

    // Compute used = total - free - buffers - cached - sreclaimable
    if (result.contains("total_mb") && result.contains("free_mb")) {
        long total = result["total_mb"].get<long>();
        long free_val = result["free_mb"].get<long>();
        long buffers = result.value("buffers_mb", 0L);
        long cached = result.value("cached_mb", 0L);
        long slab = result.value("slab_reclaimable_mb", 0L);
        result["used_mb"] = total - free_val - buffers - cached - slab;

        if (total > 0) {
            long used = result["used_mb"].get<long>();
            result["used_percent"] = static_cast<int>((used * 100) / total);
        }
    }

    return result.dump();
}

// ---------------------------------------------------------------------------
// system.temperature — CPU temperature
// ---------------------------------------------------------------------------
static std::string handle_system_temperature(const std::string& args_json) {
    (void)args_json;

    json zones = json::array();

    DIR* dir = opendir("/sys/class/thermal");
    if (!dir) {
        // No thermal zones is not an error on some systems
        json result;
        result["zones"] = zones;
        result["count"] = 0;
        result["note"] = "no thermal zones found (virtual machine?)";
        return result.dump();
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name.find("thermal_zone") != 0) continue;

        std::string base = "/sys/class/thermal/" + name;
        json zone;
        zone["zone"] = name;

        // Read type (e.g., "x86_pkg_temp", "acpitz")
        std::string type = read_file_line(base + "/type");
        if (!type.empty()) zone["type"] = type;

        // Read temperature (in millidegrees Celsius)
        std::string temp_str = read_file_line(base + "/temp");
        if (!temp_str.empty()) {
            try {
                long milli_c = std::stol(temp_str);
                zone["temp_celsius"] = milli_c / 1000.0;
            } catch (...) {}
        }

        zones.push_back(zone);
    }
    closedir(dir);

    json result;
    result["zones"] = zones;
    result["count"] = zones.size();
    return result.dump();
}

// ---------------------------------------------------------------------------
// system.shutdown — initiate shutdown (requires confirmation)
// ---------------------------------------------------------------------------
static std::string handle_system_shutdown(const std::string& args_json) {
    (void)args_json;

    // Sync filesystems before shutdown
    sync();

    // Send SIGTERM to supervisor (PID 1) which handles graceful shutdown
    // We can't call reboot() directly — this runs in the child, not PID 1
    if (kill(1, SIGTERM) != 0) {
        return json_error("shutdown failed: " + std::string(strerror(errno)));
    }

    // This should not be reached
    json result;
    result["status"] = "shutdown initiated";
    return result.dump();
}

// ---------------------------------------------------------------------------
// system.reboot — initiate reboot (requires confirmation)
// ---------------------------------------------------------------------------
static std::string handle_system_reboot(const std::string& args_json) {
    (void)args_json;

    // Sync filesystems before reboot
    sync();

    // Send SIGUSR1 to supervisor (PID 1) which handles graceful reboot
    // We can't call reboot() directly — this runs in the child, not PID 1
    if (kill(1, SIGUSR1) != 0) {
        return json_error("reboot failed: " + std::string(strerror(errno)));
    }

    // This should not be reached
    json result;
    result["status"] = "reboot initiated";
    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_system_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "system.info",
        .description = "Get system information including hostname, kernel version, "
                       "CPU model and cores, RAM total/free, architecture, and load average.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_system_info
    });

    reg.register_tool({
        .name = "system.uptime",
        .description = "Get system uptime in seconds and human-readable format "
                       "(days, hours, minutes, seconds).",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_system_uptime
    });

    reg.register_tool({
        .name = "system.memory",
        .description = "Get detailed memory usage including total, free, available, "
                       "buffers, cached, swap, and used percentage.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_system_memory
    });

    reg.register_tool({
        .name = "system.temperature",
        .description = "Read CPU and system temperatures from thermal zones. "
                       "Returns temperature in Celsius for each thermal zone.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_system_temperature
    });

    reg.register_tool({
        .name = "system.shutdown",
        .description = "Shut down the system. Syncs filesystems and powers off. "
                       "WARNING: This will immediately power off the machine.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_system_shutdown,
        .requires_confirmation = true
    });

    reg.register_tool({
        .name = "system.reboot",
        .description = "Reboot the system. Syncs filesystems and restarts. "
                       "WARNING: This will immediately restart the machine.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_system_reboot,
        .requires_confirmation = true
    });
}
