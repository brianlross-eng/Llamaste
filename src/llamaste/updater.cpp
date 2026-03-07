// updater.cpp -- A/B update system implementation
//
// Slot detection, GRUB environment block read/write, semver comparison,
// and update manifest parsing for the dual-partition update mechanism.

#include "updater.h"
#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
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
#include "update_pubkey.h"

bool verify_update_signature(
    const unsigned char* sig, size_t sig_len,
    const unsigned char* msg, size_t msg_len,
    const unsigned char* pubkey
) {
    if (sig_len != 64) return false;
    return crypto_sign_ed25519_verify_detached(sig, msg, (unsigned long long)msg_len, pubkey) == 0;
}

// ---------------------------------------------------------------------------
// SHA-256 helpers
// ---------------------------------------------------------------------------

std::string sha256_hex(const unsigned char* data, size_t len) {
    unsigned char hash[32];
    crypto_hash_sha256(hash, data, (unsigned long long)len);
    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", hash[i]);
    }
    hex[64] = '\0';
    return std::string(hex);
}

std::string sha256_file(const std::string& path, uint64_t offset) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size < 0 || (uint64_t)file_size <= offset) {
        fclose(f);
        return "";
    }
    uint64_t data_size = (uint64_t)file_size - offset;

    // Read payload into memory (OK for squashfs — typically <200MB)
    fseek(f, (long)offset, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc((size_t)data_size);
    if (!buf) { fclose(f); return ""; }

    size_t nread = fread(buf, 1, (size_t)data_size, f);
    fclose(f);
    if (nread != (size_t)data_size) { free(buf); return ""; }

    std::string result = sha256_hex(buf, (size_t)data_size);
    free(buf);
    return result;
}

// ---------------------------------------------------------------------------
// .update file parsing
// ---------------------------------------------------------------------------

