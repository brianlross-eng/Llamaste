// tools_install.cpp -- Installer tools for live ISO mode
//
// Provides tools for detecting target disks and installing Llamaste
// to a local hard drive from the live ISO environment.
//
// These tools are only registered when running in live (ISO) mode.

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
// Helper: run a command and return exit code
// ---------------------------------------------------------------------------

#ifndef _WIN32
static int run_command(const std::string& cmd) {
    fprintf(stderr, "[installer] Running: %s\n", cmd.c_str());
    int ret = system(cmd.c_str());
    if (WIFEXITED(ret))
        return WEXITSTATUS(ret);
    return -1;
}
#endif

// ---------------------------------------------------------------------------
// Helper: find the boot device (the device we booted from, which we must not
// write to). For ISO boot, this is typically sr0 or the USB device.
// ---------------------------------------------------------------------------

static std::string find_boot_device() {
    // Read /proc/cmdline to find root= parameter
    std::ifstream f("/proc/cmdline");
    std::string cmdline;
    if (std::getline(f, cmdline)) {
        // Look for root=LABEL= or root=/dev/
        auto pos = cmdline.find("root=");
        if (pos != std::string::npos) {
            std::string root_spec = cmdline.substr(pos + 5);
            auto space = root_spec.find(' ');
            if (space != std::string::npos)
                root_spec = root_spec.substr(0, space);
            // If it's a device path, extract the base device
            if (root_spec.find("/dev/") == 0) {
                // /dev/sda3 -> sda, /dev/nvme0n1p3 -> nvme0n1
                std::string dev = root_spec.substr(5);
                // Remove partition number suffix
                while (!dev.empty() && (dev.back() >= '0' && dev.back() <= '9'))
                    dev.pop_back();
                if (!dev.empty() && dev.back() == 'p')
                    dev.pop_back();  // nvme0n1p -> nvme0n1
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
        if (name.find("sr") == 0) continue;  // CD-ROM
        if (name.find("fd") == 0) continue;  // Floppy

        // Skip the boot device
        if (name == boot_dev) continue;

        std::string base = "/sys/block/" + name;

        // Check if it's a real device (has a 'device' or 'size' entry)
        std::string size_str = read_sysfs(base + "/size");
        if (size_str.empty()) continue;

        long long sectors = 0;
        try { sectors = std::stoll(size_str); } catch (...) { continue; }
        if (sectors == 0) continue;

        double size_gb = (sectors * 512.0) / (1024.0 * 1024.0 * 1024.0);

        // Skip tiny devices (< 2 GB, probably not a real disk)
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
// install.to_disk — Write Llamaste image to target disk
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
    // Strip partition suffix for comparison
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
        g_install_progress.set_error("Cannot open device: " + device);
        g_install_progress.finished = true;
        return;
    }
    uint64_t disk_size = 0;
    ioctl(fd, BLKGETSIZE64, &disk_size);
    close(fd);

    if (disk_size < (uint64_t)1024 * 1024 * 1024) {  // Minimum 1 GB
        g_install_progress.set_error("Disk too small: " + std::to_string(disk_size / (1024*1024)) + " MB");
        g_install_progress.finished = true;
        return;
    }

    // ---------------------------------------------------------------
    // Step 1: Find and decompress the installation image
    // ---------------------------------------------------------------
    g_install_progress.set_status("Looking for installation image...");
    g_install_progress.percent = 5;

    // The image could be at several locations depending on how the ISO is mounted
    std::vector<std::string> img_paths = {
        "/install/llamaste.img.xz",
        "/media/cdrom/install/llamaste.img.xz",
        "/cdrom/install/llamaste.img.xz",
        "/mnt/iso/install/llamaste.img.xz",
    };

    std::string img_source;
    for (const auto& p : img_paths) {
        if (stat(p.c_str(), &st) == 0) {
            img_source = p;
            break;
        }
    }

    if (img_source.empty()) {
        // Try uncompressed image
        std::vector<std::string> raw_paths = {
            "/install/llamaste.img",
            "/media/cdrom/install/llamaste.img",
            "/cdrom/install/llamaste.img",
        };
        for (const auto& p : raw_paths) {
            if (stat(p.c_str(), &st) == 0) {
                img_source = p;
                break;
            }
        }
    }

    if (img_source.empty()) {
        g_install_progress.set_error("Installation image not found. Checked /install/ and /media/cdrom/install/");
        g_install_progress.finished = true;
        return;
    }

    g_install_progress.set_status("Found image: " + img_source);
    g_install_progress.percent = 10;

    // ---------------------------------------------------------------
    // Step 2: Write image to disk
    // ---------------------------------------------------------------
    g_install_progress.set_status("Writing image to " + device + "...");

    bool is_xz = (img_source.find(".xz") != std::string::npos);
    std::string dd_cmd;
    if (is_xz) {
        dd_cmd = "xz -dc '" + img_source + "' | dd of='" + device + "' bs=4M status=none 2>/dev/null";
    } else {
        dd_cmd = "dd if='" + img_source + "' of='" + device + "' bs=4M status=none 2>/dev/null";
    }

    // Track progress by checking bytes written periodically
    // For simplicity in Phase 1, we just run the command and update progress
    // in estimated steps
    g_install_progress.percent = 15;

    int ret = run_command(dd_cmd);
    if (ret != 0) {
        g_install_progress.set_error("Failed to write image to disk (exit code " + std::to_string(ret) + ")");
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
    // Also try partprobe/blockdev
    run_command("partprobe '" + device + "' 2>/dev/null || blockdev --rereadpt '" + device + "' 2>/dev/null || true");

    // Give the kernel a moment to process
    std::this_thread::sleep_for(std::chrono::seconds(2));

    g_install_progress.percent = 70;

    // ---------------------------------------------------------------
    // Step 4: Resize DATA partition (partition 5) to fill remaining disk
    // ---------------------------------------------------------------
    g_install_progress.set_status("Resizing DATA partition...");

    // Determine the partition naming scheme
    std::string part_prefix;
    if (target_name.find("nvme") == 0 || target_name.find("mmcblk") == 0)
        part_prefix = device + "p";
    else
        part_prefix = device;

    std::string data_part = part_prefix + "5";

    // Use sfdisk to resize partition 5 to fill remaining space
    // The '--' is needed for the command to work without interactive input
    std::string sfdisk_cmd = "echo ',+' | sfdisk -N 5 '" + device + "' --no-reread 2>/dev/null";
    ret = run_command(sfdisk_cmd);

    if (ret != 0) {
        // Try sgdisk as fallback
        std::string sgdisk_cmd = "sgdisk -e -d 5 -n 5:0:0 -t 5:8300 -c 5:data '" + device + "' 2>/dev/null";
        ret = run_command(sgdisk_cmd);
        if (ret != 0) {
            fprintf(stderr, "[installer] Warning: Could not resize DATA partition (non-fatal)\n");
        }
    }

    // Re-read partition table again
    fd = open(device.c_str(), O_RDWR);
    if (fd >= 0) {
        ioctl(fd, BLKRRPART, 0);
        close(fd);
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));

    g_install_progress.percent = 80;

    // ---------------------------------------------------------------
    // Step 5: Format the DATA partition
    // ---------------------------------------------------------------
    g_install_progress.set_status("Formatting DATA partition...");

    // Check if resize2fs is available (resize existing filesystem)
    std::string resize_cmd = "resize2fs '" + data_part + "' 2>/dev/null";
    ret = run_command(resize_cmd);

    if (ret != 0) {
        // Fallback: reformat the partition
        std::string mkfs_cmd = "mkfs.ext4 -q -L DATA '" + data_part + "' 2>/dev/null";
        ret = run_command(mkfs_cmd);
        if (ret != 0) {
            fprintf(stderr, "[installer] Warning: Could not format DATA partition (non-fatal)\n");
        }
    }

    g_install_progress.percent = 90;

    // ---------------------------------------------------------------
    // Step 6: Create data directories on the new partition
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
    } else {
        fprintf(stderr, "[installer] Warning: Could not mount DATA partition for initialization\n");
    }

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
