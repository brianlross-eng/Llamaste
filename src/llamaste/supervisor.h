#pragma once
#include <string>

struct SupervisorConfig {
    std::string model_path;
    std::string boot_mode;    // "server", "desktop", "live"
    int http_port = 80;
    int cpu_cores = 1;
    int ram_total_mb = 0;
    std::string device_name = "llamaste";
};

[[noreturn]] void supervisor_run(const SupervisorConfig& config);

// Format helpers (used by console display thread, testable on host)
std::string format_uptime(long seconds);
std::string format_bar(int percent, int width = 16);
std::string format_bytes(long kb);
