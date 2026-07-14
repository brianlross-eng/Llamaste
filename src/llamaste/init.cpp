#include "init.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <unistd.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/statfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "netlink_route.h"
#include <linux/fs.h>
#include <linux/blkpg.h>
#include <linux/loop.h>

// EXT4 online resize ioctl — grows a mounted ext4 filesystem
#ifndef EXT4_IOC_RESIZE_FS
#define EXT4_IOC_RESIZE_FS _IOW('f', 16, __u64)
#endif

// ---- GPT structures (matches tools_install.cpp) ----

struct __attribute__((packed)) GPTHeader {
    char     signature[8];      // "EFI PART"
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
};

struct __attribute__((packed)) GPTEntry {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t starting_lba;
    uint64_t ending_lba;
    uint64_t attributes;
    uint16_t name[36];
};

static uint32_t crc32_gpt(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// Derive whole-disk device from partition device path.
// /dev/sda5 → /dev/sda, /dev/nvme0n1p5 → /dev/nvme0n1
static std::string disk_device_for(const char* part_dev) {
    std::string s(part_dev);
    if (s.find("nvme") != std::string::npos ||
        s.find("loop") != std::string::npos ||
        s.find("mmcblk") != std::string::npos) {
        // Pattern: /dev/nvme0n1p5 → strip trailing pN
        auto p = s.rfind('p');
        if (p != std::string::npos && p > 4)
            return s.substr(0, p);
    } else {
        // Pattern: /dev/sda5 → strip trailing digits
        size_t end = s.size();
        while (end > 0 && s[end - 1] >= '0' && s[end - 1] <= '9') end--;
        if (end > 0 && end < s.size())
            return s.substr(0, end);
    }
    return "";
}

// Extract partition number from device path.
// /dev/sda5 → 5, /dev/nvme0n1p5 → 5
static int partition_number_from(const char* dev) {
    size_t len = strlen(dev);
    size_t end = len;
    while (end > 0 && dev[end - 1] >= '0' && dev[end - 1] <= '9') end--;
    if (end < len) return atoi(dev + end);
    return -1;
}

// Log helper: writes to both stderr and /tmp/init-resize.log
static FILE* resize_log = nullptr;
static void rlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    if (!resize_log) resize_log = fopen("/tmp/init-resize.log", "w");
    if (resize_log) {
        va_list ap2;
        va_start(ap2, fmt);
        vfprintf(resize_log, fmt, ap2);
        va_end(ap2);
        fflush(resize_log);
    }
}