static uint32_t read_u32_le(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

UpdateFileInfo parse_update_file(const std::string& path) {
    UpdateFileInfo info;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        info.error = "cannot open file: " + path;
        return info;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Read fixed header: magic(4) + format_version(4) + manifest_size(4) = 12 bytes
    unsigned char hdr[12];
    if (fread(hdr, 1, 12, f) != 12) {
        fclose(f);
        info.error = "file too small for header";
        return info;
    }

    // Verify magic
    if (memcmp(hdr, "LMUP", 4) != 0) {
        fclose(f);
        info.error = "invalid magic (not a .update file)";
        return info;
    }

    // Check format version
    uint32_t fmt_ver = read_u32_le(hdr + 4);
    if (fmt_ver != 1) {
        fclose(f);
        info.error = "unsupported format version: " + std::to_string(fmt_ver);
        return info;
    }

    // Read manifest
    uint32_t manifest_size = read_u32_le(hdr + 8);
    if (manifest_size > 1024 * 1024) {  // sanity: max 1MB manifest
        fclose(f);
        info.error = "manifest too large: " + std::to_string(manifest_size);
        return info;
    }

    std::string manifest_json(manifest_size, '\0');
    if (fread(&manifest_json[0], 1, manifest_size, f) != manifest_size) {
        fclose(f);
        info.error = "truncated manifest";
        return info;
    }
    info.manifest_json = manifest_json;

    // Read signature (64 bytes)
    if (fread(info.signature, 1, 64, f) != 64) {
        fclose(f);
        info.error = "truncated signature";
        return info;
    }

    // Payload starts here
    info.payload_offset = 12 + manifest_size + 64;
    if ((uint64_t)file_size > info.payload_offset) {
        info.payload_size = (uint64_t)file_size - info.payload_offset;
    }

    fclose(f);

    // Parse manifest JSON
    info.manifest = parse_manifest(manifest_json);
    if (!info.manifest.valid) {
        info.error = "invalid manifest JSON";
        return info;
    }

    info.valid = true;
    return info;
}

// ---------------------------------------------------------------------------
// Update installation
// ---------------------------------------------------------------------------

std::string install_update(const std::string& path) {
    json result;

    // Step 1: Parse .update file
    auto info = parse_update_file(path);
    if (!info.valid) {
        result["error"] = info.error;
        return result.dump();
    }

    // Step 2: Check version — must be newer than current
    if (version_compare(info.manifest.version, LLAMASTE_VERSION) <= 0) {
        result["error"] = "update version " + info.manifest.version +
                          " is not newer than current " + std::string(LLAMASTE_VERSION);
        return result.dump();
    }

    // Step 3: Check minimum version requirement
    if (!info.manifest.min_version.empty() &&
        version_compare(LLAMASTE_VERSION, info.manifest.min_version) < 0) {
        result["error"] = "current version " + std::string(LLAMASTE_VERSION) +
                          " is below minimum required " + info.manifest.min_version;
        return result.dump();
    }

    // Step 4: Verify Ed25519 signature of manifest
    bool sig_ok = verify_update_signature(
        info.signature, 64,
        (const unsigned char*)info.manifest_json.data(),
        info.manifest_json.size(),
        UPDATE_PUBLIC_KEY
    );
    if (!sig_ok) {
        result["error"] = "signature verification failed — update is not authentic";
        return result.dump();
    }

    // Step 5: Verify SHA-256 of payload
    std::string payload_sha = sha256_file(path, info.payload_offset);
    if (payload_sha.empty()) {
        result["error"] = "failed to compute payload SHA-256";
        return result.dump();
    }
    if (payload_sha != info.manifest.system.sha256) {
        result["error"] = "payload SHA-256 mismatch: expected " +
                          info.manifest.system.sha256 + " got " + payload_sha;
        return result.dump();
    }

#ifdef _WIN32
    // On Windows (host testing) — skip partition write
    result["success"] = true;
    result["version"] = info.manifest.version;
    result["message"] = "Update verified (Windows test mode — partition write skipped)";
    return result.dump();
#else
    // Step 6: Write payload to inactive partition
    std::string active = detect_current_slot();
    int part_num = inactive_partition_num(active);
    std::string target_slot = inactive_slot(active);

    // Find the block device — try common patterns
    std::string block_dev;
    const char* candidates[] = {
        "/dev/sda", "/dev/vda", "/dev/nvme0n1p"
    };
    for (const char* base : candidates) {
        std::string dev = std::string(base);
        // nvme uses pN directly, sda/vda use N
        if (dev.find("nvme") != std::string::npos) {
            dev += std::to_string(part_num);
        } else {
            dev += std::to_string(part_num);
        }
        struct stat st;
        if (stat(dev.c_str(), &st) == 0) {
            block_dev = dev;
            break;
        }
    }
    if (block_dev.empty()) {
        result["error"] = "cannot find block device for partition " + std::to_string(part_num);
        return result.dump();
    }

    // Open source (payload from .update file)
    FILE* src = fopen(path.c_str(), "rb");
    if (!src) {
        result["error"] = "cannot reopen .update file";
        return result.dump();
    }
    fseek(src, (long)info.payload_offset, SEEK_SET);

    // Open target partition
    int tgt_fd = open(block_dev.c_str(), O_WRONLY);
    if (tgt_fd < 0) {
        fclose(src);
        result["error"] = "cannot open " + block_dev + " for writing";
        return result.dump();
    }

    // Stream write in 4MB chunks
    const size_t BUF_SIZE = 4 * 1024 * 1024;
    unsigned char* buf = (unsigned char*)malloc(BUF_SIZE);
    if (!buf) {
        close(tgt_fd);
        fclose(src);
        result["error"] = "out of memory for write buffer";
        return result.dump();
    }

    uint64_t written = 0;
    bool write_ok = true;
    while (written < info.payload_size) {
        size_t chunk = (size_t)std::min((uint64_t)BUF_SIZE, info.payload_size - written);
        size_t nread = fread(buf, 1, chunk, src);
        if (nread != chunk) {
            write_ok = false;
            break;
        }
        ssize_t nw = write(tgt_fd, buf, nread);
        if (nw < 0 || (size_t)nw != nread) {
            write_ok = false;
            break;
        }
        written += nread;
    }

    // Sync and close
    fsync(tgt_fd);
    close(tgt_fd);
    fclose(src);
    free(buf);

    if (!write_ok) {
        result["error"] = "write failed after " + std::to_string(written) + " bytes";
        return result.dump();
    }

    // Step 7: Update grubenv to boot into the new slot
    std::string grubenv_path = find_grubenv_path();
    if (!grubenv_path.empty()) {
        auto vars = grubenv_read(grubenv_path);
        vars["active_slot"] = target_slot;
        vars["boot_success"] = "0";
        vars["boot_counter"] = "3";
        grubenv_write(grubenv_path, vars);
    }

    // Step 8: Save slot metadata
    std::string meta_dir = "/data/llamaste/slots";
    mkdir(meta_dir.c_str(), 0755);
    std::string meta_path = meta_dir + "/" + target_slot + ".json";
    json meta;
    meta["version"] = info.manifest.version;
    meta["build_date"] = info.manifest.build_date;
    meta["installed_at"] = ""; // TODO: add timestamp
    std::ofstream mf(meta_path);
    if (mf.is_open()) {
        mf << meta.dump(2);
    }

    result["success"] = true;
    result["version"] = info.manifest.version;
    result["installed_to_slot"] = target_slot;
    result["partition"] = block_dev;
    result["bytes_written"] = written;
    result["message"] = "Update installed to slot " + target_slot +
                        ". Reboot to activate (version " + info.manifest.version + ").";
    return result.dump();
#endif
}

// Note: Online update checking (check_for_update) is implemented directly in
// tools_update.cpp using libcurl, not here, to avoid linking curl into the
// updater library which is also used by tests.

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
        "/boot/efi/grub/grubenv",
        "/boot/efi/boot/grub/grubenv",
        "/boot/grub/grubenv",
        "/mnt/esp/EFI/BOOT/grubenv",
        "/mnt/esp/grub/grubenv",
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
