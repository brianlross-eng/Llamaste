# Llamaste

**A bootable Linux image where the LLM IS the operating system.**

Llamaste is a single C++ binary that combines llama.cpp inference, an agent loop,
62 system tools, and a web UI -- and runs as PID 1. The Linux kernel handles
hardware; the LLM handles everything else: file management, system monitoring,
networking, scheduling, and help -- all through natural conversation in your
browser.

---

## Features

### Core

- Single C++ binary (~11,000 LOC) runs as PID 1 -- no init system, no shell, no BusyBox
- CPU-only inference via llama.cpp with AVX2 SIMD (~14 tok/s on a 1.5B Q4_K_M model)
- Qwen2.5-Instruct models auto-selected by available RAM (0.5B to 32B)
- Boots in ~2 seconds on real hardware
- Immutable root filesystem (squashfs) with persistent ext4 data partition

### Web UI

- Dark terminal-themed interface with four tabs: Chat, Files, Dashboard, System
- Streaming responses via Server-Sent Events
- Inline tool call cards showing what was executed and the result
- File browser with upload, preview, and folder creation
- Live system health dashboard (CPU, RAM, disk, temperature, network)
- Proactive notifications for health alerts, model status, and cluster events

### Networking and Discovery

- WiFi manager: scan, connect, disconnect, forget, signal strength
- mDNS responder: reachable at `http://llamaste.local` on your LAN
- DNS-SD advertisement (`_mcp._tcp`) for automatic MCP client discovery

### Mesh Clustering

- Multi-node distributed inference via llama.cpp RPC
- mDNS peer discovery with automatic model upgrade when cluster RAM expands
- Dead-peer crash recovery in ~10 seconds

### Updates and Reliability

- A/B root partitions (SYS-A / SYS-B) with GRUB boot counter rollback
- Ed25519-signed OTA update bundles
- PID 1 supervisor with hardware watchdog, crash backoff, and automatic child respawn

### Voice

- Neural text-to-speech via sherpa-onnx Piper VITS (20 voices)
- Whisper speech-to-text in desktop mode
- Wake-phrase pipeline for hands-free operation

### Integration

- MCP server exposes all 62 tools to Claude Desktop and other MCP clients
- OpenAI-compatible chat completions API
- Device authentication with bcrypt password hashing and session tokens

---

## Quick Start

1. Download `llamaste.iso` from the releases page.
2. Write it to a USB drive with [Etcher](https://etcher.balena.io/) or [Rufus](https://rufus.ie/).
3. Boot from the USB drive and select "Llamaste Live + Install" from GRUB.
4. Open `http://<IP>:80` from another device and click "Install to Disk".
5. Reboot, set a device password, and download a model from the Dashboard tab.

For detailed installation instructions (USB, raw image, VirtualBox, QEMU), see
[INSTALL.md](INSTALL.md).

---

## System Requirements

| Component | Minimum        | Recommended     |
|-----------|----------------|-----------------|
| CPU       | x86-64 (any)   | x86-64 with AVX2 |
| RAM       | 4 GB           | 8--16 GB        |
| Disk      | 8 GB           | 32+ GB          |
| Network   | Wired or WiFi  | Wired Ethernet  |
| Boot      | BIOS or UEFI   | UEFI            |

RAM determines which model is loaded automatically:

| RAM   | Model        | Parameters |
|-------|--------------|------------|
| 4 GB  | Qwen2.5-0.5B | 0.5 B      |
| 8 GB  | Qwen2.5-7B   | 7 B        |
| 16 GB | Qwen2.5-14B  | 14 B       |
| 32 GB | Qwen2.5-32B  | 32 B       |

---

## Architecture

Llamaste runs as three layers:

```
+-------------------------------------------------+
|                    Web UI                        |
|  Vanilla JS + SSE, dark terminal theme           |
+-------------------------------------------------+
|                llamaste binary                   |
|  PID 1 supervisor + child process                |
|  HTTP server | Agent loop | 62 tools             |
|  llama.cpp inference | mDNS | Scheduler | TTS   |
+-------------------------------------------------+
|                 Linux kernel                     |
|  Hardware drivers, DRM/KMS, networking, storage  |
+-------------------------------------------------+
```

The `llamaste` binary is a single C++ executable (musl libc) that runs as
Linux's init process. It mounts filesystems, detects hardware, selects a model,
serves the web UI, runs the agent loop, dispatches tool calls, manages WiFi,
advertises via mDNS, and -- in desktop mode -- launches a Wayland compositor
with a fullscreen kiosk browser.

The PID 1 supervisor forks a child process for all user-facing functionality.
If the child crashes, the supervisor respawns it with exponential backoff. A
hardware watchdog ensures the system recovers from kernel-level hangs.

For the full technical reference (source layout, API endpoints, build internals,
thread model, disk layout), see [DEVELOPER.md](DEVELOPER.md).

---

## Built-in Tools

Llamaste includes 62 tools organized into 14 suites. The LLM calls these
automatically during conversation, or you can request specific operations.

| Suite        | Tools |
|--------------|-------|
| fs           | list_directory, read_file, write_file, delete_file, disk_usage, search |
| process      | list, info |
| network      | interfaces, connections, dns_lookup, ping, set_ip, get_ip |
| system       | info, uptime, memory, temperature, shutdown, reboot |
| config       | get, set, list, reset |
| model        | list, info, current, recommended, search, files, download, usb_import |
| schedule     | create, list, delete, update |
| auth         | change_password, get_api_key, set_session_timeout |
| audio        | status, transcribe, speak, config, download_model |
| install      | list_disks, install, progress |
| wifi         | status, scan, connect, disconnect, list, forget |
| cluster      | status, peers, reload, capacity, models |
| update       | status, check, install, rollback |
| voice        | list_tts, download_tts |

Destructive operations (shutdown, reboot, delete_file, install) require a
confirmation flag.

---

## Building from Source

Llamaste is built with [Buildroot](https://buildroot.org/) on Linux. A full
build takes 20--40 minutes; incremental rebuilds take 1--3 minutes.

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install -y build-essential gcc g++ make cmake \
  git wget cpio unzip rsync bc file python3 \
  libncurses-dev libssl-dev qemu-system-x86 \
  grub-common grub-pc-bin grub-efi-amd64-bin \
  xorriso mtools dosfstools e2fsprogs

# Clone
git clone https://github.com/brianlross-eng/Llamaste.git
cd Llamaste

# Set up Buildroot
git clone https://github.com/buildroot/buildroot.git /root/llamaste-build
cd /root/llamaste-build
make BR2_EXTERNAL=/path/to/Llamaste/br2-external llamaste_x86_64_defconfig
make -j$(nproc)

# Build the bootable ISO (optional)
/path/to/Llamaste/scripts/build-iso.sh /root/llamaste-build
```

Build outputs appear in `/root/llamaste-build/output/images/`:

| File              | Description                 |
|-------------------|-----------------------------|
| llamaste.img      | Raw disk image              |
| llamaste.iso      | Bootable ISO with installer |
| bzImage           | Linux kernel                |
| rootfs.squashfs   | Root filesystem             |

For detailed build instructions and the full API reference, see
[DEVELOPER.md](DEVELOPER.md).

---

## License

Copyright 2026 Brian Ross.

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for the
full text.

Qwen2.5 models are licensed under Apache 2.0 by Alibaba Cloud.