// Try to grow a GPT partition to fill remaining disk space.
// Only grows if the partition is the last one on disk and there's
// significant unallocated space after it (>1 MB).
static void grow_gpt_partition(const char* part_dev) {
    rlog("[init] grow_gpt_partition: probing %s\n", part_dev);

    std::string disk_dev = disk_device_for(part_dev);
    if (disk_dev.empty()) { rlog("[init] GPT: cannot derive disk device\n"); return; }

    int part_num = partition_number_from(part_dev);
    if (part_num < 1) { rlog("[init] GPT: invalid partition number\n"); return; }
    int part_index = part_num - 1; // 0-based GPT entry index
    rlog("[init] GPT: disk=%s part_num=%d index=%d\n",
         disk_dev.c_str(), part_num, part_index);

    int fd = open(disk_dev.c_str(), O_RDWR | O_SYNC);
    if (fd < 0) {
        rlog("[init] GPT: cannot open %s O_RDWR: %m\n", disk_dev.c_str());
        return;
    }

    // Get total disk size
    uint64_t disk_bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &disk_bytes) != 0 || disk_bytes < 1048576) {
        rlog("[init] GPT: BLKGETSIZE64 failed or disk too small (%lu bytes)\n",
             (unsigned long)disk_bytes);
        close(fd);
        return;
    }
    uint64_t disk_sectors = disk_bytes / 512;
    rlog("[init] GPT: disk_bytes=%lu disk_sectors=%lu (%lu MB)\n",
         (unsigned long)disk_bytes, (unsigned long)disk_sectors,
         (unsigned long)(disk_bytes / (1024*1024)));

    // Read primary GPT header at LBA 1
    GPTHeader hdr;
    if (pread(fd, &hdr, sizeof(hdr), 512) != sizeof(hdr)) {
        rlog("[init] GPT: cannot read header at LBA 1\n");
        close(fd);
        return;
    }
    if (memcmp(hdr.signature, "EFI PART", 8) != 0) {
        rlog("[init] GPT: bad signature (not EFI PART)\n");
        close(fd);
        return;
    }
    rlog("[init] GPT: header OK, entries=%u entry_size=%u last_usable=%lu\n",
         hdr.num_partition_entries, hdr.partition_entry_size,
         (unsigned long)hdr.last_usable_lba);

    // Validate entry index in range
    if ((uint32_t)part_index >= hdr.num_partition_entries ||
        hdr.partition_entry_size < sizeof(GPTEntry)) {
        rlog("[init] GPT: part_index %d out of range (max %u) or entry_size %u too small\n",
             part_index, hdr.num_partition_entries, hdr.partition_entry_size);
        close(fd);
        return;
    }

    // Read all partition entries
    size_t entries_bytes = (size_t)hdr.num_partition_entries * hdr.partition_entry_size;
    std::vector<uint8_t> entries(entries_bytes);
    if (pread(fd, entries.data(), entries_bytes,
              hdr.partition_entry_lba * 512) != (ssize_t)entries_bytes) {
        rlog("[init] GPT: cannot read %zu bytes of entries at LBA %lu\n",
             entries_bytes, (unsigned long)hdr.partition_entry_lba);
        close(fd);
        return;
    }

    GPTEntry* target = (GPTEntry*)(entries.data() +
                                    (size_t)part_index * hdr.partition_entry_size);

    rlog("[init] GPT: partition %d: start_lba=%lu end_lba=%lu (%lu MB)\n",
         part_num, (unsigned long)target->starting_lba,
         (unsigned long)target->ending_lba,
         (unsigned long)((target->ending_lba - target->starting_lba + 1) * 512 / (1024*1024)));

    // Check the partition exists (has a non-zero type GUID)
    bool all_zero = true;
    for (int i = 0; i < 16; i++) {
        if (target->type_guid[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) {
        rlog("[init] GPT: partition %d has zero type GUID (empty)\n", part_num);
        close(fd);
        return;
    }

    // Make sure this is the last partition (no other partition ends after it)
    for (uint32_t i = 0; i < hdr.num_partition_entries; i++) {
        if ((int)i == part_index) continue;
        GPTEntry* other = (GPTEntry*)(entries.data() +
                                       (size_t)i * hdr.partition_entry_size);
        if (other->ending_lba > target->ending_lba) {
            rlog("[init] GPT: partition %u (end_lba=%lu) is beyond our partition (end_lba=%lu)\n",
                 i + 1, (unsigned long)other->ending_lba, (unsigned long)target->ending_lba);
            close(fd);
            return;
        }
    }

    // Calculate maximum expansion
    // Reserve 33 sectors at end of disk for backup GPT (32 entries + 1 header)
    uint64_t new_last_usable = disk_sectors - 34;
    uint64_t growth = new_last_usable > target->ending_lba
                    ? new_last_usable - target->ending_lba : 0;

    rlog("[init] GPT: new_last_usable=%lu growth=%lu sectors (%lu MB)\n",
         (unsigned long)new_last_usable, (unsigned long)growth,
         (unsigned long)(growth * 512 / (1024*1024)));

    // Only grow if there's at least 2 MB of room
    if (growth < 4096) { // 4096 sectors = 2 MB
        rlog("[init] GPT: growth too small (%lu sectors), skipping\n",
             (unsigned long)growth);
        close(fd);
        return;
    }

    uint64_t old_end = target->ending_lba;
    target->ending_lba = new_last_usable;

    // Update GPT header
    hdr.last_usable_lba = new_last_usable;
    hdr.alternate_lba = disk_sectors - 1;

    // Recalculate CRC32s
    hdr.partition_array_crc32 = crc32_gpt(entries.data(), entries_bytes);
    hdr.header_crc32 = 0;
    hdr.header_crc32 = crc32_gpt((uint8_t*)&hdr, hdr.header_size);

    rlog("[init] GPT: writing updated partition table...\n");

    // Write primary GPT entries and header
    ssize_t w1 = pwrite(fd, entries.data(), entries_bytes, hdr.partition_entry_lba * 512);
    ssize_t w2 = pwrite(fd, &hdr, sizeof(hdr), 512);
    rlog("[init] GPT: primary write: entries=%zd header=%zd\n", w1, w2);

    if (w1 != (ssize_t)entries_bytes || w2 != (ssize_t)sizeof(hdr)) {
        rlog("[init] GPT: WARNING — primary GPT write failed, skipping resize\n");
        close(fd);
        return;
    }

    // Write backup GPT structures
    uint64_t backup_entries_lba = disk_sectors - 33;
    ssize_t w3 = pwrite(fd, entries.data(), entries_bytes, backup_entries_lba * 512);

    GPTHeader backup = hdr;
    backup.my_lba = disk_sectors - 1;
    backup.alternate_lba = 1;
    backup.partition_entry_lba = backup_entries_lba;
    backup.header_crc32 = 0;
    backup.header_crc32 = crc32_gpt((uint8_t*)&backup, backup.header_size);
    ssize_t w4 = pwrite(fd, &backup, sizeof(backup), (disk_sectors - 1) * 512);
    rlog("[init] GPT: backup write: entries=%zd header=%zd\n", w3, w4);

    if (w3 != (ssize_t)entries_bytes || w4 != (ssize_t)sizeof(backup)) {
        rlog("[init] GPT: WARNING — backup GPT write failed (non-fatal)\n");
    }

    // Update Protective MBR size + CHS end
    uint8_t mbr[512];
    if (pread(fd, mbr, 512, 0) == 512) {
        // PMBR disk size at offset 458 is 32-bit — clamp to max uint32
        uint32_t pmbr_size = (uint32_t)std::min(disk_sectors - 1,
                                                  (uint64_t)0xFFFFFFFF);
        memcpy(&mbr[458], &pmbr_size, 4);
        // CHS end at offsets 451-453 — use max value (all disks > 8GB)
        mbr[446 + 5] = 0xFE;  // CHS end: head 254
        mbr[446 + 6] = 0xFF;  // CHS end: sector 63 + cylinder high bits
        mbr[446 + 7] = 0xFF;  // CHS end: cylinder low bits
        if (pwrite(fd, mbr, 512, 0) != 512) {
            rlog("[init] GPT: WARNING — PMBR write failed (non-fatal)\n");
        }
    } else {
        rlog("[init] GPT: WARNING — PMBR read failed, skipping PMBR update\n");
    }

    int sync_ret = fsync(fd);
    rlog("[init] GPT: fsync=%d\n", sync_ret);

    // Update the kernel's partition table.
    // BLKRRPART may not update already-visible partitions, so we use BLKPG
    // to directly resize the specific partition in the kernel's in-memory table.
    struct blkpg_partition bp;
    memset(&bp, 0, sizeof(bp));
    bp.start  = (long long)target->starting_lba * 512;
    bp.length = (long long)(new_last_usable - target->starting_lba + 1) * 512;
    bp.pno    = part_num;

    struct blkpg_ioctl_arg ba;
    memset(&ba, 0, sizeof(ba));
    ba.op = BLKPG_RESIZE_PARTITION;
    ba.datalen = sizeof(bp);
    ba.data = &bp;

    if (ioctl(fd, BLKPG, &ba) != 0) {
        rlog("[init] GPT: BLKPG_RESIZE_PARTITION failed: %m, trying BLKRRPART\n");
        // Fallback to BLKRRPART
        ioctl(fd, BLKRRPART, 0);
    } else {
        rlog("[init] GPT: kernel partition %d resized via BLKPG\n", part_num);
    }

    close(fd);

    uint64_t grown_mb = (growth * 512) / (1024 * 1024);
    rlog("[init] Grew DATA partition %s by %lu MB (LBA %lu → %lu)\n",
            part_dev, (unsigned long)grown_mb,
            (unsigned long)old_end, (unsigned long)new_last_usable);

    // Small delay for kernel to update device nodes
    usleep(200000);
}

// Grow ext4 filesystem online to fill its partition.
// Uses EXT4_IOC_RESIZE_FS ioctl on the mounted filesystem.
static void grow_ext4_online(const char* part_dev) {
    rlog("[init] ext4 resize: checking %s\n", part_dev);

    struct statfs sfs;
    if (statfs("/data", &sfs) != 0) {
        rlog("[init] ext4: statfs /data failed: %m\n");
        return;
    }

    int partfd = open(part_dev, O_RDONLY);
    if (partfd < 0) {
        rlog("[init] ext4: cannot open %s: %m\n", part_dev);
        return;
    }

    uint64_t part_bytes = 0;
    if (ioctl(partfd, BLKGETSIZE64, &part_bytes) != 0) {
        rlog("[init] ext4: BLKGETSIZE64 on %s failed: %m\n", part_dev);
        close(partfd);
        return;
    }
    close(partfd);

    if (part_bytes == 0 || sfs.f_bsize == 0) {
        rlog("[init] ext4: part_bytes=%lu bsize=%lu\n",
             (unsigned long)part_bytes, (unsigned long)sfs.f_bsize);
        return;
    }

    uint64_t fs_block_size = sfs.f_bsize;
    uint64_t current_blocks = sfs.f_blocks;
    uint64_t max_blocks = part_bytes / fs_block_size;

    rlog("[init] ext4: part=%lu MB, fs=%lu MB (blocks: current=%lu max=%lu bsize=%lu)\n",
         (unsigned long)(part_bytes / (1024*1024)),
         (unsigned long)((current_blocks * fs_block_size) / (1024*1024)),
         (unsigned long)current_blocks, (unsigned long)max_blocks,
         (unsigned long)fs_block_size);

    // Only resize if there's more than 4 MB to gain
    if (max_blocks <= current_blocks + 1024) {
        rlog("[init] ext4: no significant growth possible, skipping\n");
        return;
    }

    int mountfd = open("/data", O_RDONLY);
    if (mountfd < 0) {
        rlog("[init] ext4: cannot open /data: %m\n");
        return;
    }

    if (ioctl(mountfd, EXT4_IOC_RESIZE_FS, &max_blocks) == 0) {
        uint64_t grown_mb = ((max_blocks - current_blocks) * fs_block_size) /
                            (1024 * 1024);
        rlog("[init] Grew /data filesystem by %lu MB (now %lu MB)\n",
                (unsigned long)grown_mb,
                (unsigned long)((max_blocks * fs_block_size) / (1024 * 1024)));
        close(mountfd);
        return;
    }

    int resize_err = errno;
    close(mountfd);
    rlog("[init] ext4 online resize failed: %m (errno=%d)\n", resize_err);

    // Online resize failed (common when ext4 was created on a tiny partition
    // and needs to grow to a much larger size — insufficient reserved GDT blocks).
    // Fallback: unmount, mkfs.ext4, remount with a properly-sized filesystem.
    uint64_t part_mb = part_bytes / (1024 * 1024);
    uint64_t fs_mb = (current_blocks * fs_block_size) / (1024 * 1024);
    if (part_mb > fs_mb * 4) {  // Only if partition is >4x the filesystem
        rlog("[init] ext4: partition %lu MB >> filesystem %lu MB, recreating...\n",
             (unsigned long)part_mb, (unsigned long)fs_mb);

        if (umount("/data") != 0) {
            rlog("[init] ext4: cannot unmount /data for mkfs: %m\n");
            return;
        }

        // Try mkfs.ext4
        pid_t pid = fork();
        if (pid == 0) {
            execl("/usr/sbin/mkfs.ext4", "mkfs.ext4", "-F", "-q",
                  "-L", "LLAMASTE-DATA", part_dev, nullptr);
            execl("/sbin/mkfs.ext4", "mkfs.ext4", "-F", "-q",
                  "-L", "LLAMASTE-DATA", part_dev, nullptr);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                rlog("[init] ext4: mkfs.ext4 succeeded on %s\n", part_dev);
                if (mount(part_dev, "/data", "ext4", 0, nullptr) == 0) {
                    rlog("[init] ext4: remounted /data (%lu MB)\n", (unsigned long)part_mb);
                    // Recreate standard directories
                    const char* dirs[] = {
                        "/data/models", "/data/llamaste", "/data/llamaste/conversations",
                        "/data/llamaste/config", "/data/llamaste/logs",
                        "/data/llamaste/skills", "/data/llamaste/cache", nullptr
                    };
                    for (int i = 0; dirs[i]; i++) mkdir(dirs[i], 0755);
                } else {
                    rlog("[init] ext4: remount after mkfs failed: %m\n");
                }
            } else {
                rlog("[init] ext4: mkfs.ext4 failed (status=%d), remounting old fs\n", status);
                mount(part_dev, "/data", "ext4", 0, nullptr);
            }
        } else {
            rlog("[init] ext4: fork failed: %m\n");
            mount(part_dev, "/data", "ext4", 0, nullptr);
        }
    }
}

