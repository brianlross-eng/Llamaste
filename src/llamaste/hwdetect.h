#pragma once
#include <string>
#include "cpu_topology.h"

struct HardwareInfo {
    std::string cpu_model = "unknown";
    int cpu_cores = 1;              // Logical cores (threads) — kept for backward compat
    int physical_cores = 0;         // Physical cores (deduplicated)
    int ram_total_mb = 0;
    int ram_free_mb = 0;
    bool gpu_detected = false;
    std::string gpu_name;
    std::string gpu_driver;         // kernel driver name (i915, amdgpu, nouveau, simpledrm)
    bool has_avx2 = false;
    bool has_avx512 = false;
    // CPU topology (hybrid P/E core detection)
    CpuTopology topology;
    // Bluetooth
    bool bluetooth_detected = false;
    std::string bluetooth_name;     // e.g. "hci0"
    // Input devices
    bool touchpad_detected = false;
    // Battery (laptop detection)
    bool has_battery = false;
    int battery_percent = -1;       // -1 = unknown
    // Network interfaces detected by eudev
    int wifi_interfaces = 0;
    int ethernet_interfaces = 0;
};

HardwareInfo detect_hardware();
int read_meminfo_kb(const char* key);
std::string read_sysfs_line(const char* path);
