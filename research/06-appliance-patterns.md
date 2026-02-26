# Appliance OS Design Patterns

## Alpine Linux Lessons

### Why It's Small (~5 MB Docker base)
- **musl libc** (~600 KB vs glibc ~8-10 MB, MIT license)
- **BusyBox** (~1 MB for ~300+ Unix utilities)
- **apk** package manager (simple gzipped tarballs)
- **OpenRC** init system (~1 MB vs systemd 40+ MB)

### Diskless Mode (Relevant to Llamaste)
- System runs entirely from RAM
- lbu (Alpine Local Backup) saves config changes to tarball
- Boots from read-only media, loads into tmpfs, applies overlay

## OpenWrt Lessons (Most Applicable Reference)

### Filesystem Architecture
- Read-only SquashFS base partition
- Writable JFFS2/ext4 overlay via OverlayFS
- All modifications go to overlay
- **Factory reset = wipe overlay partition**

### UCI Configuration (Single Source of Truth)
```
config interface 'lan'
    option proto 'static'
    option ipaddr '192.168.1.1'
```
- CLI tool (uci) reads/writes same files as web UI
- Changes staged in memory, committed atomically

### LuCI Web Interface
- JSON-RPC API backend (rpcd daemon)
- Pure JavaScript frontend calling rpcd
- ~100 KB HTTP server (uhttpd)

### Image Builder
- Produces custom firmware with exactly selected packages
- Pattern: build system -> purpose-built images

## TinyCore Linux Lessons

### Extreme Minimalism
- Core variant: ~11 MB CLI
- Entire OS lives in compressed cpio initramfs loaded into RAM
- Extensions are loop-mounted SquashFS images

### Key Insight
A complete Linux system genuinely boots from 11 MB initramfs in seconds. RAM-based operation gives excellent performance.

## Raspberry Pi OS Lessons

### First-Boot Configuration Pattern
- FAT32 boot partition accessible from any OS
- Pre-boot config files: userconf.txt, ssh, wpa_supplicant.conf
- firstrun.sh executes once then deletes itself
- Auto-expands root partition to fill storage

**Directly applicable to Llamaste:** User writes image, optionally places config on FAT32 partition, boots, system auto-configures.

## CoreOS/Flatcar Lessons

### Immutable OS Pattern
- Root filesystem is read-only
- A/B partition scheme for atomic updates
- If new partition fails to boot, automatic rollback
- Ignition: JSON config applied once at first boot from initramfs

### Filesystem Layout
- /usr/ = immutable OS (read-only)
- /etc/ = writable overlay (reset on OS update)
- /var/ = persistent data

## Pi-hole Lessons

### Appliance Feel
- Single purpose, single web dashboard
- Works immediately with sane defaults
- Accessible via browser from any device on LAN
- FTL daemon: single C binary does core work + embedded API

**For Llamaste:** llama-server fills same architectural role as FTL. Web UI should be a thin layer on top of the API.

### Teleporter Pattern
Export all settings as single file, import on new installation.

## Common Patterns Summary

### Read-Only Root Filesystem
- Survives power loss without corruption
- Prevents accidental/malicious modification
- Enables reliable factory reset
- Reduces flash storage wear

### Typical Appliance Partition Layout
```
/dev/sda1 (FAT32, 256MB)   -> /boot     (bootloader, kernel, pre-boot config)
/dev/sda2 (SquashFS)        -> /lower    (read-only root)
/dev/sda3 (ext4)            -> /overlay  (writable changes)
/dev/sda4 (ext4, remaining) -> /data     (user data, models, logs)
```

### Hardware Watchdog
```bash
# Most x86/ARM SoCs have hardware watchdog
# Userspace must "pet" /dev/watchdog regularly
# If daemon stops -> hardware forces reboot
# Default timeout: ~60 seconds
```

### Update Mechanism: A/B Partitions
- Two root partitions (A and B)
- Update writes to inactive partition
- Bootloader switches on reboot
- Automatic rollback if boot fails

Tools: **RAUC**, **SWUpdate**, **Mender**

## musl vs glibc for Llamaste

### CPU-Only (Use musl)
- Fully static binaries, zero dependencies
- MIT license (clean static linking)
- ~600 KB vs ~8-10 MB
- Total system ~25 MB

### GPU-Accelerated (Use glibc)
- CUDA requires glibc (NVIDIA doesn't provide static CUDA libs)
- CUDA libs add 200+ MB anyway
- No practical benefit to musl when CUDA is present

### Recommendation
- **Llamaste Lite (CPU-only):** musl + static llama.cpp = ~25 MB system
- **Llamaste Full (GPU):** glibc + dynamic linking = ~300 MB system

## BusyBox as Complete Userspace

### What It Provides (~1-2 MB)
- Shell (ash, POSIX sh compatible)
- Core utils (ls, cp, mv, rm, mkdir, cat, etc.)
- File utils (find, grep, sed, mini-awk, tar, gzip)
- System utils (mount, fdisk, mkfs, ps, top, sysctl, mdev)
- Network utils (ifconfig, ip subset, ping, wget, nc, httpd, udhcpc)
- Init and service management
- Editor (minimal vi)

### What It Doesn't Provide
- No bash (scripts must be POSIX sh)
- No systemd
- No full ip command
- No package manager
- No compiler/build tools

### mdev vs udev
- mdev: BusyBox built-in, minimal, sufficient for appliance use
- eudev: Needed if NVIDIA driver expects udev-style device nodes

## Web UI for Headless Appliances

### Lightweight Web Servers
| Server | Size | Notes |
|--------|------|-------|
| BusyBox httpd | 0 extra | Basic HTTP + CGI, free with BusyBox |
| uhttpd | ~100 KB | OpenWrt's server, CGI + Lua + TLS |
| lighttpd | ~1 MB | Pi-hole's server, FastCGI support |
| **llama.cpp built-in** | **0 extra** | **Already has HTTP server + web UI** |

### Frontend Options
| Framework | Size | Build Step |
|-----------|------|------------|
| Vanilla JS | Tiny | None |
| Preact | ~3 KB | Yes |
| Alpine.js | ~15 KB | None |
| htmx | ~14 KB | None |

### Recommended Architecture for Llamaste
- llama.cpp's built-in HTTP server handles inference API + web UI
- Separate tiny management API (CGI or small daemon) for system status, config, model management
- Or: single HTTP server handles everything