#endif // !_WIN32

// ---- Public API ----

static void try_mount(const char* src, const char* tgt,
                      const char* fs, unsigned long flags,
                      const char* data) {
#ifndef _WIN32
    mkdir(tgt, 0755);
    if (mount(src, tgt, fs, flags, data) != 0)
        fprintf(stderr, "[init] mount %s failed: %m\n", tgt);
#endif
}

void init_mount_filesystems() {
    try_mount("proc",     "/proc",    "proc",     0, nullptr);
    try_mount("sysfs",    "/sys",     "sysfs",    0, nullptr);
    try_mount("devtmpfs", "/dev",     "devtmpfs", 0, nullptr);
    try_mount("tmpfs",    "/tmp",     "tmpfs",    0, "size=64M");
    try_mount("tmpfs",    "/run",     "tmpfs",    0, "size=16M");
    // /var/run and /var/db must be writable for dhcpcd, wpa_supplicant, etc.
    // Squashfs is read-only, so mount tmpfs over them.
    try_mount("tmpfs",    "/var/run",  "tmpfs",    0, "size=4M");
    try_mount("tmpfs",    "/var/db",   "tmpfs",    0, "size=4M");
    mkdir("/dev/pts", 0755);
    try_mount("devpts",   "/dev/pts", "devpts",   0, nullptr);
    // /dev/shm: required by wlroots' os_create_anonymous_file() fallback path.
    // When memfd_create() fails for any reason, wlroots falls back to creating
    // a file in /dev/shm. Without this mount, the fallback also fails → wlroots
    // crashes (SIGSEGV) on first keyboard input ("Failed to allocate shm file
    // for XKB keymap"). memfd_create is the primary path but /dev/shm must exist.
    mkdir("/dev/shm", 0755);
    try_mount("tmpfs",    "/dev/shm", "tmpfs",    0, "size=32M");
}

// Enumerate all block device partitions from /sys/block/
static std::vector<std::string> discover_partitions() {
    std::vector<std::string> parts;
#ifndef _WIN32
    DIR* d = opendir("/sys/block");
    if (!d) return parts;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string blk = ent->d_name;
        // Skip loop, ram, dm devices
        if (blk.find("loop") == 0 || blk.find("ram") == 0 ||
            blk.find("dm-") == 0) continue;

        // Enumerate partitions under /sys/block/<dev>/
        std::string blk_dir = "/sys/block/" + blk;
        DIR* bd = opendir(blk_dir.c_str());
        if (!bd) continue;
        struct dirent* pe;
        while ((pe = readdir(bd)) != nullptr) {
            std::string pname = pe->d_name;
            // Partition dirs start with the block device name
            if (pname.find(blk) != 0) continue;
            std::string part_path = blk_dir + "/" + pname + "/partition";
            struct stat st;
            if (stat(part_path.c_str(), &st) == 0) {
                parts.push_back("/dev/" + pname);
            }
        }
        closedir(bd);
    }
    closedir(d);
#endif
    return parts;
}

