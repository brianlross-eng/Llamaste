#pragma once
#include <string>

struct HardwareInfo {
    std::string cpu_model = "unknown";
    int cpu_cores = 1;
    int ram_total_mb = 0;
    int ram_free_mb = 0;
    bool gpu_detected = false;
    std::string gpu_name;
    bool has_avx2 = false;
    bool has_avx512 = false;
};

HardwareInfo detect_hardware();
int read_meminfo_kb(const char* key);
std::string read_sysfs_line(const char* path);
