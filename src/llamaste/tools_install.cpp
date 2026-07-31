// tools_install.cpp -- Installer tools for live ISO mode
//
// Provides tools for detecting target disks and installing Llamaste
// to a local hard drive from the live ISO environment.
//
// These tools are only registered when running in live (ISO) mode.
//
// IMPORTANT: This system has NO shell (/bin/sh), NO BusyBox, NO external
// commands. All operations must be implemented in pure C++ using syscalls.

#include "tools.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <algorithm>

#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <linux/fs.h>  // BLKRRPART, BLKGETSIZE64
#include <sys/mount.h>
#endif

// liblzma for XZ decompression (linked statically via Buildroot)
#ifdef HAVE_LZMA
#include <lzma.h>
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Installation progress tracking
// ---------------------------------------------------------------------------

struct InstallProgress {
    std::atomic<int> percent{0};
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> success{false};
    std::string status;
    std::string error;
    std::mutex status_mutex;

    void set_status(const std::string& s) {
        std::lock_guard<std::mutex> lock(status_mutex);
        status = s;
        fprintf(stderr, "[installer] %s\n", s.c_str());
    }

    std::string get_status() {
        std::lock_guard<std::mutex> lock(status_mutex);
        return status;
    }

    void set_error(const std::string& e) {
        std::lock_guard<std::mutex> lock(status_mutex);
        error = e;
        fprintf(stderr, "[installer] ERROR: %s\n", e.c_str());
    }

    std::string get_error() {
        std::lock_guard<std::mutex> lock(status_mutex);
        return error;
    }

    void reset() {
        percent = 0;
        running = false;
        finished = false;
        success = false;
        std::lock_guard<std::mutex> lock(status_mutex);
        status = "";
        error = "";
    }
};

static InstallProgress g_install_progress;

// ---------------------------------------------------------------------------
// Helper: read a sysfs attribute as a string
// ---------------------------------------------------------------------------

static std::string read_sysfs(const std::string& path) {
    std::ifstream f(path);
    std::string val;
    if (f.is_open() && std::getline(f, val)) {
        // Trim trailing whitespace/newlines
        while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' '))
            val.pop_back();
    }
    return val;
}

// ---------------------------------------------------------------------------
// Helper: find the boot device (which we must not write to)
// ---------------------------------------------------------------------------

static std::string find_boot_device() {
    // Read /proc/cmdline to find root= parameter
    std::ifstream f("/proc/cmdline");
    std::string cmdline;
    if (std::getline(f, cmdline)) {
        auto pos = cmdline.find("root=");
        if (pos != std::string::npos) {
            std::string root_spec = cmdline.substr(pos + 5);
            auto space = root_spec.find(' ');
            if (space != std::string::npos)
                root_spec = root_spec.substr(0, space);
            if (root_spec.find("/dev/") == 0) {
                std::string dev = root_spec.substr(5);
                // Remove partition number suffix: sda3->sda, nvme0n1p3->nvme0n1
                while (!dev.empty() && (dev.back() >= '0' && dev.back() <= '9'))
                    dev.pop_back();
                if (!dev.empty() && dev.back() == 'p')
                    dev.pop_back();
                return dev;
            }
        }
    }

    // Fallback: check /proc/mounts for the root mount
    std::ifstream mounts("/proc/mounts");
    std::string line;
    while (std::getline(mounts, line)) {
        if (line.find(" / ") != std::string::npos) {
            auto space = line.find(' ');
            if (space != std::string::npos) {
                std::string dev = line.substr(0, space);
                if (dev.find("/dev/") == 0) {
                    dev = dev.substr(5);
                    while (!dev.empty() && (dev.back() >= '0' && dev.back() <= '9'))
                        dev.pop_back();
                    if (!dev.empty() && dev.back() == 'p')
                        dev.pop_back();
                    return dev;
                }
            }
        }
    }

    return "sr0";  // Default: assume CD-ROM is boot device
}

// ---------------------------------------------------------------------------
// CRC32 for GPT (standard CRC32 used by UEFI/GPT spec)
// ---------------------------------------------------------------------------

#ifndef _WIN32
static uint32_t crc32_gpt(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}
#endif

// ---------------------------------------------------------------------------
// GPT structures (packed, matching on-disk format)
// ---------------------------------------------------------------------------