// Try to mount a partition as ext4 on /data, with GPT grow + ext4 grow.
// Returns true on success.
static bool try_mount_data_partition(const char* dev) {
#ifndef _WIN32
    rlog("[init] mount_data: trying %s\n", dev);

    // Read ext4 superblock magic to verify this is actually ext4
    {
        int fd = open(dev, O_RDONLY);
        if (fd >= 0) {
            uint8_t sb[2048];
            ssize_t n = pread(fd, sb, sizeof(sb), 0);
            close(fd);
            if (n >= 1082) {
                uint16_t magic = (uint16_t)sb[1080] | ((uint16_t)sb[1081] << 8);
                uint32_t features_compat = sb[1116] | (sb[1117]<<8) | (sb[1118]<<16) | (sb[1119]<<24);
                uint32_t features_incompat = sb[1120] | (sb[1121]<<8) | (sb[1122]<<16) | (sb[1123]<<24);
                uint32_t features_ro_compat = sb[1124] | (sb[1125]<<8) | (sb[1126]<<16) | (sb[1127]<<24);
                rlog("[init] mount_data: %s superblock magic=0x%04X (expect 0xEF53)"
                     " compat=0x%08X incompat=0x%08X ro_compat=0x%08X\n",
                     dev, magic, features_compat, features_incompat, features_ro_compat);
                if (magic != 0xEF53) {
                    rlog("[init] mount_data: %s is NOT ext4 (bad magic), skipping\n", dev);
                    return false;
                }
            } else {
                rlog("[init] mount_data: %s read only %zd bytes (need 1082)\n", dev, n);
            }
        } else {
            rlog("[init] mount_data: %s open failed: %m (errno=%d)\n", dev, errno);
        }
    }

    grow_gpt_partition(dev);

    if (mount(dev, "/data", "ext4", 0, nullptr) == 0) {
        rlog("[init] Mounted %s on /data\n", dev);
        grow_ext4_online(dev);
        return true;
    } else {
        int e = errno;
        rlog("[init] mount_data: mount %s failed: %m (errno=%d)\n", dev, e);

        // If EIO, try noload (skip journal replay) as diagnostic
        if (e == EIO) {
            rlog("[init] mount_data: retrying %s with noload (skip journal)...\n", dev);
            if (mount(dev, "/data", "ext4", 0, "noload") == 0) {
                rlog("[init] mount_data: %s mounted with noload! Journal issue.\n", dev);
                // Remount read-write
                if (mount(nullptr, "/data", nullptr, MS_REMOUNT, nullptr) == 0) {
                    rlog("[init] mount_data: remounted /data read-write\n");
                }
                grow_ext4_online(dev);
                return true;
            } else {
                rlog("[init] mount_data: noload mount also failed: %m (errno=%d)\n", errno);
            }
        }

        // Last resort: recreate ext4 filesystem with mkfs.ext4
        // This handles reinstall scenarios where old backup GPT persists
        // but the ext4 superblock was overwritten by dd
        {
            const char* mkfs_paths[] = {
                "/sbin/mkfs.ext4", "/usr/sbin/mkfs.ext4",
                "/bin/mkfs.ext4", "/usr/bin/mkfs.ext4", nullptr
            };
            const char* mkfs = nullptr;
            for (int i = 0; mkfs_paths[i]; i++) {
                if (access(mkfs_paths[i], X_OK) == 0) {
                    mkfs = mkfs_paths[i];
                    break;
                }
            }
            if (mkfs) {
                rlog("[init] mount_data: attempting mkfs.ext4 on %s (recreate filesystem)...\n", dev);
                pid_t pid = fork();
                if (pid == 0) {
                    const char* argv[] = {mkfs, "-q", "-F", "-L", "DATA", dev, nullptr};
                    execv(mkfs, (char* const*)argv);
                    _exit(127);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                        rlog("[init] mount_data: mkfs.ext4 succeeded, retrying mount...\n");
                        if (mount(dev, "/data", "ext4", 0, nullptr) == 0) {
                            rlog("[init] Mounted %s on /data (after mkfs.ext4 recreate)\n", dev);
                            return true;
                        } else {
                            rlog("[init] mount_data: mount after mkfs.ext4 STILL failed: %m\n");
                        }
                    } else {
                        rlog("[init] mount_data: mkfs.ext4 failed (status=%d)\n", status);
                    }
                }
            } else {
                rlog("[init] mount_data: mkfs.ext4 not found, cannot recreate filesystem\n");
            }
        }
    }
#endif
    return false;
}

bool init_mount_data() {
#ifndef _WIN32
    // Static candidate list (common configurations)
    const char* candidates[] = {
        "/dev/vda5", "/dev/sda5", "/dev/sdb5", "/dev/sdc5", "/dev/nvme0n1p5",
        "/dev/vda4", "/dev/sda4", "/dev/sdb4", "/dev/sdc4", "/dev/nvme0n1p4",
        nullptr
    };

    mkdir("/data", 0755);

    // Log all block devices for diagnostics
    rlog("[init] mount_data: scanning for DATA partition...\n");
    auto all_parts = discover_partitions();
    rlog("[init] mount_data: discovered %zu partitions:", all_parts.size());
    for (const auto& p : all_parts) rlog(" %s", p.c_str());
    rlog("\n");

    // Phase 1: try static candidates (fast path)
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0) {
            rlog("[init] mount_data: found %s (mode=0%o)\n", candidates[i], st.st_mode);
            if (try_mount_data_partition(candidates[i]))
                return true;
        }
    }

    // Phase 2: try all discovered partitions (handles unexpected device names)
    rlog("[init] mount_data: static candidates failed, trying all discovered partitions...\n");
    for (const auto& part : all_parts) {
        // Skip partitions already tried in static list
        bool already_tried = false;
        for (int i = 0; candidates[i]; i++) {
            if (part == candidates[i]) { already_tried = true; break; }
        }
        if (already_tried) continue;

        struct stat st;
        if (stat(part.c_str(), &st) == 0) {
            rlog("[init] mount_data: trying discovered %s\n", part.c_str());
            if (try_mount_data_partition(part.c_str()))
                return true;
        }
    }

    rlog("[init] WARNING: No data partition found, using tmpfs /data (1G)\n");
    fprintf(stderr, "[init] WARNING: No data partition, using tmpfs\n");
    try_mount("tmpfs", "/data", "tmpfs", 0, "size=1G");
#endif
    return false;
}

void init_mount_esp() {
#ifndef _WIN32
    // Mount ESP (EFI System Partition) at /boot/efi for grubenv access.
    // ESP holds the GRUB environment block used for A/B slot switching.
    const char* candidates[] = {
        "/dev/sda2", "/dev/vda2", "/dev/nvme0n1p2", nullptr
    };

    mkdir("/boot", 0755);
    mkdir("/boot/efi", 0755);

    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0) {
            if (mount(candidates[i], "/boot/efi", "vfat", MS_NOATIME, "") == 0) {
                fprintf(stderr, "[init] Mounted %s on /boot/efi (ESP)\n", candidates[i]);
                return;
            } else {
                fprintf(stderr, "[init] ESP mount %s failed: %m\n", candidates[i]);
            }
        }
    }

    fprintf(stderr, "[init] WARNING: Could not mount ESP (grubenv unavailable)\n");
#endif
}

void init_create_data_dirs() {
    const char* dirs[] = {
        "/data/models",
        "/data/llamaste",
        "/data/llamaste/conversations",
        "/data/llamaste/config",
        "/data/llamaste/logs",
        "/data/llamaste/skills",
        "/data/tmp",
        nullptr
    };
    for (int i = 0; dirs[i]; i++)
        mkdir(dirs[i], 0755);
}

