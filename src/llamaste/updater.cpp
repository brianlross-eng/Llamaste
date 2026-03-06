// updater.cpp -- A/B update system implementation
//
// Slot detection, GRUB environment block read/write, semver comparison,
// and update manifest parsing for the dual-partition update mechanism.

#include "updater.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>

#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Slot detection
// ---------------------------------------------------------------------------

std::string parse_active_slot(const std::string& cmdline) {
    const std::string key = "llamaste.slot=";
    auto pos = cmdline.find(key);
    if (pos == std::string::npos) return "A";

    pos += key.size();
    if (pos < cmdline.size()) {
        char c = cmdline[pos];
        if (c == 'B' || c == 'b') return "B";
    }
    return "A";
}

std::string inactive_slot(const std::string& active) {
    return (active == "A") ? "B" : "A";
}

int inactive_partition_num(const std::string& active) {
    // Partition layout: 3=SYS-A, 4=SYS-B
    // If A is active, inactive is B (partition 4)
    // If B is active, inactive is A (partition 3)
    return (active == "A") ? 4 : 3;
}

std::string detect_current_slot() {
#ifdef _WIN32
    return "A";
#else
    std::ifstream f("/proc/cmdline");
    if (!f.is_open()) return "A";
    std::string cmdline;
    std::getline(f, cmdline);
    return parse_active_slot(cmdline);
#endif
}

// ---------------------------------------------------------------------------
// GRUB environment block
// ---------------------------------------------------------------------------

static const char* GRUBENV_HEADER = "# GRUB Environment Block\n";
static const size_t GRUBENV_SIZE = 1024;

std::map<std::string, std::string> grubenv_parse(const std::string& content) {
    std::map<std::string, std::string> vars;
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        vars[key] = val;
    }
    return vars;
}

std::string grubenv_serialize(const std::map<std::string, std::string>& vars) {
    std::string result = GRUBENV_HEADER;
    for (const auto& [key, val] : vars) {
        result += key + "=" + val + "\n";
    }
    // Pad with '#' to exactly 1024 bytes
    if (result.size() < GRUBENV_SIZE) {
        result.append(GRUBENV_SIZE - result.size(), '#');
    } else if (result.size() > GRUBENV_SIZE) {
        // Truncate (shouldn't happen with reasonable data)
        result.resize(GRUBENV_SIZE);
    }
    return result;
}

std::map<std::string, std::string> grubenv_read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return grubenv_parse(content);
}

bool grubenv_write(const std::string& path, const std::map<std::string, std::string>& vars) {
    std::string tmp_path = path + ".tmp";
    std::string data = grubenv_serialize(vars);

    FILE* fp = fopen(tmp_path.c_str(), "wb");
    if (!fp) return false;

    size_t written = fwrite(data.data(), 1, data.size(), fp);
    if (written != data.size()) {
        fclose(fp);
        return false;
    }

    fflush(fp);
#ifndef _WIN32
    fsync(fileno(fp));
#endif
    fclose(fp);

    // Atomic rename
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Version comparison
// ---------------------------------------------------------------------------

int version_compare(const std::string& a, const std::string& b) {
    int a_major = 0, a_minor = 0, a_patch = 0;
    int b_major = 0, b_minor = 0, b_patch = 0;
    sscanf(a.c_str(), "%d.%d.%d", &a_major, &a_minor, &a_patch);
    sscanf(b.c_str(), "%d.%d.%d", &b_major, &b_minor, &b_patch);

    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

// ---------------------------------------------------------------------------
// Update manifest
// ---------------------------------------------------------------------------

UpdateManifest parse_manifest(const std::string& json_str) {
    UpdateManifest m;
    try {
        auto j = json::parse(json_str);

        m.format_version = j.value("format_version", 0);
        m.version = j.value("version", "");
        m.build_date = j.value("build_date", "");
        m.arch = j.value("arch", "");
        m.min_version = j.value("min_version", "");

        if (j.contains("components") && j["components"].contains("system")) {
            auto& sys = j["components"]["system"];
            m.system.file = sys.value("file", "");
            m.system.sha256 = sys.value("sha256", "");
            m.system.size_compressed = sys.value("size_compressed", (uint64_t)0);
            m.system.size_uncompressed = sys.value("size_uncompressed", (uint64_t)0);
        }

        if (j.contains("changelog") && j["changelog"].is_array()) {
            for (const auto& entry : j["changelog"]) {
                if (entry.is_string()) {
                    m.changelog.push_back(entry.get<std::string>());
                }
            }
        }

        m.valid = !m.version.empty() && m.format_version > 0;
    } catch (...) {
        m.valid = false;
    }
    return m;
}

// ---------------------------------------------------------------------------
// Ed25519 signature verification
// ---------------------------------------------------------------------------

#include "tweetnacl.h"

bool verify_update_signature(
    const unsigned char* sig, size_t sig_len,
    const unsigned char* msg, size_t msg_len,
    const unsigned char* pubkey
) {
    if (sig_len != 64) return false;
    return crypto_sign_ed25519_verify_detached(sig, msg, (unsigned long long)msg_len, pubkey) == 0;
}

// ---------------------------------------------------------------------------
// ESP / grubenv helpers
// ---------------------------------------------------------------------------

static bool file_exists(const std::string& path) {
#ifdef _WIN32
    FILE* f = fopen(path.c_str(), "r");
    if (f) { fclose(f); return true; }
    return false;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0;
#endif
}

std::string find_grubenv_path() {
    static const char* candidates[] = {
        "/boot/efi/EFI/BOOT/grubenv",
        "/boot/efi/boot/grub/grubenv",
        "/boot/grub/grubenv",
        "/mnt/esp/EFI/BOOT/grubenv",
        "/mnt/esp/boot/grub/grubenv",
    };
    for (const char* path : candidates) {
        if (file_exists(path)) return path;
    }
    return "";
}

void mark_boot_success() {
#ifdef _WIN32
    return;  // No-op on Windows
#else
    // Try to find grubenv directly first
    std::string gpath = find_grubenv_path();

    // If not found, try mounting ESP
    if (gpath.empty()) {
        // Create mount point if needed
        mkdir("/mnt/esp", 0755);

        // Try common ESP devices
        static const char* esp_devs[] = {
            "/dev/sda2", "/dev/vda2", "/dev/nvme0n1p2"
        };
        bool mounted = false;
        for (const char* dev : esp_devs) {
            if (file_exists(dev)) {
                if (mount(dev, "/mnt/esp", "vfat", MS_NOATIME, "") == 0) {
                    mounted = true;
                    break;
                }
            }
        }
        if (mounted) {
            gpath = find_grubenv_path();
        }
    }

    if (gpath.empty()) {
        fprintf(stderr, "updater: cannot find grubenv, skipping mark_boot_success\n");
        return;
    }

    auto vars = grubenv_read(gpath);
    vars["boot_success"] = "1";
    vars.erase("boot_counter");
    grubenv_write(gpath, vars);
#endif
}