#ifndef _WIN32
struct __attribute__((packed)) GPTHeader {
    uint8_t  signature[8];           // "EFI PART"
    uint32_t revision;               // 0x00010000
    uint32_t header_size;            // Usually 92
    uint32_t header_crc32;           // CRC32 of header (with this field zeroed)
    uint32_t reserved;               // Must be zero
    uint64_t my_lba;                 // Location of this header
    uint64_t alternate_lba;          // Location of backup header
    uint64_t first_usable_lba;       // First usable LBA for partitions
    uint64_t last_usable_lba;        // Last usable LBA
    uint8_t  disk_guid[16];          // Disk GUID
    uint64_t partition_entry_lba;    // Start LBA of partition entries
    uint32_t num_partition_entries;   // Number of partition entries
    uint32_t partition_entry_size;   // Size of each entry (usually 128)
    uint32_t partition_array_crc32;  // CRC32 of partition entry array
};

struct __attribute__((packed)) GPTEntry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint8_t  name[72];  // UTF-16LE name
};
#endif

// ---------------------------------------------------------------------------
// Pure C++ block copy: file -> device with progress tracking
// ---------------------------------------------------------------------------

#ifndef _WIN32
static int copy_file_to_device(const std::string& src_path, const std::string& device,
                                uint64_t src_size, int pct_start, int pct_end) {
    int src_fd = open(src_path.c_str(), O_RDONLY);
    if (src_fd < 0) {
        g_install_progress.set_error("Cannot open source: " + src_path + " (" + strerror(errno) + ")");
        return -1;
    }

    int dst_fd = open(device.c_str(), O_WRONLY);
    if (dst_fd < 0) {
        close(src_fd);
        g_install_progress.set_error("Cannot open device: " + device + " (" + strerror(errno) + ")");
        return -1;
    }

    const size_t BUF_SIZE = 4 * 1024 * 1024;  // 4 MB buffer
    uint8_t* buf = (uint8_t*)malloc(BUF_SIZE);
    if (!buf) {
        close(src_fd);
        close(dst_fd);
        g_install_progress.set_error("Failed to allocate copy buffer");
        return -1;
    }

    uint64_t total_written = 0;
    ssize_t n;
    while ((n = read(src_fd, buf, BUF_SIZE)) > 0) {
        ssize_t offset = 0;
        while (offset < n) {
            ssize_t w = write(dst_fd, buf + offset, n - offset);
            if (w <= 0) {
                free(buf);
                close(src_fd);
                close(dst_fd);
                g_install_progress.set_error("Write error at offset " +
                    std::to_string(total_written) + ": " + strerror(errno));
                return -1;
            }
            offset += w;
        }
        total_written += n;

        // Update progress
        if (src_size > 0) {
            double fraction = (double)total_written / (double)src_size;
            int pct = pct_start + (int)((pct_end - pct_start) * fraction);
            g_install_progress.percent = std::min(pct, pct_end);
        }
    }

    if (n < 0) {
        free(buf);
        close(src_fd);
        close(dst_fd);
        g_install_progress.set_error("Read error: " + std::string(strerror(errno)));
        return -1;
    }

    fsync(dst_fd);
    free(buf);
    close(src_fd);
    close(dst_fd);

    fprintf(stderr, "[installer] Wrote %llu bytes to %s\n",
            (unsigned long long)total_written, device.c_str());
    return 0;
}
#endif

// ---------------------------------------------------------------------------
// XZ decompression -> device using liblzma (pure C++, no external commands)
// ---------------------------------------------------------------------------

