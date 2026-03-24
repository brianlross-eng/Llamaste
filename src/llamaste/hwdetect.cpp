#include "hwdetect.h"
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/wireless.h>
#endif

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

    // GPU from /sys/class/drm — also detect driver name
    DIR* drm = opendir("/sys/class/drm");
    if (drm) {
        struct dirent* entry;
        while ((entry = readdir(drm)) != nullptr) {
            // Only look at card devices (card0, card1), skip connectors (card0-HDMI-A-1)
            if (strncmp(entry->d_name, "card", 4) != 0) continue;
            if (strchr(entry->d_name, '-')) continue;

            std::string base = std::string("/sys/class/drm/") + entry->d_name;
            std::string vendor = read_sysfs_line((base + "/device/vendor").c_str());

            if (vendor == "0x10de" || vendor == "0x1002" || vendor == "0x8086") {
                hw.gpu_detected = true;
                if (vendor == "0x10de") hw.gpu_name = "NVIDIA";
                else if (vendor == "0x1002") hw.gpu_name = "AMD";
                else if (vendor == "0x8086") hw.gpu_name = "Intel";

                // Read driver name from symlink
                char link[256] = {};
                std::string drv_path = base + "/device/driver";
                ssize_t len = readlink(drv_path.c_str(), link, sizeof(link) - 1);
                if (len > 0) {
                    link[len] = '\0';
                    const char* drv = strrchr(link, '/');
                    hw.gpu_driver = drv ? (drv + 1) : link;
                }
                break; // prefer first real GPU
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

#ifndef _WIN32
    // Bluetooth from /sys/class/bluetooth
    DIR* bt = opendir("/sys/class/bluetooth");
    if (bt) {
        struct dirent* entry;
        while ((entry = readdir(bt)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            if (strncmp(entry->d_name, "hci", 3) == 0) {
                hw.bluetooth_detected = true;
                hw.bluetooth_name = entry->d_name;
                break;
            }
        }
        closedir(bt);
    }

    // Touchpad from /sys/class/input — look for touchpad/trackpad devices
    DIR* input = opendir("/sys/class/input");
    if (input) {
        struct dirent* entry;
        while ((entry = readdir(input)) != nullptr) {
            if (strncmp(entry->d_name, "event", 5) != 0) continue;
            std::string name_path = std::string("/sys/class/input/") +
                entry->d_name + "/device/name";
            std::string name = read_sysfs_line(name_path.c_str());
            // Common touchpad identifiers
            if (name.find("ouchpad") != std::string::npos ||
                name.find("trackpad") != std::string::npos ||
                name.find("Touchpad") != std::string::npos ||
                name.find("TrackPad") != std::string::npos ||
                name.find("ELAN") != std::string::npos ||
                name.find("Synaptics") != std::string::npos ||
                name.find("ALPS") != std::string::npos) {
                hw.touchpad_detected = true;
                break;
            }
        }
        closedir(input);
    }

    // Battery from /sys/class/power_supply
    DIR* ps = opendir("/sys/class/power_supply");
    if (ps) {
        struct dirent* entry;
        while ((entry = readdir(ps)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string type_path = std::string("/sys/class/power_supply/") +
                entry->d_name + "/type";
            std::string type = read_sysfs_line(type_path.c_str());
            if (type == "Battery") {
                hw.has_battery = true;
                std::string cap_path = std::string("/sys/class/power_supply/") +
                    entry->d_name + "/capacity";
                std::string cap = read_sysfs_line(cap_path.c_str());
                if (!cap.empty()) hw.battery_percent = std::atoi(cap.c_str());
                break;
            }
        }
        closedir(ps);
    }

    // Network interfaces from /sys/class/net
    DIR* net = opendir("/sys/class/net");
    if (net) {
        struct dirent* entry;
        while ((entry = readdir(net)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            if (strcmp(entry->d_name, "lo") == 0) continue;
            if (strcmp(entry->d_name, "sit0") == 0) continue;

            // Check if wireless by testing for /sys/class/net/<iface>/wireless
            std::string wireless_path = std::string("/sys/class/net/") +
                entry->d_name + "/wireless";
            struct stat st;
            if (stat(wireless_path.c_str(), &st) == 0) {
                hw.wifi_interfaces++;
            } else {
                // Check it's a real device (has /device symlink, not virtual)
                std::string dev_path = std::string("/sys/class/net/") +
                    entry->d_name + "/device";
                if (stat(dev_path.c_str(), &st) == 0) {
                    hw.ethernet_interfaces++;
                }
            }
        }
        closedir(net);
    }
#endif

    return hw;
}
