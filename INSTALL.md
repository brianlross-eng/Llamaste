# Llamaste 1.0.000a -- Installation & User Guide

Llamaste is a bootable Linux operating system where the AI IS the operating system. A single binary handles everything above the kernel -- file management, system monitoring, scheduling, and help -- all through natural conversation in your web browser.

---

## What's Included in 1.0.000a (Alpha)

- **Real AI inference** -- Qwen2.5 models via llama.cpp. Download from the Dashboard with one click; model auto-selected by RAM tier
- **MCP server** -- expose all 44 tools to Claude Desktop and other MCP clients via `http://llamaste.local/mcp`
- **mDNS auto-discovery** -- `http://llamaste.local` on your LAN; MCP clients can also discover via DNS-SD (`_mcp._tcp`)
- **Voice I/O (desktop mode)** -- say "Llamaste, what time is it?" and hear a spoken response (whisper.cpp STT + Flite TTS)
- **Redesigned web UI** -- status bar, four tabs (Chat, Files, Dashboard, System), dark terminal theme
- **Proactive notifications** -- startup toast, health alerts (RAM / disk / temperature / model not loaded)
- **Heartbeat scheduler** -- cron-style and interval tasks; live SSE notifications
- **Desktop mode** -- fullscreen kiosk browser (Cage Wayland + Cog) on a locally attached display
- **44 built-in tools** across 11 categories
- **Device authentication** -- password-protected web UI and API, bcrypt hashing, session tokens
- **A/B root partitions** -- reserved for future OTA updates

---

## System Requirements

| Component | Minimum           | Recommended          |
|-----------|-------------------|----------------------|
| CPU       | x86-64 (any)      | x86-64 with AVX2     |
| RAM       | 4 GB              | 8--16 GB             |
| Disk      | 8 GB              | 32+ GB               |
| Network   | Wired Ethernet    | Wired Ethernet       |
| Boot      | BIOS or UEFI      | UEFI                 |

RAM determines which AI model Llamaste will load automatically:

| RAM    | Model           | Parameters | Quality |
|--------|-----------------|------------|---------|
| 4 GB   | Qwen2.5-0.5B    | 0.5 B      | Basic   |
| 8 GB   | Qwen2.5-7B      | 7 B        | Good    |
| 16 GB  | Qwen2.5-14B     | 14 B       | Great   |
| 32 GB  | Qwen2.5-32B     | 32 B       | Best    |

WiFi is not supported. You need a wired Ethernet connection with a DHCP server on your network.

---

## Installation Methods

### Method 1: USB Install (Recommended)

This is the simplest path. You boot from a USB drive, then install to your machine's internal disk.