#if defined(HAVE_LZMA) && !defined(_WIN32)
static int decompress_xz_to_device(const std::string& xz_path, const std::string& device,
                                    uint64_t xz_file_size, int pct_start, int pct_end) {
    int src_fd = open(xz_path.c_str(), O_RDONLY);
    if (src_fd < 0) {
        g_install_progress.set_error("Cannot open XZ file: " + xz_path + " (" + strerror(errno) + ")");
        return -1;
    }

    int dst_fd = open(device.c_str(), O_WRONLY);
    if (dst_fd < 0) {
        close(src_fd);
        g_install_progress.set_error("Cannot open device: " + device + " (" + strerror(errno) + ")");
        return -1;
    }

    // Initialize liblzma decoder
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret = lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
    if (ret != LZMA_OK) {
        close(src_fd);
        close(dst_fd);
        g_install_progress.set_error("Failed to initialize XZ decoder");
        return -1;
    }

    const size_t IN_BUF_SIZE = 1024 * 1024;      // 1 MB input buffer
    const size_t OUT_BUF_SIZE = 4 * 1024 * 1024;  // 4 MB output buffer
    uint8_t* in_buf = (uint8_t*)malloc(IN_BUF_SIZE);
    uint8_t* out_buf = (uint8_t*)malloc(OUT_BUF_SIZE);
    if (!in_buf || !out_buf) {
        free(in_buf);
        free(out_buf);
        lzma_end(&strm);
        close(src_fd);
        close(dst_fd);
        g_install_progress.set_error("Failed to allocate decompression buffers");
        return -1;
    }

    uint64_t total_read = 0;
    uint64_t total_written = 0;
    lzma_action action = LZMA_RUN;

    strm.next_in = nullptr;
    strm.avail_in = 0;
    strm.next_out = out_buf;
    strm.avail_out = OUT_BUF_SIZE;

    bool done = false;
    int result = 0;

    while (!done) {
        // Read more input if needed
        if (strm.avail_in == 0) {
            ssize_t n = read(src_fd, in_buf, IN_BUF_SIZE);
            if (n < 0) {
                g_install_progress.set_error("XZ read error: " + std::string(strerror(errno)));
                result = -1;
                break;
            }
            if (n == 0) {
                action = LZMA_FINISH;
            }
            strm.next_in = in_buf;
            strm.avail_in = (size_t)n;
            total_read += (uint64_t)n;
        }

        // Decompress
        ret = lzma_code(&strm, action);

        // Write output if buffer is full or stream ended
        if (strm.avail_out == 0 || ret == LZMA_STREAM_END) {
            size_t write_size = OUT_BUF_SIZE - strm.avail_out;
            if (write_size > 0) {
                ssize_t offset = 0;
                while ((size_t)offset < write_size) {
                    ssize_t w = write(dst_fd, out_buf + offset, write_size - offset);
                    if (w <= 0) {
                        g_install_progress.set_error("Write error during decompression: " +
                            std::string(strerror(errno)));
                        result = -1;
                        done = true;
                        break;
                    }
                    offset += w;
                }
                total_written += write_size;
            }
            strm.next_out = out_buf;
            strm.avail_out = OUT_BUF_SIZE;
        }

        if (ret == LZMA_STREAM_END) {
            done = true;
        } else if (ret != LZMA_OK) {
            const char* msg = "Unknown error";
            switch (ret) {
                case LZMA_MEM_ERROR: msg = "Memory allocation failed"; break;
                case LZMA_FORMAT_ERROR: msg = "Not an XZ file"; break;
                case LZMA_DATA_ERROR: msg = "Compressed data is corrupt"; break;
                case LZMA_BUF_ERROR: msg = "Buffer error"; break;
                default: break;
            }
            g_install_progress.set_error(std::string("XZ decompression error: ") + msg);
            result = -1;
            break;
        }

        // Update progress based on compressed bytes read
        if (xz_file_size > 0) {
            double fraction = (double)total_read / (double)xz_file_size;
            int pct = pct_start + (int)((pct_end - pct_start) * fraction);
            g_install_progress.percent = std::min(pct, pct_end);
        }
    }

    fsync(dst_fd);
    free(in_buf);
    free(out_buf);
    lzma_end(&strm);
    close(src_fd);
    close(dst_fd);

    if (result == 0) {
        fprintf(stderr, "[installer] Decompressed %llu bytes -> %llu bytes to %s\n",
                (unsigned long long)total_read, (unsigned long long)total_written,
                device.c_str());
    }
    return result;
}
#endif  // HAVE_LZMA && !_WIN32

// ---------------------------------------------------------------------------
// GPT partition 5 resize — expand to fill remaining disk space (pure C++)
// ---------------------------------------------------------------------------

