#include "init.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

static void try_mount(const char* src, const char* tgt,
                      const char* fs, unsigned long flags,
                      const char* data) {
    mkdir(tgt, 0755);
    if (mount(src, tgt, fs, flags, data) != 0)
        fprintf(stderr, "[init] mount %s failed: %m\n", tgt);
}

void init_mount_filesystems() {
    try_mount("proc",     "/proc",    "proc",     0, nullptr);
    try_mount("sysfs",    "/sys",     "sysfs",    0, nullptr);
    try_mount("devtmpfs", "/dev",     "devtmpfs", 0, nullptr);
    try_mount("tmpfs",    "/tmp",     "tmpfs",    0, "size=64M");
    try_mount("tmpfs",    "/run",     "tmpfs",    0, "size=16M");
    mkdir("/dev/pts", 0755);
    try_mount("devpts",   "/dev/pts", "devpts",   0, nullptr);
}

bool init_mount_data() {
    const char* candidates[] = {
        "/dev/vda5", "/dev/sda5", "/dev/nvme0n1p5",
        "/dev/vda4", "/dev/sda4", "/dev/nvme0n1p4",
        nullptr
    };

    mkdir("/data", 0755);
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0) {
            if (mount(candidates[i], "/data", "ext4", 0, nullptr) == 0) {
                fprintf(stderr, "[init] Mounted %s on /data\n", candidates[i]);
                return true;
            }
        }
    }

    fprintf(stderr, "[init] WARNING: No data partition, using tmpfs\n");
    try_mount("tmpfs", "/data", "tmpfs", 0, "size=1G");
    return false;
}

void init_create_data_dirs() {
    const char* dirs[] = {
        "/data/models",
        "/data/llamaste",
        "/data/llamaste/conversations",
        "/data/llamaste/config",
        "/data/llamaste/logs",
        "/data/llamaste/skills",
        nullptr
    };
    for (int i = 0; dirs[i]; i++)
        mkdir(dirs[i], 0755);
}

void init_tune_performance() {
    for (int i = 0; i < 256; i++) {
        char path[128];
        snprintf(path, sizeof(path),
            "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", i);
        std::ofstream f(path);
        if (!f.is_open()) break;
        f << "performance";
    }

    std::ofstream thp("/sys/kernel/mm/transparent_hugepage/enabled");
    if (thp.is_open()) thp << "madvise";

    std::ofstream sw("/proc/sys/vm/swappiness");
    if (sw.is_open()) sw << "1";

    std::ofstream mm("/proc/sys/vm/max_map_count");
    if (mm.is_open()) mm << "1048576";
}

void init_set_hostname(const std::string& default_name) {
    std::string hostname = default_name;
    std::ifstream hf("/data/llamaste/config/hostname");
    if (hf.is_open()) {
        std::string saved;
        if (std::getline(hf, saved) && !saved.empty())
            hostname = saved;
    }
    std::ofstream hn("/proc/sys/kernel/hostname");
    if (hn.is_open()) hn << hostname;
    fprintf(stderr, "[init] Hostname: %s\n", hostname.c_str());
}

std::string init_parse_boot_mode() {
    std::ifstream f("/proc/cmdline");
    std::string line;
    if (std::getline(f, line)) {
        if (line.find("llamaste.mode=desktop") != std::string::npos)
            return "desktop";
    }
    return "server";
}