void init_setup_audio() {
#ifndef _WIN32
    // ALSA cards default to muted with volume 0. Without alsa-utils (amixer),
    // nothing unmutes them. Use the ALSA mixer C API directly.
    // This requires libasound (alsa-lib) which is already linked.
    fprintf(stderr, "[init] Setting up ALSA audio...\n");

    // Check if sound cards exist
    std::ifstream cards("/proc/asound/cards");
    if (!cards.is_open()) {
        fprintf(stderr, "[init] No ALSA sound cards found\n");
        return;
    }
    std::string cards_content((std::istreambuf_iterator<char>(cards)),
                               std::istreambuf_iterator<char>());
    if (cards_content.find('[') == std::string::npos) {
        fprintf(stderr, "[init] No ALSA sound cards detected\n");
        return;
    }
    fprintf(stderr, "[init] ALSA cards:%s\n",
            cards_content.substr(0, 200).c_str());

    // Use amixer-style approach via writing to /sys or using ALSA control files
    // Since we can't easily link the mixer API without header changes,
    // write directly to ALSA control via procfs/sysfs
    // Alternative: fork/exec amixer if available, or write a simple mixer setup

    // Simple approach: try to write volume via /proc/asound/card0/codec#0
    // or use tinymixer-style direct ioctl. For now, fork/exec amixer if it exists.
    const char* amixer_paths[] = {
        "/usr/bin/amixer", "/bin/amixer", "/sbin/amixer", nullptr
    };
    const char* amixer = nullptr;
    for (int i = 0; amixer_paths[i]; i++) {
        if (access(amixer_paths[i], X_OK) == 0) {
            amixer = amixer_paths[i];
            break;
        }
    }

    if (!amixer) {
        // No amixer — try tinyalsa or direct ALSA ioctl approach
        // For now, just log that audio setup needs alsa-utils
        fprintf(stderr, "[init] amixer not found — ALSA mixer controls may be muted\n");
        fprintf(stderr, "[init] Add BR2_PACKAGE_ALSA_UTILS=y to defconfig for audio\n");
        return;
    }

    // Unmute Master and set to 80%
    auto run_amixer = [](const char* amixer_path, const char* const argv[]) {
        pid_t pid = fork();
        if (pid == 0) {
            execv(amixer_path, const_cast<char* const*>(argv));
            _exit(127);
        }
        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    };

    const char* args1[] = { amixer, "sset", "Master", "80%", "unmute", nullptr };
    run_amixer(amixer, args1);
    const char* args2[] = { amixer, "sset", "Speaker", "80%", "unmute", nullptr };
    run_amixer(amixer, args2);
    const char* args3[] = { amixer, "sset", "Headphone", "80%", "unmute", nullptr };
    run_amixer(amixer, args3);
    const char* args4[] = { amixer, "sset", "PCM", "80%", "unmute", nullptr };
    run_amixer(amixer, args4);
    // Unmute capture for microphone
    const char* args5[] = { amixer, "sset", "Capture", "80%", "cap", nullptr };
    run_amixer(amixer, args5);
    const char* args6[] = { amixer, "sset", "Internal Mic", "80%", "cap", nullptr };
    run_amixer(amixer, args6);

    fprintf(stderr, "[init] ALSA mixer: unmuted Master/Speaker/Headphone/PCM/Capture\n");
#endif
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
        if (line.find("llamaste.mode=live") != std::string::npos)
            return "live";
        if (line.find("llamaste.mode=desktop") != std::string::npos)
            return "desktop";
    }
    return "server";
}

void init_bring_up_loopback() {
#ifndef _WIN32
    // Bring up lo (127.0.0.1) — no init system does this for us.
    // Without it, http://localhost is "Network unreachable".
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;

    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);

    // Set address 127.0.0.1
    struct sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_addr);
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(0x7f000001); // 127.0.0.1
    ioctl(sock, SIOCSIFADDR, &ifr);

    // Set netmask 255.0.0.0
    struct sockaddr_in* mask = reinterpret_cast<struct sockaddr_in*>(&ifr.ifr_netmask);
    mask->sin_family = AF_INET;
    mask->sin_addr.s_addr = htonl(0xff000000);
    ioctl(sock, SIOCSIFNETMASK, &ifr);

    // Bring up
    ioctl(sock, SIOCGIFFLAGS, &ifr);
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
    ioctl(sock, SIOCSIFFLAGS, &ifr);

    close(sock);
    fprintf(stderr, "[init] Loopback interface lo brought up (127.0.0.1)\n");
#endif
}