#ifndef _WIN32
static int resize_gpt_data_partition(const std::string& device) {
    int fd = open(device.c_str(), O_RDWR | O_SYNC);
    if (fd < 0) {
        fprintf(stderr, "[installer] Cannot open %s for GPT resize: %s\n",
                device.c_str(), strerror(errno));
        return -1;
    }

    // Get disk size in bytes
    uint64_t disk_size = 0;
    if (ioctl(fd, BLKGETSIZE64, &disk_size) != 0) {
        fprintf(stderr, "[installer] Cannot get disk size: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    uint64_t disk_sectors = disk_size / 512;
    fprintf(stderr, "[installer] Disk size: %llu bytes (%llu sectors)\n",
            (unsigned long long)disk_size, (unsigned long long)disk_sectors);

    // Read primary GPT header at LBA 1
    GPTHeader header;
    if (pread(fd, &header, sizeof(header), 512) != sizeof(header)) {
        fprintf(stderr, "[installer] Cannot read GPT header: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    // Verify GPT signature
    if (memcmp(header.signature, "EFI PART", 8) != 0) {
        fprintf(stderr, "[installer] Invalid GPT signature\n");
        close(fd);
        return -1;
    }

    fprintf(stderr, "[installer] GPT: %u partition entries, %u bytes each, at LBA %llu\n",
            header.num_partition_entries, header.partition_entry_size,
            (unsigned long long)header.partition_entry_lba);

    // Read all partition entries
    uint32_t entries_size = header.num_partition_entries * header.partition_entry_size;
    uint8_t* entries = (uint8_t*)malloc(entries_size);
    if (!entries) {
        close(fd);
        return -1;
    }

    off_t entries_offset = header.partition_entry_lba * 512;
    if (pread(fd, entries, entries_size, entries_offset) != (ssize_t)entries_size) {
        fprintf(stderr, "[installer] Cannot read partition entries: %s\n", strerror(errno));
        free(entries);
        close(fd);
        return -1;
    }

    // Find partition 5 (0-indexed = entry 4)
    if (header.num_partition_entries < 5) {
        fprintf(stderr, "[installer] Not enough partition entries for DATA partition\n");
        free(entries);
        close(fd);
        return -1;
    }

    GPTEntry* part5 = (GPTEntry*)(entries + 4 * header.partition_entry_size);

    // Check if partition 5 exists (has a type GUID)
    bool has_type = false;
    for (int i = 0; i < 16; i++) {
        if (part5->type_guid[i] != 0) { has_type = true; break; }
    }
    if (!has_type) {
        fprintf(stderr, "[installer] Partition 5 has no type GUID (not used)\n");
        free(entries);
        close(fd);
        return -1;
    }

    // Calculate new end LBA for partition 5:
    // Last usable LBA = disk_sectors - 34 (33 sectors for backup GPT + 1)
    // The backup GPT needs: 32 sectors for entries + 1 sector for header = 33 sectors
    // So last usable LBA = disk_sectors - 34
    uint64_t new_last_usable = disk_sectors - 34;
    uint64_t old_end = part5->ending_lba;

    if (new_last_usable <= part5->starting_lba) {
        fprintf(stderr, "[installer] Disk too small for partition resize\n");
        free(entries);
        close(fd);
        return -1;
    }

    if (new_last_usable <= old_end) {
        fprintf(stderr, "[installer] Partition 5 already at maximum size (end LBA %llu)\n",
                (unsigned long long)old_end);
        free(entries);
        close(fd);
        return 0;  // Not an error
    }

    fprintf(stderr, "[installer] Resizing partition 5: end LBA %llu -> %llu (%.1f GB)\n",
            (unsigned long long)old_end, (unsigned long long)new_last_usable,
            (double)(new_last_usable - part5->starting_lba + 1) * 512.0 / (1024.0 * 1024.0 * 1024.0));

    // Update partition 5 end LBA
    part5->ending_lba = new_last_usable;

    // Update the GPT header's last_usable_lba
    header.last_usable_lba = new_last_usable;

    // Update alternate (backup) header LBA to end of disk
    header.alternate_lba = disk_sectors - 1;

    // Recalculate partition entry array CRC32
    header.partition_array_crc32 = crc32_gpt(entries, entries_size);

    // Recalculate header CRC32 (must zero the CRC field first)
    header.header_crc32 = 0;
    header.header_crc32 = crc32_gpt((const uint8_t*)&header, header.header_size);

    // Write updated partition entries (primary, at partition_entry_lba)
    if (pwrite(fd, entries, entries_size, entries_offset) != (ssize_t)entries_size) {
        fprintf(stderr, "[installer] Cannot write partition entries: %s\n", strerror(errno));
        free(entries);
        close(fd);
        return -1;
    }

    // Write updated primary GPT header (LBA 1)
    if (pwrite(fd, &header, sizeof(header), 512) != sizeof(header)) {
        fprintf(stderr, "[installer] Cannot write GPT header: %s\n", strerror(errno));
        free(entries);
        close(fd);
        return -1;
    }

    // Write backup GPT at end of disk:
    // Backup partition entries go at (disk_sectors - 33) * 512
    // Backup header goes at (disk_sectors - 1) * 512
    off_t backup_entries_offset = (disk_sectors - 33) * 512;
    if (pwrite(fd, entries, entries_size, backup_entries_offset) != (ssize_t)entries_size) {
        fprintf(stderr, "[installer] Warning: Cannot write backup partition entries\n");
        // Non-fatal — primary GPT is already updated
    }

    // Create backup header (swap my_lba and alternate_lba, update partition_entry_lba)
    GPTHeader backup_header = header;
    backup_header.my_lba = disk_sectors - 1;
    backup_header.alternate_lba = 1;  // Points to primary
    backup_header.partition_entry_lba = disk_sectors - 33;
    // Recalculate backup header CRC
    backup_header.header_crc32 = 0;
    backup_header.header_crc32 = crc32_gpt((const uint8_t*)&backup_header, backup_header.header_size);

    off_t backup_header_offset = (disk_sectors - 1) * 512;
    if (pwrite(fd, &backup_header, sizeof(backup_header), backup_header_offset) != sizeof(backup_header)) {
        fprintf(stderr, "[installer] Warning: Cannot write backup GPT header\n");
    }

    // ---------------------------------------------------------------
    // Update the Protective MBR to cover the full disk
    // Without this, EFI firmware (especially VirtualBox) rejects the GPT
    // because the PMBR size doesn't match the actual disk size.
    // ---------------------------------------------------------------
    uint8_t mbr[512];
    if (pread(fd, mbr, 512, 0) == 512) {
        // MBR partition entry 1 starts at offset 446, is 16 bytes
        // Bytes 12-15 = number of sectors (little-endian uint32_t)
        // Per GPT spec: type 0xEE, start LBA 1, size = min(disk_sectors-1, 0xFFFFFFFF)
        uint32_t pmbr_size;
        if (disk_sectors - 1 > 0xFFFFFFFFULL) {
            pmbr_size = 0xFFFFFFFF;  // Disk > 2 TB, use maximum
        } else {
            pmbr_size = (uint32_t)(disk_sectors - 1);
        }
        // Write the size into the PMBR partition entry (offset 446 + 12 = 458)
        memcpy(mbr + 458, &pmbr_size, 4);

        // Also update the CHS end address to FE/FF/FF (maximum CHS for protective MBR)
        // MBR entry layout at offset 446: [boot(1)] [CHS_start(3)] [type(1)] [CHS_end(3)] [LBA(4)] [size(4)]
        // CHS end = offset 446 + 5 = 451 (head), 452 (sector+cyl_hi), 453 (cyl_lo)
        mbr[451] = 0xFE;  // End head
        mbr[452] = 0xFF;  // End sector + cylinder high bits
        mbr[453] = 0xFF;  // End cylinder low bits

        if (pwrite(fd, mbr, 512, 0) != 512) {
            fprintf(stderr, "[installer] Warning: Cannot update protective MBR\n");
        } else {
            fprintf(stderr, "[installer] Protective MBR updated: size = %u sectors\n", pmbr_size);
        }
    } else {
        fprintf(stderr, "[installer] Warning: Cannot read MBR for PMBR update\n");
    }

    fsync(fd);
    free(entries);

    // Tell kernel to re-read partition table
    ioctl(fd, BLKRRPART, 0);
    close(fd);

    fprintf(stderr, "[installer] GPT partition 5 resized successfully\n");
    return 0;
}
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Minimal ext4 formatting using fork/exec (no shell needed)
// If mkfs.ext4 binary is available, use it; otherwise skip (non-fatal)
// ---------------------------------------------------------------------------

#ifndef _WIN32
static int try_format_ext4(const std::string& partition) {
    // Check if mkfs.ext4 exists anywhere
    const char* mkfs_paths[] = {
        "/sbin/mkfs.ext4",
        "/usr/sbin/mkfs.ext4",
        "/bin/mkfs.ext4",
        "/usr/bin/mkfs.ext4",
        "/sbin/mke2fs",
        "/usr/sbin/mke2fs",
        nullptr
    };

    const char* mkfs_path = nullptr;
    for (int i = 0; mkfs_paths[i]; i++) {
        if (access(mkfs_paths[i], X_OK) == 0) {
            mkfs_path = mkfs_paths[i];
            break;
        }
    }

    if (!mkfs_path) {
        fprintf(stderr, "[installer] mkfs.ext4 not found — skipping DATA partition format\n");
        fprintf(stderr, "[installer] The DATA partition will use the pre-existing filesystem from the image\n");
        return -1;  // Non-fatal
    }

    fprintf(stderr, "[installer] Formatting DATA partition with %s\n", mkfs_path);

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        // Child process: run mkfs.ext4
        // -F: force. After writing the image the partition already holds an ext4
        // fs (labelled DATA); without -F, mkfs.ext4 prompts "Proceed anyway?" and
        // hangs forever (the installer child has no stdin).
        const char* argv[] = {mkfs_path, "-F", "-q", "-L", "DATA", partition.c_str(), nullptr};
        execv(mkfs_path, (char* const*)argv);
        _exit(127);  // exec failed
    }

    // Parent: wait for child
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[installer] DATA partition formatted successfully\n");
        return 0;
    }

    fprintf(stderr, "[installer] mkfs.ext4 returned %d — DATA partition may use existing filesystem\n",
            WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    return -1;
}
#endif

// ---------------------------------------------------------------------------
// install.detect_disks — Scan for available target disks
// ---------------------------------------------------------------------------

static std::string handle_detect_disks(const std::string& /*args*/) {
#ifndef _WIN32
    std::string boot_dev = find_boot_device();

    json disks = json::array();

    DIR* dir = opendir("/sys/block");
    if (!dir) {
        json err;
        err["error"] = "Cannot read /sys/block";
        return err.dump();
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // Skip . and ..
        if (name == "." || name == "..") continue;

        // Skip non-disk devices
        if (name.find("loop") == 0) continue;
        if (name.find("ram") == 0) continue;
        if (name.find("dm-") == 0) continue;
        if (name.find("sr") == 0) continue;   // CD-ROM
        if (name.find("fd") == 0) continue;   // Floppy

        // Skip the boot device
        if (name == boot_dev) continue;

        std::string base = "/sys/block/" + name;

        // Check if it's a real device (has a 'size' entry)
        std::string size_str = read_sysfs(base + "/size");
        if (size_str.empty()) continue;

        long long sectors = 0;
        try { sectors = std::stoll(size_str); } catch (...) { continue; }
        if (sectors == 0) continue;

        double size_gb = (sectors * 512.0) / (1024.0 * 1024.0 * 1024.0);

        // Skip tiny devices (< 2 GB)
        if (size_gb < 2.0) continue;

        // Read model name
        std::string model = read_sysfs(base + "/device/model");
        if (model.empty())
            model = read_sysfs(base + "/device/name");
        if (model.empty())
            model = "Unknown";

        // Count partitions
        int part_count = 0;
        DIR* part_dir = opendir(base.c_str());
        if (part_dir) {
            struct dirent* pentry;
            while ((pentry = readdir(part_dir)) != nullptr) {
                std::string pname = pentry->d_name;
                if (pname.find(name) == 0 && pname.size() > name.size())
                    part_count++;
            }
            closedir(part_dir);
        }

        // Check if removable
        std::string removable = read_sysfs(base + "/removable");

        json disk;
        disk["device"] = "/dev/" + name;
        disk["name"] = name;
        disk["size_gb"] = static_cast<int>(size_gb * 10) / 10.0;  // 1 decimal
        disk["size_sectors"] = sectors;
        disk["model"] = model;
        disk["partitions"] = part_count;
        disk["removable"] = (removable == "1");
        disks.push_back(disk);
    }
    closedir(dir);

    return disks.dump();
#else
    json err;
    err["error"] = "Disk detection not supported on Windows";
    return err.dump();
#endif
}

// ---------------------------------------------------------------------------
// install.to_disk — Write Llamaste image to target disk (pure C++)
// ---------------------------------------------------------------------------

static void install_worker(const std::string& device) {
    g_install_progress.set_status("Starting installation to " + device);
    g_install_progress.percent = 0;

#ifndef _WIN32
    // Safety: verify the device is not the boot device
    std::string boot_dev = find_boot_device();
    std::string target_name = device;
    if (target_name.find("/dev/") == 0)
        target_name = target_name.substr(5);
    std::string target_base = target_name;
    while (!target_base.empty() && (target_base.back() >= '0' && target_base.back() <= '9'))
        target_base.pop_back();
    if (!target_base.empty() && target_base.back() == 'p')
        target_base.pop_back();

    if (target_base == boot_dev) {
        g_install_progress.set_error("Refusing to write to boot device: " + device);
        g_install_progress.finished = true;
        return;
    }

    // Verify target device exists
    struct stat st;
    if (stat(device.c_str(), &st) != 0) {
        g_install_progress.set_error("Device not found: " + device);
        g_install_progress.finished = true;
        return;
    }

    // Get target disk size
    int fd = open(device.c_str(), O_RDONLY);
    if (fd < 0) {
        g_install_progress.set_error("Cannot open device: " + device + " (" + strerror(errno) + ")");
        g_install_progress.finished = true;
        return;
    }
    uint64_t disk_size = 0;
    ioctl(fd, BLKGETSIZE64, &disk_size);
    close(fd);

    if (disk_size < (uint64_t)512 * 1024 * 1024) {  // Minimum 512 MB
        g_install_progress.set_error("Disk too small: " + std::to_string(disk_size / (1024*1024)) + " MB (need 512 MB+)");
        g_install_progress.finished = true;
        return;
    }

    // ---------------------------------------------------------------
    // Step 1: Find the installation image
    // ---------------------------------------------------------------
    g_install_progress.set_status("Looking for installation image...");
    g_install_progress.percent = 5;

    // Check for XZ compressed image first, then raw image
    std::vector<std::string> xz_paths = {
        "/install/llamaste.img.xz",
        "/media/cdrom/install/llamaste.img.xz",
        "/cdrom/install/llamaste.img.xz",
    };
    std::vector<std::string> raw_paths = {
        "/install/llamaste.img",
        "/media/cdrom/install/llamaste.img",
        "/cdrom/install/llamaste.img",
    };

    std::string img_source;
    bool is_xz = false;
    uint64_t src_file_size = 0;

#ifdef HAVE_LZMA
    // Try XZ first (preferred, smaller ISO)
    for (const auto& p : xz_paths) {
        if (stat(p.c_str(), &st) == 0) {
            img_source = p;
            is_xz = true;
            src_file_size = st.st_size;
            break;
        }
    }
#endif

    // Fallback to raw image
    if (img_source.empty()) {
        for (const auto& p : raw_paths) {
            if (stat(p.c_str(), &st) == 0) {
                img_source = p;
                is_xz = false;
                src_file_size = st.st_size;
                break;
            }
        }
    }

    // If no raw image found, also try XZ paths (even without HAVE_LZMA, for error msg)
    if (img_source.empty()) {
        for (const auto& p : xz_paths) {
            if (stat(p.c_str(), &st) == 0) {
                img_source = p;
                is_xz = true;
                src_file_size = st.st_size;
                break;
            }
        }
        if (!img_source.empty() && is_xz) {
#ifndef HAVE_LZMA
            g_install_progress.set_error("Found " + img_source + " but XZ decompression not available (liblzma not linked)");
            g_install_progress.finished = true;
            return;
#endif
        }
    }

    if (img_source.empty()) {
        g_install_progress.set_error("Installation image not found. Checked /install/ for llamaste.img and llamaste.img.xz");
        g_install_progress.finished = true;
        return;
    }

    g_install_progress.set_status("Found image: " + img_source +
        " (" + std::to_string(src_file_size / (1024*1024)) + " MB" +
        (is_xz ? ", XZ compressed" : ", raw") + ")");
    g_install_progress.percent = 10;

    // ---------------------------------------------------------------
    // Step 2: Write image to disk (pure C++, no shell, no dd)
    // ---------------------------------------------------------------
    g_install_progress.set_status("Writing image to " + device + "...");
    int ret;

    if (is_xz) {
#ifdef HAVE_LZMA
        ret = decompress_xz_to_device(img_source, device, src_file_size, 10, 60);
#else
        g_install_progress.set_error("XZ decompression not available");
        g_install_progress.finished = true;
        return;
#endif
    } else {
        ret = copy_file_to_device(img_source, device, src_file_size, 10, 60);
    }

    if (ret != 0) {
        // Error already set by copy/decompress function
        g_install_progress.finished = true;
        return;
    }

    g_install_progress.set_status("Image written successfully");
    g_install_progress.percent = 60;

    // ---------------------------------------------------------------
    // Step 3: Re-read partition table
    // ---------------------------------------------------------------
    g_install_progress.set_status("Re-reading partition table...");

    fd = open(device.c_str(), O_RDWR);
    if (fd >= 0) {
        ioctl(fd, BLKRRPART, 0);
        close(fd);
    }

    // Give the kernel a moment to process
    std::this_thread::sleep_for(std::chrono::seconds(2));
    g_install_progress.percent = 65;

    // ---------------------------------------------------------------
    // Step 4: Resize DATA partition (partition 5) to fill remaining disk
    // This is pure C++ GPT manipulation — no external tools needed
    // ---------------------------------------------------------------
    g_install_progress.set_status("Resizing DATA partition to fill disk...");

    ret = resize_gpt_data_partition(device);
    if (ret != 0) {
        fprintf(stderr, "[installer] Warning: Could not resize DATA partition (non-fatal)\n");
    }

    g_install_progress.percent = 75;

    // ---------------------------------------------------------------
    // Step 5: Format the expanded DATA partition
    // Try to use mkfs.ext4 if available, otherwise skip (non-fatal)
    // ---------------------------------------------------------------
    g_install_progress.set_status("Formatting DATA partition...");

    // Determine the partition naming scheme
    std::string part_prefix;
    if (target_name.find("nvme") == 0 || target_name.find("mmcblk") == 0)
        part_prefix = device + "p";
    else
        part_prefix = device;
    std::string data_part = part_prefix + "5";

    // Wait for partition device to appear
    for (int i = 0; i < 10; i++) {
        if (stat(data_part.c_str(), &st) == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    ret = try_format_ext4(data_part);
    // Non-fatal if this fails — the partition has the original ext4 from the image

    g_install_progress.percent = 85;

    // ---------------------------------------------------------------
    // Step 6: Initialize data directories on the new partition
    // ---------------------------------------------------------------
    g_install_progress.set_status("Initializing data directories...");

    std::string mount_point = "/tmp/install-data";
    mkdir(mount_point.c_str(), 0755);

    if (mount(data_part.c_str(), mount_point.c_str(), "ext4", 0, nullptr) == 0) {
        const char* dirs[] = {
            "models", "llamaste", "llamaste/conversations",
            "llamaste/config", "llamaste/logs", "llamaste/skills",
            "llamaste/cache", nullptr
        };
        for (int i = 0; dirs[i]; i++) {
            std::string dir = mount_point + "/" + dirs[i];
            mkdir(dir.c_str(), 0755);
        }

        // Write default config
        std::string config_path = mount_point + "/llamaste/config/llamaste.json";
        std::ofstream cfg(config_path);
        if (cfg.is_open()) {
            cfg << "{\n"
                << "    \"version\": 1,\n"
                << "    \"model\": \"auto\",\n"
                << "    \"listen\": \"0.0.0.0\",\n"
                << "    \"port\": 80,\n"
                << "    \"threads\": 0,\n"
                << "    \"context_size\": 2048,\n"
                << "    \"log_level\": \"info\"\n"
                << "}\n";
        }

        umount(mount_point.c_str());
        fprintf(stderr, "[installer] Data directories initialized\n");
    } else {
        fprintf(stderr, "[installer] Warning: Could not mount DATA partition for initialization (%s)\n",
                strerror(errno));
    }

    // Step 7 removed: initramfs now handles root device discovery at boot.
    // No ESP patching needed — grub.cfg + initramfs.cpio.gz on ESP scan
    // all block devices for the squashfs root partition (NVMe, SATA, virtio).

    g_install_progress.percent = 100;
    g_install_progress.set_status("Installation complete! Remove the installation media and reboot.");
    g_install_progress.success = true;
    g_install_progress.finished = true;

#else
    g_install_progress.set_error("Installation not supported on Windows");
    g_install_progress.finished = true;
#endif
}

static std::string handle_install_to_disk(const std::string& args) {
    auto params = json::parse(args, nullptr, false);

    if (params.is_discarded() || !params.contains("device")) {
        json err;
        err["error"] = "Missing required parameter: device";
        return err.dump();
    }

    std::string device = params["device"].get<std::string>();
    bool confirm = params.value("confirm", false);

    if (!confirm) {
        json err;
        err["error"] = "Installation requires confirm: true. WARNING: This will erase ALL data on " + device;
        return err.dump();
    }

    // Check if already running
    if (g_install_progress.running) {
        json err;
        err["error"] = "Installation already in progress";
        return err.dump();
    }

    // Start installation in background thread
    g_install_progress.reset();
    g_install_progress.running = true;

    std::thread worker(install_worker, device);
    worker.detach();

    json result;
    result["started"] = true;
    result["device"] = device;
    result["message"] = "Installation started. Monitor progress via /install/progress";
    return result.dump();
}

// ---------------------------------------------------------------------------
// install.progress — Check installation progress
// ---------------------------------------------------------------------------

static std::string handle_install_progress(const std::string& /*args*/) {
    json result;
    result["percent"] = g_install_progress.percent.load();
    result["running"] = g_install_progress.running.load();
    result["finished"] = g_install_progress.finished.load();
    result["success"] = g_install_progress.success.load();
    result["status"] = g_install_progress.get_status();

    std::string err = g_install_progress.get_error();
    if (!err.empty())
        result["error"] = err;

    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_install_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "install.detect_disks",
        .description = "Detect available disk drives for installation. Returns a list of block devices "
                       "with their size, model, and partition count. Excludes the boot device and "
                       "devices smaller than 2 GB.",
        .parameters = R"json({
            "type": "object",
            "properties": {},
            "required": []
        })json",
        .handler = handle_detect_disks,
        .requires_confirmation = false
    });

    reg.register_tool({
        .name = "install.to_disk",
        .description = "Install Llamaste to a target disk. Writes the disk image, resizes the DATA "
                       "partition to fill the disk, and initializes the data directories. "
                       "WARNING: This erases ALL data on the target disk.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "device": {
                    "type": "string",
                    "description": "Target device path (e.g., /dev/sda, /dev/nvme0n1)"
                },
                "confirm": {
                    "type": "boolean",
                    "description": "Must be true to proceed. Confirms intent to erase the target disk."
                }
            },
            "required": ["device", "confirm"]
        })json",
        .handler = handle_install_to_disk,
        .requires_confirmation = true
    });

    reg.register_tool({
        .name = "install.progress",
        .description = "Check the progress of an ongoing installation. Returns percent complete, "
                       "current status message, and whether the installation has finished.",
        .parameters = R"json({
            "type": "object",
            "properties": {},
            "required": []
        })json",
        .handler = handle_install_progress,
        .requires_confirmation = false
    });
}

// Expose progress for HTTP endpoints
const InstallProgress& get_install_progress() {
    return g_install_progress;
}