1. **Download** `llamaste.iso` from the releases page.
2. **Write the ISO to a USB drive** using one of these tools:
   - [Etcher](https://etcher.balena.io/) (Windows, Mac, Linux -- graphical, easiest)
   - [Rufus](https://rufus.ie/) (Windows)
   - Command line (Linux/Mac):
     ```bash
     sudo dd if=llamaste.iso of=/dev/sdX bs=4M status=progress
     sync
     ```
     Replace `/dev/sdX` with your USB drive. Check with `lsblk` first.
3. **Boot from the USB drive.** Enter your BIOS/UEFI setup (usually F2, F12, or Del at power-on) and select the USB drive as the boot device.
4. **Select "Llamaste Live + Install"** from the GRUB menu.
5. **Open the web UI.** From another device on the same network, go to `http://<IP>:80`. The IP address is displayed on the console screen.
6. **Click "Install to Disk"** in the live-mode banner at the top of the web UI.
7. **Select your target disk** and confirm. The installer writes the OS image and expands the data partition to fill the disk.
8. **Reboot.** Remove the USB drive and restart the machine.

### Method 2: Raw Image (Headless / Automated)

For servers or scripted deployments where you can write directly to a disk.

1. **Download** `llamaste-x86_64.img.xz`.
2. **Decompress and write** to the target disk:
   ```bash
   xzcat llamaste-x86_64.img.xz | sudo dd of=/dev/sdX bs=4M status=progress
   sync
   ```
3. **Boot** the machine from that disk.
4. **Access the web UI** at `http://<IP>:80` from another device.

The raw image ships with a small data partition. If you used the ISO installer (Method 1), the data partition is automatically expanded. With the raw image, you may want to grow it manually using standard partition tools.

### Method 3: VirtualBox

VirtualBox is ideal for testing Llamaste without dedicated hardware.

1. **Create a new VM** in VirtualBox:
   - Name: `Llamaste`
   - Type: Linux, Version: Other Linux (64-bit)
   - RAM: **4096 MB** (4 GB minimum)
   - CPUs: **2**
   - Create a virtual hard disk: **16 GB** (VDI, dynamically allocated)

2. **Enable EFI** (recommended):
   - VM Settings > System > check **Enable EFI**

3. **Set up networking:**
   - VM Settings > Network > Adapter 1 > Attached to: **NAT**
   - Click **Advanced > Port Forwarding**
   - Add a rule: Name=`HTTP`, Protocol=`TCP`, Host Port=`8080`, Guest Port=`80`

4. **Attach the ISO:**
   - VM Settings > Storage > click the empty CD icon > Choose a disk file > select `llamaste.iso`

5. **Start the VM** and select "Llamaste Live + Install" from GRUB.

6. **Install:** Open `http://localhost:8080` in your host browser, click "Install to Disk", select the virtual hard disk, and confirm.

7. **Remove the ISO:** After installation, go to VM Settings > Storage and remove the ISO from the virtual CD drive.

8. **Reboot the VM.** Llamaste boots from the virtual hard disk. The web UI is available at `http://localhost:8080`.

### Method 4: QEMU (Quick Testing)

QEMU is useful for developers and CI pipelines.

**Boot the raw image:**
```bash
qemu-system-x86_64 \
  -m 512M \
  -nographic \
  -drive file=llamaste.img,format=raw,if=virtio \
  -kernel output/images/bzImage \
  -append "console=ttyS0 root=/dev/vda3 rootfstype=squashfs ro \
           init=/opt/llamaste/llamaste ip=dhcp" \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80
```

**Boot the ISO:**
```bash
qemu-system-x86_64 \
  -m 512M \
  -nographic \
  -cdrom llamaste.iso \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80
```

**Test the full install flow:**
```bash
# Create an empty target disk
qemu-img create -f raw target.img 4G

# Boot ISO with the target disk attached
qemu-system-x86_64 \
  -m 512M -nographic \
  -cdrom llamaste.iso \
  -drive file=target.img,format=raw,if=virtio \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80

# After installing via the web UI, boot from the target disk
qemu-system-x86_64 \
  -m 512M -nographic \
  -drive file=target.img,format=raw,if=virtio \
  -net nic,model=e1000 -net user,hostfwd=tcp::8080-:80
```

Then open `http://localhost:8080` in your browser.

---

## First Boot

When Llamaste boots, you will see kernel messages on the console (or serial output) followed by:

```
[llamaste] HTTP server listening on 0.0.0.0:80
```

Boot takes roughly 1--2 seconds on real hardware. The console displays the machine's IP address.

**To access the web UI**, open a browser on another device (or the host machine, if using a VM) and go to:

- `http://<IP address>:80` (the IP shown on the console), or
- `http://llamaste.local` (mDNS -- works on most networks), or
- `http://localhost:8080` (if using VirtualBox/QEMU with port forwarding)

### First Login

The first time you access the web UI you will be prompted to set a device password. This password protects the web interface and API. After setting it, log in and you are ready to use Llamaste.

### Adding a Model

On first boot with no model present, a **"No AI Model Loaded"** alert will appear. Download a model from the Dashboard tab:

1. Click the **Dashboard** tab → **Download Recommended Model** button.
2. Llamaste downloads the best Qwen2.5 model for your RAM (1.5 B, 3 B, 7 B, or 14 B) from Hugging Face.
3. After download completes, the server restarts inference automatically.

Alternatively, copy a GGUF file to `/data/models/` and reboot — Llamaste selects the best model for your available RAM.

---

## Web UI Guide

The web UI has three zones: a **status bar** across the top, **tab content** in the middle, and a **tab bar** across the bottom.

### Status Bar

The status bar is always visible and shows at a glance:

| Element        | Description                                      |
|----------------|--------------------------------------------------|
| LLAMASTE       | Wordmark / home indicator                        |
| Clock          | Current time                                     |
| Model name     | Active model (or "--" in stub mode)              |
| Speed          | Inference speed in tokens per second             |
| RAM            | Memory usage                                     |
| IP             | Network address                                  |
| Connection dot | Green = connected, yellow = reconnecting         |
| Bell badge     | Unread notification count                        |

### Chat Tab

The Chat tab is your primary interface. Type a message and Llamaste responds in natural language. When the AI uses a built-in tool (reading a file, checking system status, etc.), you will see a **tool call card** showing what was executed and the result.

Features:
- Multiple conversations (click **+** to start a new one)
- Streaming responses via Server-Sent Events
- Tool call results displayed inline with expandable details
- Markdown rendering in responses

### Files Tab

A graphical file browser for the `/data/` partition.

- **Navigate** directories by clicking folder names
- **Preview** text files by clicking them (content appears in the right pane)
- **Upload** files using the Upload button in the toolbar
- **Create folders** with the New Folder button
- **Breadcrumb path** at the top shows your current location

All file operations are restricted to `/data/` for security.

### Dashboard Tab

A live system health overview with auto-refreshing cards:

- **CPU** -- usage percentage with progress bar, temperature reading
- **Memory** -- used vs. total RAM with progress bar
- **Storage** -- used vs. total space on `/data/` with progress bar
- **Hardware** -- CPU model, core count, GPU (if detected), AVX2 support
- **Network** -- IP address, boot mode, system uptime
- **Model** -- active model name and inference speed

### System Tab

Administrative information and settings:

- **Model Management** -- shows the active model
- **MCP Server** -- API endpoint URL, full Bearer token (copy button), Claude Desktop config snippet, key regeneration
- **Scheduled Tasks** -- lists recurring tasks created via the scheduler
- **Network** -- static/DHCP configuration
- **About** -- version and project information

### Notifications

Toast notifications slide in from the top-right corner. A badge on the notification bell shows the unread count.

| Notification | When |
|---|---|
| **Llamaste Ready** | Once at startup — shows the server URL |
| **No AI Model Loaded** | 60 seconds after boot if no model is present (repeats every 30 min) |
| **High RAM / Disk / Temperature** | When usage exceeds threshold (RAM > 85%, Disk > 90%, Temp > 80 °C) |
| **Scheduled task fired** | When a recurring task runs and produces output |

---

## Built-In Tools

Llamaste includes 44 tools across 11 categories. The AI calls these automatically during conversation, or you can ask for specific operations.

| Category      | Tools |
|---------------|-------|
| **fs.**       | list_directory, read_file, write_file, delete_file, disk_usage, search |
| **process.**  | list, info |
| **network.**  | interfaces, connections, dns_lookup, ping, set_ip_static, set_ip_dhcp |
| **system.**   | info, uptime, memory, temperature, shutdown, reboot |
| **config.**   | get, set, list, reset |
| **model.**    | list, info, current, recommended, search, files, download, usb_import |
| **schedule.** | create, list, delete, update |
| **auth.**     | set_password, get_status |
| **audio.**    | status, transcribe, speak, config, download_model |
| **install.**  | list_disks, install, progress |
Destructive operations (shutdown, reboot, delete_file, install.to_disk) require a confirmation flag to execute.

---

## Connecting Claude Desktop (MCP)

Llamaste exposes all 44 tools to Claude Desktop and other MCP clients via the Model Context Protocol.

### Quick Setup

1. Open the Llamaste web UI → **System** tab → **MCP Server** card.
2. Copy the pre-filled **Claude Desktop configuration snippet** (it includes the live API key and device IP).
3. Paste it into `%APPDATA%\Claude\claude_desktop_config.json` (Windows) or `~/.config/Claude/claude_desktop_config.json` (Mac/Linux).
4. Restart Claude Desktop.

### Manual Configuration

```json
{
  "mcpServers": {
    "llamaste": {
      "type": "http",
      "url": "http://llamaste.local/mcp",
      "headers": {
        "Authorization": "Bearer <your-64-hex-api-key>"
      }
    }
  }
}
```

Get your API key from **System → MCP Server** in the web UI. Replace `llamaste.local` with the device IP if mDNS is unavailable on your network.

### Auto-Discovery

MCP clients that support DNS-SD can discover Llamaste automatically — it advertises as `_mcp._tcp.local` on your LAN.

---

## Configuration

Llamaste stores its configuration at `/data/llamaste/config/llamaste.json`:

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

| Key            | Description                                          | Default      |
|----------------|------------------------------------------------------|--------------|
| `model`        | `"auto"` (select by RAM) or path to a .gguf file     | `"auto"`     |
| `listen`       | IP address the HTTP server binds to                  | `"0.0.0.0"`  |
| `port`         | HTTP server port                                     | `80`         |
| `threads`      | CPU threads for inference (0 = auto-detect)          | `0`          |
| `context_size`  | LLM context window size in tokens                   | `2048`       |
| `log_level`    | Logging verbosity: debug, info, warn, error          | `"info"`     |

You can edit this file directly on the data partition, or use the `config.*` tools through the Chat tab (for example: "set the log level to debug").

---

## Boot Modes

Llamaste supports three boot modes, selected via the GRUB menu or kernel command line.

| Mode      | Kernel Parameter         | Description                                   |
|-----------|--------------------------|-----------------------------------------------|
| Server    | `llamaste.mode=server`   | Headless. HTTP server only. Default mode.     |
| Desktop   | `llamaste.mode=desktop`  | Launches a Wayland compositor with a fullscreen browser showing the web UI on the local display. |
| Live      | `llamaste.mode=live`     | Boots from ISO/USB with a tmpfs data partition. Enables the installer tools for writing to disk. |

**Server mode** is the default. The machine boots, starts the HTTP server, and waits for connections. There is no graphical display.

**Desktop mode** starts a Cage Wayland compositor running the Cog kiosk browser pointed at the local web UI. This turns any machine with a monitor into a self-contained AI terminal. Desktop mode requires the graphics packages (included in the Buildroot image when built with desktop support).

**Live mode** activates automatically when booting from the ISO. The data partition is a temporary RAM disk. Use the web UI's "Install to Disk" feature to write Llamaste to a permanent disk.

---

## Disk Layout

Llamaste uses a 5-partition GPT disk layout designed for reliability and future A/B updates.

| # | Name      | Filesystem | Size       | Purpose                              |
|---|-----------|------------|------------|--------------------------------------|
| 1 | BIOS Boot | --         | 1 MB       | GRUB legacy BIOS boot code           |
| 2 | ESP       | FAT16      | 32 MB      | UEFI boot: kernel, GRUB, EFI files   |
| 3 | SYS-A     | squashfs   | ~6 MB      | Active root filesystem (read-only)   |
| 4 | SYS-B     | (reserved) | 256 MB     | Standby root for A/B updates         |
| 5 | DATA      | ext4       | Remainder  | Persistent data, models, config      |

The root filesystem is **immutable** (compressed squashfs). The OS cannot be modified at runtime. All persistent data lives on the DATA partition:

| Path                           | Contents                    |
|--------------------------------|-----------------------------|
| `/data/models/`                | GGUF model files            |
| `/data/llamaste/config/`       | Configuration files         |
| `/data/llamaste/conversations/`| Chat history                |
| `/data/llamaste/logs/`         | System logs                 |

---

## Troubleshooting

### Black screen on boot

- If using a monitor, the kernel may be sending output to a serial port. Try connecting a serial cable and using `screen /dev/ttyUSB0 115200`.
- In GRUB, edit the kernel line and add `console=tty0` to force VGA output.
- For VirtualBox, ensure the VM type is set to Linux 64-bit and EFI is enabled in System settings.

### No network connection

- Llamaste requires a **wired Ethernet** connection. WiFi is not supported.
- Check that the cable is connected and your network has a DHCP server.
- The kernel obtains an IP address via `ip=dhcp` at boot time. If DHCP fails, no IP is assigned.

### Cannot find the web UI

- The IP address is printed on the console at boot. Look for the line containing `HTTP server listening`.
- Try `http://llamaste.local` -- this uses mDNS and works on most home and office networks.
- If using VirtualBox with NAT, make sure port forwarding is configured (host 8080 to guest 80) and access `http://localhost:8080`.
- If using QEMU with `-net user,hostfwd=tcp::8080-:80`, access `http://localhost:8080`.

### Installation fails

- Ensure the target disk is at least 1 GB (8 GB recommended).
- The target disk must not be the currently booted device.
- In VirtualBox, make sure the virtual hard disk is attached before booting the ISO.
- Try a different disk or USB port on physical hardware.

### Model not loading

- Verify the model file is in `/data/models/`.
- The filename must end in `.gguf`.
- Confirm you have enough RAM for the model (see the System Requirements table).
- Check logs at `/data/llamaste/logs/` for error messages.

### VirtualBox tips

- **EFI must be enabled** in VM Settings > System for UEFI boot to work.
- Use **NAT** networking with port forwarding (host 8080 -> guest 80) for easy access.
- If the VM shows a UEFI shell instead of GRUB, the ISO may not be attached. Check Storage settings.
- After installing, **remove the ISO** from the virtual CD drive before rebooting.
- Graphics controller: VMSVGA works best with Linux guests.

---

## Building from Source

Llamaste is built with Buildroot on Linux. A full build from scratch takes 20--40 minutes; incremental rebuilds take 1--3 minutes.

```bash
# Install build dependencies (Ubuntu/Debian)
sudo apt update
sudo apt install -y build-essential gcc g++ make cmake \
  git wget cpio unzip rsync bc file \
  python3 libncurses-dev libssl-dev \
  qemu-system-x86 \
  grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools \
  dosfstools e2fsprogs genimage

# Clone and build
git clone https://github.com/nicholasgasior/llamaste.git
cd llamaste
git clone https://github.com/buildroot/buildroot.git /root/llamaste-build
cd /root/llamaste-build
make BR2_EXTERNAL=/path/to/llamaste/br2-external llamaste_x86_64_defconfig
make -j$(nproc)

# Build the ISO (optional)
/path/to/llamaste/scripts/build-iso.sh /root/llamaste-build
```

Output files appear in `/root/llamaste-build/output/images/`:

| File                 | Description                    | Size    |
|----------------------|--------------------------------|---------|
| `llamaste.img`       | Raw disk image                 | ~360 MB |
| `llamaste.iso`       | Bootable ISO with installer    | ~400 MB |
| `bzImage`            | Linux kernel                   | ~5 MB   |
| `rootfs.squashfs`    | Root filesystem                | ~6 MB   |

For detailed build instructions, architecture documentation, and the API reference, see [DEVELOPER.md](DEVELOPER.md).

---

## Known Limitations (Alpha)

- **No WiFi**: Only wired Ethernet is supported. WiFi drivers are not included.
- **Voice requires desktop mode**: The whisper + Flite voice pipeline only activates in desktop mode (boots into kiosk browser). Server mode (headless) has no voice to save RAM.
- **Single-user**: There are no multi-user accounts. One device password protects everything.
- **No HTTPS**: The HTTP server does not support TLS. Do not expose it to the public internet without a reverse proxy.
- **CPU inference only**: No GPU acceleration in this release. Inference speed scales with CPU cores and AVX2 support.
- **No OTA updates yet**: A/B partitions are reserved; update mechanism is planned for Phase 4.

---

## License

Llamaste is released under the [Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0).

Qwen2.5 models are licensed under Apache 2.0 by Alibaba Cloud.