void init_apply_network_config() {
#ifndef _WIN32
    // Read /data/config/network.json — if mode=static, apply settings.
    // If absent or mode=dhcp, write resolv.conf from kernel DHCP info.
    std::ifstream f("/data/config/network.json");
    std::string content;
    std::string mode = "dhcp";

    if (f.is_open()) {
        content.assign((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        f.close();
    }

    // Minimal JSON parsing — look for key fields
    // Format: {"mode":"static","interface":"eth0","ip":"192.168.1.100",
    //          "netmask":"255.255.255.0","gateway":"192.168.1.1","dns":"8.8.8.8"}
    auto get_val = [&](const char* key) -> std::string {
        std::string needle = std::string("\"") + key + "\"";
        auto pos = content.find(needle);
        if (pos == std::string::npos) return "";
        pos = content.find(':', pos);
        if (pos == std::string::npos) return "";
        pos = content.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = content.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return content.substr(pos + 1, end - pos - 1);
    };

    if (!content.empty()) mode = get_val("mode");

    if (mode != "static") {
        // DHCP mode — kernel ip=dhcp already configured the interface.
        // Write /etc/resolv.conf from /proc/net/pnp (kernel DHCP result).
        std::ifstream pnp("/proc/net/pnp");
        if (pnp.is_open()) {
            std::ofstream resolv("/etc/resolv.conf");
            if (resolv.is_open()) {
                std::string line;
                while (std::getline(pnp, line)) {
                    // /proc/net/pnp contains lines like "nameserver 10.0.2.3"
                    if (line.find("nameserver") == 0 || line.find("domain") == 0) {
                        resolv << line << "\n";
                    }
                }
                fprintf(stderr, "[init] Wrote /etc/resolv.conf from kernel DHCP\n");
            }
        } else {
            // Fallback: use 8.8.8.8 if no /proc/net/pnp
            std::ofstream resolv("/etc/resolv.conf");
            if (resolv.is_open()) {
                resolv << "nameserver 8.8.8.8\n";
                fprintf(stderr, "[init] Wrote /etc/resolv.conf (fallback: 8.8.8.8)\n");
            }
        }

        // Apply default gateway from kernel DHCP.
        // Kernel ip=dhcp sets IP/netmask but may not add the default route.
        // Check /proc/net/route first — if no default route, read gateway
        // from /proc/net/ipconfig (kernel DHCP result).
        {
            bool has_default = false;
            FILE* rt = fopen("/proc/net/route", "r");
            if (rt) {
                char line[256], riface[32];
                unsigned long dest;
                while (fgets(line, sizeof(line), rt)) {
                    if (sscanf(line, "%31s %lx", riface, &dest) == 2 && dest == 0
                        && strcmp(riface, "lo") != 0) {
                        has_default = true;
                        break;
                    }
                }
                fclose(rt);
            }

            if (!has_default) {
                // Read gateway from /proc/net/ipconfig (kernel writes this after ip=dhcp)
                std::string gw;
                std::ifstream ipc("/proc/net/ipconfig");
                if (ipc.is_open()) {
                    std::string iline;
                    while (std::getline(ipc, iline)) {
                        // Format: "Gateway  : 192.168.137.180"
                        if (iline.find("Gateway") != std::string::npos) {
                            auto colon = iline.rfind(':');
                            if (colon != std::string::npos) {
                                gw = iline.substr(colon + 1);
                                gw.erase(0, gw.find_first_not_of(" \t"));
                                gw.erase(gw.find_last_not_of(" \t\r\n") + 1);
                                if (gw == "0.0.0.0" || gw.empty()) gw.clear();
                            }
                        }
                    }
                }

                if (!gw.empty()) {
                    // Find the interface that has an IP (not lo)
                    FILE* rrf = fopen("/proc/net/route", "r");
                    std::string iface;
                    if (rrf) {
                        char line[256], riface[32]; unsigned long dest;
                        while (fgets(line, sizeof(line), rrf)) {
                            if (sscanf(line, "%31s %lx", riface, &dest) == 2
                                && dest != 0 && strcmp(riface, "lo") != 0) {
                                iface = riface; break;
                            }
                        }
                        fclose(rrf);
                    }
                    if (iface.empty()) iface = "eno1"; // fallback

                    // Dump routes BEFORE adding gateway (diagnostic)
                    netlink_dump_routes("pre-gw");

                    // Use netlink RTM_NEWROUTE — the modern API that ip(8) uses internally.
                    // Previous approaches (SIOCADDRT ioctl, fork/exec ip route) both
                    // silently failed on real hardware.
                    netlink_del_default_route();
                    int ret = netlink_add_default_route(gw.c_str(), iface.c_str());
                    if (ret != 0) {
                        fprintf(stderr, "[init] NETLINK gateway failed, trying SIOCADDRT fallback\n");
                        struct in_addr gw_addr = {};
                        if (inet_pton(AF_INET, gw.c_str(), &gw_addr) == 1) {
                            int sk = socket(AF_INET, SOCK_DGRAM, 0);
                            if (sk >= 0) {
                                struct rtentry art = {};
                                ((struct sockaddr_in*)&art.rt_gateway)->sin_family = AF_INET;
                                ((struct sockaddr_in*)&art.rt_gateway)->sin_addr = gw_addr;
                                ((struct sockaddr_in*)&art.rt_dst)->sin_family = AF_INET;
                                ((struct sockaddr_in*)&art.rt_dst)->sin_addr.s_addr = 0;
                                ((struct sockaddr_in*)&art.rt_genmask)->sin_family = AF_INET;
                                ((struct sockaddr_in*)&art.rt_genmask)->sin_addr.s_addr = 0;
                                art.rt_flags = RTF_UP | RTF_GATEWAY;
                                char devbuf[IFNAMSIZ] = {};
                                strncpy(devbuf, iface.c_str(), IFNAMSIZ - 1);
                                art.rt_dev = devbuf;
                                int r2 = ioctl(sk, SIOCADDRT, &art);
                                fprintf(stderr, "[init] SIOCADDRT fallback: %s\n",
                                        r2 == 0 ? "OK" : strerror(errno));
                                close(sk);
                            }
                        }
                    }

                    // Dump routes AFTER adding gateway (diagnostic)
                    netlink_dump_routes("post-gw");
                } else {
                    fprintf(stderr, "[init] WARNING: No gateway found in /proc/net/ipconfig\n");
                }
            }
        }
        return;
    }

    std::string iface = get_val("interface");
    std::string ip = get_val("ip");
    std::string netmask = get_val("netmask");
    std::string gateway = get_val("gateway");
    std::string dns = get_val("dns");

    if (iface.empty()) iface = "eth0";
    if (ip.empty()) {
        fprintf(stderr, "[init] Static IP config missing 'ip', skipping\n");
        return;
    }
    if (netmask.empty()) netmask = "255.255.255.0";

    fprintf(stderr, "[init] Applying static IP: %s/%s on %s gw=%s dns=%s\n",
            ip.c_str(), netmask.c_str(), iface.c_str(),
            gateway.empty() ? "none" : gateway.c_str(),
            dns.empty() ? "none" : dns.c_str());

    // Apply IP address via ioctl
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "[init] Cannot create socket for network config: %m\n");
        return;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);

    // Set IP address
    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &addr->sin_addr);
    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0)
        fprintf(stderr, "[init] SIOCSIFADDR failed: %m\n");

    // Set netmask
    addr = (struct sockaddr_in*)&ifr.ifr_netmask;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, netmask.c_str(), &addr->sin_addr);
    if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0)
        fprintf(stderr, "[init] SIOCSIFNETMASK failed: %m\n");

    // Bring interface up
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        ioctl(sock, SIOCSIFFLAGS, &ifr);
    }

    // Set default gateway via netlink
    if (!gateway.empty()) {
        netlink_dump_routes("pre-static-gw");
        netlink_del_default_route();
        int ret = netlink_add_default_route(gateway.c_str(), iface.c_str());
        if (ret != 0) {
            fprintf(stderr, "[init] netlink gateway failed for static config\n");
        }
        netlink_dump_routes("post-static-gw");
    }

    close(sock);

    // Set DNS resolver
    if (!dns.empty()) {
        std::ofstream resolv("/etc/resolv.conf");
        if (resolv.is_open()) {
            resolv << "nameserver " << dns << "\n";
        }
    }

    fprintf(stderr, "[init] Static network config applied\n");
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// do_live_pivot — mount squashfs overlay and re-exec into the full system
//
// When booting from the live ISO, /install/rootfs.squashfs is the complete
// installed rootfs.  This function:
//   1. Creates /live/{tmpfs,squashfs,root}
//   2. Attaches rootfs.squashfs to a free loop device and mounts it (ro)
//   3. Mounts overlayfs at /live/root  (squashfs lower + tmpfs upper)
//   4. Binds /proc /sys /dev /run into the new root
//   5. Binds /install so the installer finds images at the expected path
//   6. Pivots via the classic switch_root pattern:
//        chdir /live/root → MS_MOVE "." to "/" → chroot "." → execv
//
// Guard (double):
//   • /llamaste-live-iso  is a marker file created by build-iso.sh ONLY in the
//     ISO root directory — it is NEVER placed inside rootfs.squashfs.
//     After the pivot + re-exec, the new root is the squashfs overlay which has
//     no /llamaste-live-iso → guard fails → do_live_pivot() returns false immediately.
//     *** DO NOT use /boot/bzImage as this guard ***
//     Buildroot installs the kernel to output/target/boot/, which goes into
//     rootfs.squashfs — so /boot/bzImage exists in BOTH the ISO root AND the
//     squashfs, causing an infinite pivot loop on re-exec.
//   • /install/rootfs.squashfs must also be present
//   If either check fails, returns false and boot continues normally.
// ─────────────────────────────────────────────────────────────────────────────
bool do_live_pivot(char** argv) {
#ifdef _WIN32
    (void)argv;
    return false;
#else
    // Guard 1: marker file only exists in the ISO root, never in rootfs.squashfs.
    // build-iso.sh creates this with: touch "${ISO_ROOT}/llamaste-live-iso"
    if (access("/llamaste-live-iso", F_OK) != 0)
        return false;
    // Guard 2: squashfs must actually be present
    if (access("/install/rootfs.squashfs", R_OK) != 0)
        return false;

    fprintf(stderr, "[init] Live pivot: pivoting to full squashfs system\n");

    // --- Working dirs (on the current ISO / tmpfs root) ---
    mkdir("/live",           0755);
    mkdir("/live/squashfs", 0755);
    mkdir("/live/tmpfs",    0755);
    mkdir("/live/root",     0755);

    // tmpfs provides the overlay upper + work layers (persists for the session)
    if (mount("tmpfs", "/live/tmpfs", "tmpfs", 0, "size=512M") < 0) {
        fprintf(stderr, "[init] Live pivot: tmpfs: %m\n");
        return false;
    }
    mkdir("/live/tmpfs/upper", 0755);
    mkdir("/live/tmpfs/work",  0755);

    // --- Attach rootfs.squashfs to a free loop device ---
    int ctrl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
    if (ctrl < 0) {
        fprintf(stderr, "[init] Live pivot: open loop-control: %m\n");
        return false;
    }
    int loop_num = ioctl(ctrl, LOOP_CTL_GET_FREE);
    close(ctrl);
    if (loop_num < 0) {
        fprintf(stderr, "[init] Live pivot: LOOP_CTL_GET_FREE: %m\n");
        return false;
    }

    char loop_dev[64];
    snprintf(loop_dev, sizeof(loop_dev), "/dev/loop%d", loop_num);

    int sq_fd = open("/install/rootfs.squashfs", O_RDONLY | O_CLOEXEC);
    if (sq_fd < 0) {
        fprintf(stderr, "[init] Live pivot: open squashfs: %m\n");
        return false;
    }
    int lp_fd = open(loop_dev, O_RDWR | O_CLOEXEC);
    if (lp_fd < 0) {
        fprintf(stderr, "[init] Live pivot: open %s: %m\n", loop_dev);
        close(sq_fd);
        return false;
    }
    if (ioctl(lp_fd, LOOP_SET_FD, sq_fd) < 0) {
        fprintf(stderr, "[init] Live pivot: LOOP_SET_FD: %m\n");
        close(lp_fd); close(sq_fd);
        return false;
    }
    close(lp_fd);
    close(sq_fd);
    fprintf(stderr, "[init] Live pivot: squashfs attached to %s\n", loop_dev);

    // --- Mount squashfs read-only ---
    if (mount(loop_dev, "/live/squashfs", "squashfs", MS_RDONLY, nullptr) < 0) {
        fprintf(stderr, "[init] Live pivot: mount squashfs: %m\n");
        return false;
    }

    // --- Mount overlayfs at /live/root ---
    char ovl[256];
    snprintf(ovl, sizeof(ovl),
             "lowerdir=/live/squashfs,upperdir=/live/tmpfs/upper,workdir=/live/tmpfs/work");
    if (mount("overlay", "/live/root", "overlay", 0, ovl) < 0) {
        fprintf(stderr, "[init] Live pivot: mount overlay: %m\n");
        return false;
    }
    fprintf(stderr, "[init] Live pivot: overlay mounted at /live/root\n");

    // --- Bind essential pseudo-filesystems into new root ---
    // (mkdir is a no-op if the dir already exists in the squashfs lower layer)
    const char* pseudo[] = { "proc", "sys", "dev", "run", nullptr };
    for (int i = 0; pseudo[i]; i++) {
        char src[64], dst[128];
        snprintf(src, sizeof(src), "/%s",           pseudo[i]);
        snprintf(dst, sizeof(dst), "/live/root/%s", pseudo[i]);
        mkdir(dst, 0755);
        if (mount(src, dst, nullptr, MS_BIND, nullptr) < 0)
            fprintf(stderr, "[init] Live pivot: bind %s: %m\n", src);
    }

    // Bind /install so the installer finds disk images at /install/llamaste.img*
    mkdir("/live/root/install", 0755);
    if (mount("/install", "/live/root/install", nullptr, MS_BIND, nullptr) < 0)
        fprintf(stderr, "[init] Live pivot: bind /install: %m\n");

    // Bind the full ISO root as /cdrom (for reference / future use)
    mkdir("/live/root/cdrom", 0755);
    if (mount("/", "/live/root/cdrom", nullptr, MS_BIND, nullptr) < 0)
        fprintf(stderr, "[init] Live pivot: bind /cdrom: %m\n");

    // --- switch_root pivot ---
    // Make all mounts private so MS_MOVE succeeds (kernel rejects on shared mounts)
    mount(nullptr, "/", nullptr, MS_PRIVATE | MS_REC, nullptr);

    if (chdir("/live/root") < 0) {
        fprintf(stderr, "[init] Live pivot: chdir /live/root: %m\n");
        return false;
    }
    // Move the overlay mount from /live/root to / atomically
    if (mount(".", "/", nullptr, MS_MOVE, nullptr) < 0) {
        fprintf(stderr, "[init] Live pivot: MS_MOVE: %m\n");
        return false;
    }
    chroot(".");
    chdir("/");

    fprintf(stderr, "[init] Live pivot: complete — re-executing from squashfs\n");
    execv("/opt/llamaste/llamaste", argv);
    fprintf(stderr, "[init] Live pivot: execv: %m\n");
    return false;   // execv only returns on error
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// init_load_modules — load network kernel modules after squashfs pivot
//
// With CONFIG_MODULES=y, WiFi vendor drivers and USB ethernet drivers are built
// as .ko modules instead of being compiled into bzImage. This means they load
// AFTER the squashfs pivot, when /lib/firmware/ is available — so firmware
// loading just works for any supported chip without CONFIG_EXTRA_FIRMWARE.
//
// We use the finit_module() syscall directly (no modprobe/kmod needed).
// Modules are loaded in dependency order by scanning /lib/modules/<ver>/kernel/
// GPU modules + eudev auto-detection for all other hardware.
//
// Strategy:
//   1. Load critical GPU modules explicitly (need to be ready before compositor)
//   2. Start udevd (auto-loads all other modules via modalias matching)
//   3. Trigger udev coldplug for all subsystems EXCEPT input
//      (input trigger is delayed until Wayland socket exists — see child_main.cpp)
//   4. Wait for device probing to settle
//
// This replaces the old hardcoded 80-module list. eudev + kmod handles
// dependency resolution and hardware detection automatically.
// ─────────────────────────────────────────────────────────────────────────────

// Helper: load a single module via finit_module() syscall
static int load_module_by_path(const std::string& mod_base, const char* rel_path) {
    std::string full_path = mod_base + "/kernel/" + rel_path;
    if (access(full_path.c_str(), R_OK) != 0) return -1; // not built

    int fd = open(full_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;

    int ret = syscall(SYS_finit_module, fd, "", 0);
    int saved_errno = errno;
    close(fd);

    if (ret == 0) {
        fprintf(stderr, "[init] GPU module: loaded %s\n", rel_path);
        return 0;
    } else if (saved_errno == EEXIST) {
        return 0; // already loaded
    } else {
        fprintf(stderr, "[init] GPU module: %s: %s\n", rel_path, strerror(saved_errno));
        return -1;
    }
}

void init_load_modules() {
#ifdef _WIN32
    return;
#else
    struct utsname uts;
    if (uname(&uts) < 0) {
        fprintf(stderr, "[init] modules: uname failed: %m\n");
        return;
    }
    std::string mod_base = "/lib/modules/" + std::string(uts.release);

    struct stat st;
    if (stat(mod_base.c_str(), &st) != 0) {
        fprintf(stderr, "[init] modules: %s not found — built-in kernel, skipping\n",
                mod_base.c_str());
        return;
    }

    // --- Phase 1: Load critical GPU modules explicitly ---
    // GPU must be ready before the desktop compositor starts.
    // These are loaded via finit_module() BEFORE eudev trigger because
    // udev coldplug would race with compositor startup.
    fprintf(stderr, "[init] Loading critical GPU modules...\n");

    // DRM dependencies (shared by amdgpu + nouveau)
    load_module_by_path(mod_base, "drivers/gpu/drm/scheduler/gpu-sched.ko");
    load_module_by_path(mod_base, "drivers/gpu/drm/drm_exec.ko");
    load_module_by_path(mod_base, "drivers/gpu/drm/drm_suballoc_helper.ko");
    // TTM/buddy may be built-in or separate modules depending on kernel version
    load_module_by_path(mod_base, "drivers/gpu/drm/ttm/ttm.ko");
    load_module_by_path(mod_base, "drivers/gpu/drm/drm_buddy.ko");
    load_module_by_path(mod_base, "drivers/gpu/drm/drm_ttm_helper.ko");

    // AMD GPU (=m, must load after squashfs pivot for firmware)
    load_module_by_path(mod_base, "drivers/gpu/drm/amd/amdxcp/amdxcp.ko");
    load_module_by_path(mod_base, "drivers/gpu/drm/amd/amdgpu/amdgpu.ko");

    // NVIDIA nouveau (=m)
    load_module_by_path(mod_base, "drivers/gpu/drm/nouveau/nouveau.ko");

    // Intel Xe GPU (=m) — Arc, Battlemage, Lunar Lake
    load_module_by_path(mod_base, "drivers/gpu/drm/xe/xe.ko");

    // --- Phase 2: Run depmod if modules.dep is missing ---
    // eudev's kmod integration needs modules.dep + modules.alias for auto-loading.
    // Buildroot should generate these, but safety check.
    std::string moddep = mod_base + "/modules.dep";
    if (access(moddep.c_str(), R_OK) != 0) {
        fprintf(stderr, "[init] modules.dep missing — running depmod\n");
        pid_t pid = fork();
        if (pid == 0) {
            execl("/usr/bin/depmod", "depmod", "-a", uts.release, nullptr);
            _exit(127);
        } else if (pid > 0) {
            int wstatus;
            waitpid(pid, &wstatus, 0);
            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
                fprintf(stderr, "[init] depmod completed\n");
            } else {
                fprintf(stderr, "[init] depmod failed (status %d)\n", wstatus);
            }
        }
    }

    // --- Phase 3: Start udevd ---
    init_start_udevd();

    // --- Phase 4: Trigger udev coldplug (everything EXCEPT input) ---
    // Input devices are triggered later in child_main.cpp after the Wayland
    // socket exists. Triggering input before wlroots initializes causes SIGSEGV.
    fprintf(stderr, "[init] Triggering udev coldplug (excluding input)...\n");
    {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/usr/bin/udevadm", "udevadm", "trigger",
                  "--action=add", "--subsystem-nomatch=input", nullptr);
            _exit(127);
        } else if (pid > 0) {
            int wstatus;
            waitpid(pid, &wstatus, 0);
        }
    }

    // --- Phase 5: Wait for device probing to settle ---
    fprintf(stderr, "[init] Waiting for udev settle (30s timeout)...\n");
    {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/usr/bin/udevadm", "udevadm", "settle",
                  "--timeout=30", nullptr);
            _exit(127);
        } else if (pid > 0) {
            int wstatus;
            waitpid(pid, &wstatus, 0);
            if (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0) {
                fprintf(stderr, "[init] udev settle complete\n");
            } else {
                fprintf(stderr, "[init] udev settle timed out (non-fatal)\n");
            }
        }
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Start udevd daemon for hardware auto-detection.
// Runs in foreground with setsid() for signal isolation (not --daemon,
// which double-forks and loses PID tracking — same issue as wpa_supplicant).
// ─────────────────────────────────────────────────────────────────────────────
pid_t g_udevd_pid = 0;

