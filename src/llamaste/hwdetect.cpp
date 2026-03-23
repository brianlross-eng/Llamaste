#include "hwdetect.h"
#include <fstream>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

std::string read_sysfs_line(const char* path) {
    std::ifstream f(path);
    std::string line;
    if (std::getline(f, line)) return line;
    return "";
}

int read_meminfo_kb(const char* key) {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(key) == 0) {
            size_t colon = line.find(':');
            if (colon != std::string::npos)
                return std::atoi(line.c_str() + colon + 1);
        }
    }
    return 0;
}

HardwareInfo detect_hardware() {
    HardwareInfo hw;

    // CPU info from /proc/cpuinfo
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    int core_count = 0;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") == 0 && hw.cpu_model == "unknown") {
            size_t colon = line.find(':');
            if (colon != std::string::npos)
                hw.cpu_model = line.substr(colon + 2);
        }
        if (line.find("processor") == 0)
            core_count++;
        if (line.find("avx2") != std::string::npos)
            hw.has_avx2 = true;
        if (line.find("avx512") != std::string::npos)
            hw.has_avx512 = true;
    }
    hw.cpu_cores = core_count > 0 ? core_count : 1;

    // RAM from /proc/meminfo
    hw.ram_total_mb = read_meminfo_kb("MemTotal") / 1024;
    hw.ram_free_mb = read_meminfo_kb("MemAvailable") / 1024;

    // GPU from /sys/class/drm
    DIR* drm = opendir("/sys/class/drm");
    if (drm) {
        struct dirent* entry;
        while ((entry = readdir(drm)) != nullptr) {
            std::string vendor_path = std::string("/sys/class/drm/")
                + entry->d_name + "/device/vendor";
            std::string vendor = read_sysfs_line(vendor_path.c_str());
            if (vendor == "0x10de") {
                hw.gpu_detected = true;
                hw.gpu_name = "NVIDIA";
            } else if (vendor == "0x1002") {
                hw.gpu_detected = true;
                hw.gpu_name = "AMD";
            } else if (vendor == "0x8086") {
                hw.gpu_detected = true;
                hw.gpu_name = "Intel";
            }
        }
        closedir(drm);
    }

    // Fallback: scan PCI bus for GPUs not exposed via DRM (e.g. no kernel driver loaded)
    if (!hw.gpu_detected) {
        DIR* pci = opendir("/sys/bus/pci/devices");
        if (pci) {
            struct dirent* entry;
            while ((entry = readdir(pci)) != nullptr) {
                if (entry->d_name[0] == '.') continue;
                std::string base = std::string("/sys/bus/pci/devices/") + entry->d_name;
                std::string vendor = read_sysfs_line((base + "/vendor").c_str());
                std::string cls = read_sysfs_line((base + "/class").c_str());
                // PCI class 0x03xxxx = display controller
                unsigned long cls_val = 0;
                if (!cls.empty()) cls_val = strtoul(cls.c_str(), nullptr, 16);
                if ((cls_val >> 16) == 0x03) {
                    hw.gpu_detected = true;
                    if (vendor == "0x10de") hw.gpu_name = "NVIDIA";
                    else if (vendor == "0x1002") hw.gpu_name = "AMD";
                    else if (vendor == "0x8086") hw.gpu_name = "Intel";
                    else hw.gpu_name = "GPU (PCI " + vendor + ")";
                    break;
                }
            }
            closedir(pci);
        }
    }

    return hw;
}
