#pragma once
#include <string>

struct SupervisorConfig {
    std::string model_path;
    std::string boot_mode;
    int http_port = 80;
    int cpu_cores = 1;
    int ram_total_mb = 0;
};

[[noreturn]] void supervisor_run(const SupervisorConfig& config);
