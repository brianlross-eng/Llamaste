# Installing Llamaste

Llamaste is a bootable Linux operating system where a single binary IS the entire OS above the kernel. The LLM handles everything: shell, file management, system config, networking, and help -- all through natural conversation.

## System Requirements

| Component | Minimum | Recommended |
|-----------|---------|-------------|
| CPU | x86-64 (any) | x86-64 with AVX2 |
| RAM | 4 GB | 8-16 GB |
| Disk | 8 GB | 32+ GB |
| Network | Ethernet (wired) | Ethernet (wired) |

RAM determines which AI model runs:

| RAM | Model | Quality |
|-----|-------|---------|
| 4 GB | Qwen2.5-0.5B | Basic |
| 8 GB | Qwen2.5-7B | Good |
| 16 GB | Qwen2.5-14B | Great |
| 32 GB | Qwen2.5-32B | Best |

## Quick Start: USB Install

1. **Download** `llamaste.iso` from the releases page
2. **Write to USB** using one of:
   - [Etcher](https://etcher.balena.io/) (Windows/Mac/Linux, graphical)
   - [Rufus](https://rufus.ie/) (Windows)
   - `dd` (Linux/Mac):
     ```bash
     sudo dd if=llamaste.iso of=/dev/sdX bs=4M status=progress
     sync
     ```
     Replace `/dev/sdX` with your USB drive (check with `lsblk`)
3. **Boot from USB** -- enter BIOS/UEFI setup (usually F2, F12, or Del at startup) and select the USB drive
4. **Install to disk** -- select "Llamaste Live + Install" from the GRUB menu
5. **Open the web UI** -- from another device on the same network, open `http://<IP>:80` (the IP is shown on the serial/VGA console)
6. **Click "Install to Disk"** in the live mode banner
7. **Select your target disk** and confirm -- the installer will write the OS and resize the data partition
8. **Reboot** -- remove the USB drive and restart

## Quick Start: Raw Image (dd)

For headless servers or automated deployment:

1. **Download** `llamaste-x86_64.img.xz`
2. **Decompress and write**:
   ```bash
   xzcat llamaste-x86_64.img.xz | sudo dd of=/dev/sdX bs=4M status=progress
   sync
   ```
3. **Boot** the target machine
4. **Access** via `http://<IP>:80` from another device

The raw image includes a fixed-size DATA partition. On first boot, you may want to resize it to use the full disk (this is handled automatically when using the ISO installer).

## QEMU Testing

Test without real hardware using QEMU:

### Test the raw image
```bash
qemu-system-x86_64 \
  -m 512M \
  -nographic \
  -drive file=llamaste.img,format=raw,if=virtio \
  -kernel output/images/bzImage \
  -append 'console=ttyS0 root=/dev/vda3 rootfstype=squashfs ro init=/opt/llamaste/llamaste ip=dhcp' \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80
```

Then open `http://localhost:8080` in your browser.

### Test the ISO
```bash
qemu-system-x86_64 \
  -m 512M \
  -nographic \
  -cdrom llamaste.iso \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80
```

### Test the installer
```bash
# Create an empty target disk
qemu-img create -f raw target.img 4G

# Boot ISO with the target disk attached
qemu-system-x86_64 \
  -m 512M \
  -nographic \
  -cdrom llamaste.iso \
  -drive file=target.img,format=raw,if=virtio \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80

# After installing, boot from the target disk
qemu-system-x86_64 \
  -m 512M \
  -nographic \
  -drive file=target.img,format=raw,if=virtio \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80
```

## Building from Source

### Prerequisites

A Linux build environment (native Linux or WSL2 on Windows):

```bash
# Ubuntu/Debian
sudo apt update
sudo apt install -y build-essential gcc g++ make cmake \
  git wget cpio unzip rsync bc file \
  python3 libncurses-dev libssl-dev \
  qemu-system-x86 \
  grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools \
  dosfstools e2fsprogs genimage
```

### Build steps

```bash
# 1. Clone the repository
git clone https://github.com/nicholasgasior/llamaste.git
cd llamaste

# 2. Clone Buildroot (one-time setup)
git clone https://github.com/buildroot/buildroot.git /root/llamaste-build
cd /root/llamaste-build

# 3. Configure with Llamaste defconfig
make BR2_EXTERNAL=/path/to/llamaste/br2-external llamaste_x86_64_defconfig

# 4. Build (first build takes 20-40 minutes, rebuilds take 1-3 minutes)
make -j$(nproc)

# 5. Build the ISO (optional)
/path/to/llamaste/scripts/build-iso.sh /root/llamaste-build
```

Output files will be in `/root/llamaste-build/output/images/`:
- `llamaste.img` -- raw disk image (359 MB)
- `llamaste.iso` -- bootable ISO (if built)
- `bzImage` -- Linux kernel (5 MB)
- `rootfs.squashfs` -- root filesystem (6 MB)

## First Boot

When Llamaste boots for the first time:

1. The kernel boots in ~2 seconds
2. The llamaste binary mounts filesystems and detects hardware
3. The HTTP server starts on port 80
4. A model is auto-selected based on available RAM (if models are present in `/data/models/`)

Without a model, Llamaste runs in **stub mode** -- the web UI and all system tools work, but the AI responds with placeholder text. To add a model:

1. Download a GGUF model file (e.g., `qwen2.5-7b-instruct-q4_k_m.gguf`)
2. Place it in `/data/models/` on the Llamaste machine
3. Reboot -- the model is auto-detected and loaded

## Configuration

Configuration lives in `/data/llamaste/config/llamaste.json`:

```json
{
    "version": 1,
    "model": "auto",
    "listen": "0.0.0.0",
    "port": 80,
    "threads": 0,
    "context_size": 2048,
    "log_level": "info"
}
```

| Key | Description | Default |
|-----|-------------|---------|
| `model` | Model selection: `"auto"` or path to .gguf file | `"auto"` |
| `listen` | IP address to bind HTTP server | `"0.0.0.0"` |
| `port` | HTTP server port | `80` |
| `threads` | CPU threads for inference (0 = auto) | `0` |
| `context_size` | LLM context window size | `2048` |
| `log_level` | Logging: `"debug"`, `"info"`, `"warn"`, `"error"` | `"info"` |

Kernel command line parameters (set in GRUB):

| Parameter | Description |
|-----------|-------------|
| `llamaste.mode=server` | Headless server mode (default) |
| `llamaste.mode=desktop` | Desktop mode with Wayland compositor |
| `llamaste.mode=live` | Live ISO mode with installer |
| `ip=dhcp` | Enable kernel-level DHCP |
| `console=ttyS0` | Serial console output |

## Disk Layout

Llamaste uses a 5-partition GPT disk layout:

| # | Name | Type | Size | Purpose |
|---|------|------|------|---------|
| 1 | BIOS Boot | BIOS boot | 1 MB | GRUB legacy BIOS boot |
| 2 | ESP | FAT32 | 32 MB | UEFI boot, kernel, GRUB config |
| 3 | SYS-A | squashfs | ~6 MB | Active root filesystem (read-only) |
| 4 | SYS-B | (empty) | 256 MB | Standby root for A/B updates |
| 5 | DATA | ext4 | Remainder | Persistent data, models, config |

The root filesystem is immutable (squashfs). All persistent data lives on the DATA partition at `/data/`:
- `/data/models/` -- GGUF model files
- `/data/llamaste/config/` -- Configuration
- `/data/llamaste/conversations/` -- Chat history
- `/data/llamaste/logs/` -- System logs

## Troubleshooting

### Black screen on boot
- Try serial console: Connect a serial cable and use `screen /dev/ttyUSB0 115200`
- Or add `console=ttyS0` to the kernel command line in GRUB

### No network
- Llamaste requires a wired Ethernet connection (WiFi not supported in Phase 1)
- Check cable connection
- Verify DHCP server is running on your network
- IP is assigned via kernel DHCP (`ip=dhcp`)

### Can't find the web UI
- The IP address is printed on the serial console at boot
- Try `http://llamaste.local` (mDNS discovery)
- Default port is 80; if running as non-root, port 8080

### Installation fails
- Ensure the target disk is large enough (minimum 1 GB, recommended 8+ GB)
- Check that the target disk is not the boot device
- Try a different disk or USB port

### Model not loading
- Verify the model file is in `/data/models/`
- Check the model filename matches the expected pattern (e.g., `qwen2.5-7b-instruct-q4_k_m.gguf`)
- Verify enough RAM for the model (see System Requirements table)