void init_start_udevd() {
#ifdef _WIN32
    return;
#else
    // Check if udevd is already running (e.g. started by child_main for desktop mode)
    if (g_udevd_pid > 0 && kill(g_udevd_pid, 0) == 0) {
        fprintf(stderr, "[init] udevd already running (PID %d)\n", g_udevd_pid);
        return;
    }

    fprintf(stderr, "[init] Starting udevd...\n");
    pid_t pid = fork();
    if (pid == 0) {
        setsid(); // New session for signal isolation
        // Redirect stderr to log file to avoid console flooding
        int log_fd = open("/tmp/udevd.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDERR_FILENO);
            close(log_fd);
        }
        execl("/sbin/udevd", "udevd", nullptr);
        // Fallback path
        execl("/usr/sbin/udevd", "udevd", nullptr);
        _exit(127);
    } else if (pid > 0) {
        g_udevd_pid = pid;
        fprintf(stderr, "[init] udevd started (PID %d)\n", pid);
        // Brief wait for udevd to initialize its netlink socket
        usleep(500000);
    } else {
        fprintf(stderr, "[init] udevd fork failed: %m\n");
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Start dbus-daemon (system bus) — required by BlueZ for Bluetooth HID.
// ─────────────────────────────────────────────────────────────────────────────
pid_t g_dbus_pid = 0;

void init_start_dbus() {
#ifdef _WIN32
    return;
#else
    // Ensure /run/dbus exists
    mkdir("/run/dbus", 0755);

    fprintf(stderr, "[init] Starting dbus-daemon...\n");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/usr/bin/dbus-daemon", "dbus-daemon",
              "--system", "--nofork", "--nopidfile", nullptr);
        _exit(127);
    } else if (pid > 0) {
        g_dbus_pid = pid;
        fprintf(stderr, "[init] dbus-daemon started (PID %d)\n", pid);
        // Wait for socket to appear
        for (int i = 0; i < 20; i++) {
            if (access("/run/dbus/system_bus_socket", F_OK) == 0) break;
            usleep(100000); // 100ms
        }
        if (access("/run/dbus/system_bus_socket", F_OK) == 0) {
            fprintf(stderr, "[init] dbus socket ready\n");
        } else {
            fprintf(stderr, "[init] WARNING: dbus socket not found after 2s\n");
        }
    } else {
        fprintf(stderr, "[init] dbus-daemon fork failed: %m\n");
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Start BlueZ bluetoothd daemon for Bluetooth HID (keyboards, mice).
// Requires dbus to be running first.
// Pairing info stored at /data/bluetooth/ (symlinked from /var/lib/bluetooth).
// ─────────────────────────────────────────────────────────────────────────────
pid_t g_bluetoothd_pid = 0;

void init_start_bluetoothd() {
#ifdef _WIN32
    return;
#else
    // Check dbus is available
    if (access("/run/dbus/system_bus_socket", F_OK) != 0) {
        fprintf(stderr, "[init] bluetoothd: dbus not available, skipping\n");
        return;
    }

    // Symlink pairing data to persistent storage
    mkdir("/data/bluetooth", 0700);
    mkdir("/var/lib", 0755);
    // Remove existing symlink/dir if any, then create symlink
    remove("/var/lib/bluetooth");
    symlink("/data/bluetooth", "/var/lib/bluetooth");

    fprintf(stderr, "[init] Starting bluetoothd...\n");
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/usr/libexec/bluetooth/bluetoothd", "bluetoothd",
              "--nodetach", nullptr);
        // Fallback path
        execl("/usr/lib/bluetooth/bluetoothd", "bluetoothd",
              "--nodetach", nullptr);
        _exit(127);
    } else if (pid > 0) {
        g_bluetoothd_pid = pid;
        fprintf(stderr, "[init] bluetoothd started (PID %d)\n", pid);
    } else {
        fprintf(stderr, "[init] bluetoothd fork failed: %m\n");
    }
#endif
}
