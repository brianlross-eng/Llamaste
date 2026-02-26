# Llamaste v2: LLM IS the OS — Implementation Plan

## Project Summary

Llamaste is a bootable Linux image where a single binary — `llamaste` — IS the entire operating system above the kernel. The kernel handles hardware; the LLM handles everything else. The user interacts with their computer through an AI that can manage files, configure the network, launch apps, monitor the system, and answer questions — all through natural conversation or voice.

The system serves double duty: a local/remote AI server for home/small business AND a usable PC/desktop when needed.

---

## Architecture: Three Layers, Not Seven

```
┌─────────────────────────────────────────────────┐
│  USER INTERFACE                                  │
│  Web UI (chat + dashboard) served by llamaste    │
│  + Optional: Wayland compositor for desktop apps │
│  + Optional: Voice I/O (whisper.cpp + piper)     │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  LLAMASTE BINARY (single static binary, PID 1)  │
│                                                  │
│  ┌──────────────┐ ┌───────────────┐             │
│  │ llama-server │ │ Agent Loop    │             │
│  │ (inference)  │ │ (conversation │             │
│  │              │ │  + tool       │             │
│  │ ggml engine  │ │  dispatch)    │             │
│  │ KV cache     │ │              │              │
│  │ SIMD dispatch│ │ System prompt │             │
│  │ HTTP API     │ │ Tool defs     │             │
│  └──────────────┘ └───────┬───────┘             │
│                           │                      │
│  ┌────────────────────────▼─────────────────┐   │
│  │ EMBEDDED TOOLS (compiled into binary)     │   │
│  │ fs.* | process.* | network.* | system.*   │   │
│  │ config.* | model.* | app.* | cluster.*    │   │
│  └───────────────────────────────────────────┘   │
│                                                  │
│  ┌───────────────────────────────────────────┐   │
│  │ WEB UI (embedded static HTML/JS/CSS)      │   │
│  │ Chat interface + system dashboard          │   │
│  └───────────────────────────────────────────┘   │
└────────────────────┬────────────────────────────┘
                     │ syscalls only (~15-20)
┌────────────────────▼────────────────────────────┐
│  LINUX KERNEL (minimal, ~5-8 MB)                 │
│  Memory (mmap, mlock, huge pages)                │
│  Threads (clone, scheduling)                     │
│  Network (TCP/IP, UDP multicast)                 │
│  Storage (AHCI, NVMe, USB)                       │
│  NIC drivers (e1000e, r8169)                     │
│  Display (DRM/KMS for desktop mode)              │
└────────────────────┬────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────┐
│  HARDWARE (any x86-64 PC, 8-32 GB RAM)          │
└─────────────────────────────────────────────────┘
```

**The key idea**: `llamaste` is ONE binary that combines llama-server + agent loop + system tools + web UI. It runs as PID 1 (the kernel executes it directly). There is no shell, no init system, no separate daemons. The LLM binary IS the userspace.

---

## Architecture Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Binary architecture | Single static C++ binary (llamaste = llama-server + agent + tools + web UI) | One process, zero IPC, PID 1 capable |
| Language for tools | C++ (extend llama-server directly) | No additional runtime, reuse llama.cpp build system |
| Agent protocol | Internal function calls (same process) | Zero overhead, tools are compiled in |
| Build system | **Buildroot** | Simpler, faster iteration, smaller images |
| C library | **musl** | ~600 KB vs glibc ~8-10 MB. Static linking. |
| Init system | **None** — binary IS PID 1 | No init system overhead, binary IS the OS |
| Root filesystem | **squashfs + zstd** | Read-only, compressed, power-loss immune |
| Bootloader | **GRUB2** | Supports both BIOS and UEFI, dual-boot menu |
| Boot modes | Dual-boot: Server (headless) / Desktop (GUI) via GRUB | Same image, kernel cmdline selects mode |
| Web UI | Vanilla JS + SSE, embedded in binary | No build tools, no dependencies, <100 KB |
| Compositor | Cage (Phase 2 kiosk) → Labwc (Phase 2 windowed) | Minimal Wayland, both in Buildroot |
| Voice | whisper.cpp + piper (optional companion binaries) | Local, no cloud dependency |
| Default models | **Qwen2.5-Instruct family** (Apache 2.0) | Best quality per tier, unrestricted license, good tool-use |
| CPU dispatch | **GGML_CPU_ALL_VARIANTS=ON** | Builds multiple SIMD variants, runtime auto-selects |
| Image format | **.img.xz** (primary) + **.iso** (secondary) | Raw disk image for USB; hybrid ISO for compatibility |

---

## Boot Modes (GRUB Menu)

```
┌───────────────────────────────────────┐
│  Llamaste                             │
│                                       │
│  > Llamaste Server  (headless, web)   │
│    Llamaste Desktop (GUI + AI chat)   │
│                                       │
│  Press Enter to boot...        (5s)   │
└───────────────────────────────────────┘
```

- **Server mode**: PID 1 = llamaste binary, no display. Access via web UI from another device. Best for headless servers, old PCs, maximum RAM for the model.
- **Desktop mode**: PID 1 = llamaste binary, starts Wayland compositor + browser kiosk. AI chat interface on screen. Can launch apps (Chrome, terminal). Needs display drivers in kernel.
- Same image, same binary, different kernel cmdline (`llamaste.mode=server` vs `llamaste.mode=desktop`).

---

## Boot Sequence (Target: <30 seconds to serving)

```
1. BIOS/UEFI loads GRUB from ESP or BIOS-BOOT partition
2. GRUB reads llamaste_slot variable, loads kernel from SYS-A (or SYS-B)
3. Kernel starts llamaste binary as PID 1 (supervisor)
4. PID 1 supervisor init sequence:
   a. Mount /proc, /sys, /dev (devtmpfs), /tmp, /data (ext4)
   b. Parse /proc/cmdline for boot mode (server/desktop)
   c. Detect hardware (CPU features, RAM, GPU via /proc + /sys)
   d. Set hostname, configure network (built-in DHCP client)
   e. Tune CPU performance (governor, huge pages, swappiness)
   f. Auto-select best model for available RAM tier
   g. Open /dev/watchdog (hardware watchdog, 60s timeout)
   h. fork() child process for inference
5. Child process:
   a. Load model into memory (mmap + mlock)
   b. Start HTTP server on port 80
   c. Serve web UI, API, agent loop with tools
   d. If desktop mode: fork/exec Wayland compositor
6. Supervisor stays alive:
   a. Ping watchdog every 30s
   b. Monitor child via SIGCHLD — re-fork on crash
   c. Handle SIGTERM → graceful shutdown (save state, unmount, poweroff)
7. Web UI accessible at http://<ip>/ from any device on LAN
8. OpenAI-compatible API at http://<ip>/v1/
```

This supervisor/child pattern (per research/17) ensures that an inference crash
causes a quick restart (~5s) instead of a kernel panic.

---

## Partition Layout

```
Part  Label       Type       FS          Size        Mount      Purpose
1     BIOS-BOOT   raw        -           1 MB        -          GRUB core.img (BIOS boot)
2     ESP         FAT32      FAT32       256 MB      /boot/efi  EFI bootloader + GRUB + kernel
3     SYS-A       squashfs   squashfs    256 MB      / (ro)     Root filesystem (active)
4     SYS-B       squashfs   squashfs    256 MB      -          Root filesystem (standby, A/B updates)
5     DATA        ext4       ext4        Remainder   /data (rw) Models, config, conversations, logs
```

A/B update layout included from Phase 1 (per research/20). SYS-B is empty initially but
reserved so the partition table never needs to change. GRUB reads a `llamaste_slot=A|B`
variable to know which system partition to boot. The DATA partition auto-expands to fill
the USB/disk on first boot.

---

## Tool System Architecture

The LLM replaces traditional OS management tools through function calling:

### Tool Categories

| Category | Tools | Replaces |
|----------|-------|----------|
| **Filesystem** | fs.list_directory, fs.read_file, fs.write_file, fs.move, fs.delete, fs.search, fs.disk_usage | File manager, shell commands |
| **Process** | process.list, process.start, process.stop, process.status | Task manager, systemd |
| **Network** | network.interfaces, network.configure, network.test, network.scan_lan | NetworkManager, ifconfig |
| **System** | system.info, system.reboot, system.shutdown, system.logs | System settings, journalctl |
| **Config** | config.get, config.set, config.export, config.import, config.factory_reset | /etc files, settings GUI |
| **Model** | model.list, model.download, model.load, model.info | Package manager |
| **App** (Phase 2) | app.launch, app.list, app.focus, app.close, app.install | Desktop environment |
| **Cluster** (Phase 3) | cluster.status, cluster.join, cluster.leave | N/A (new capability) |

### How Function Calling Works

User says: "What's using the most RAM?"

```
1. Agent builds request with tool definitions → llama-server
2. LLM generates: tool_call("process_list", {sort_by: "memory"})
3. Agent dispatches to embedded tools_process.cpp
4. tools_process.cpp reads /proc → returns JSON
5. Agent appends result to conversation → sends back to LLM
6. LLM responds: "llama-server is using 8.2 GB (your loaded model).
   The kernel uses 340 MB. Everything else is under 50 MB."
```

---

## Speed Optimization Strategy

Priority-ordered techniques to achieve sub-500ms for most OS tool calls:

| # | Technique | Impact | Cost |
|---|-----------|--------|------|
| 1 | KV cache reuse for system prompt | Skip re-processing prompt every request | Free (built into llama.cpp) |
| 2 | Grammar-constrained tool output | 100x faster JSON generation | Enable in llama.cpp |
| 3 | Semantic cache for common queries | <10ms for cached patterns | ~200 lines of code |
| 4 | N-gram speculative decoding | 1.8-2.5x speedup, no draft model | llama.cpp flag |
| 5 | Dual-model strategy (Phase 2) | 0.5B for simple tools, 7B+ for reasoning | Model management logic |
| 6 | Thread tuning | Optimize latency not throughput | Boot-time auto-config |
| 7 | Hardware-specific dispatch | Auto-select best SIMD path | Build flag |

### Expected Performance (All Optimizations Applied)

| Query Type | Technique | Expected Latency |
|-----------|-----------|-----------------|
| Cached OS command | Pattern match | <10 ms |
| Simple tool call (0.5B model) | Small model + grammar | 50-200 ms |
| Simple tool call (7B model) | KV cache + grammar + speculative | 200-500 ms |
| Complex reasoning (14B model) | Full inference | 1-3 seconds |
| Multi-step task (14B, 3+ tools) | Multiple inference rounds | 3-10 seconds |

---

## Security Model

- **Confirmation gates**: Destructive operations (delete, shutdown, reset) require explicit user confirmation via chat UI
- **Allowlist, not blocklist**: Tools are a bounded set of pre-defined operations, NOT arbitrary code execution
- **Path restrictions**: Filesystem tools restricted to /data by default
- **Rate limiting**: Maximum tool calls per conversation turn
- **Audit log**: All tool executions logged to /data/llamaste/audit.log
- **No arbitrary code**: Unlike Open Interpreter, NO eval(), exec(), or shell command execution
- **Power user escape hatch**: Terminal app can be launched for raw shell access (Phase 2)

---

## RAM Budget Analysis

### 8 GB System (Minimum)
```
Kernel + base:     ~200 MB
llamaste binary:   ~50 MB
Web UI overhead:   ~50 MB
Available for LLM: ~7.5 GB
→ Qwen2.5 7B Q4_K_M (4.5 GB) + 3 GB for KV cache + context
→ No desktop, headless/web-only mode
```

### 16 GB System (Recommended)
```
Kernel + base:     ~200 MB
llamaste binary:   ~50 MB
Compositor + UI:   ~100 MB
Chromium:          ~300 MB
Available for LLM: ~15 GB
→ Qwen2.5 14B Q4_K_M (8.7 GB) + 6 GB for KV cache + context
→ Full desktop with browser
```

### 32 GB System (Ideal)
```
Kernel + base:     ~200 MB
llamaste binary:   ~50 MB
Compositor + UI:   ~100 MB
Chromium:          ~400 MB
Voice (optional):  ~500 MB
Available for LLM: ~30 GB
→ Qwen2.5 32B Q4_K_M (19.5 GB) + 10 GB context
→ Full desktop + voice + large context
```

---

## Repository Structure

```
llamaste/
├── src/llamaste/                    # Extended llama-server source
│   ├── main.cpp                     # PID 1 init + server startup
│   ├── agent.cpp / agent.h          # Agent loop, conversation state
│   ├── tools.cpp / tools.h          # Tool registry + dispatch
│   ├── tools_fs.cpp                 # Filesystem tools
│   ├── tools_process.cpp            # Process tools
│   ├── tools_network.cpp            # Network tools
│   ├── tools_system.cpp             # System info/control tools
│   ├── tools_config.cpp             # Configuration tools
│   ├── tools_model.cpp              # Model management tools
│   ├── prompt_builder.cpp           # Dynamic system prompt
│   ├── CMakeLists.txt               # Build system
│   ├── embed_web.cmake              # Web UI embedding script
│   └── web/                         # Embedded web UI
│       ├── index.html
│       ├── chat.js
│       ├── dashboard.js
│       └── style.css
├── br2-external/                    # Buildroot external tree
│   ├── external.desc
│   ├── external.mk
│   ├── Config.in
│   ├── configs/
│   │   └── llamaste_x86_64_defconfig
│   ├── package/
│   │   └── llamaste/
│   │       ├── Config.in
│   │       └── llamaste.mk          # Buildroot package recipe
│   └── board/
│       └── llamaste/
│           ├── linux.config          # Minimal kernel config
│           ├── genimage.cfg          # Disk image layout
│           ├── grub.cfg              # GRUB dual-boot config
│           ├── post_build.sh
│           ├── post_image.sh
│           └── overlay/
│               ├── opt/llamaste/     # Binary install target
│               └── etc/              # Config overlay
├── research/                        # 15 research documents
├── models/
│   ├── download-models.sh
│   └── model-manifest.json
├── tests/
│   ├── test-qemu-bios.sh
│   ├── test-qemu-uefi.sh
│   └── test-inference.sh
├── tools/
│   ├── build.sh                     # One-command build
│   └── flash.sh                     # Flash image to USB
├── LLAMASTE-IMPLEMENTATION-PLAN.md  # THIS FILE
├── SESSION-STATUS.md                # Session progress tracker
├── CLAUDE.md                        # AI assistant context
└── README.md
```

---

## Phase 1: Headless LLM-OS (MVP)

**Goal**: Boot → single C++ binary serves AI chat with system management tools over HTTP.

### Steps

1. **Set up Buildroot environment**
   - Clone Buildroot, pin to latest stable release
   - Create BR2_EXTERNAL tree structure
   - Configure: x86_64, musl, no init system, GRUB2

2. **Create minimal kernel config**
   - ~5-8 MB, all drivers built-in (no modules)
   - Include: SMP, huge pages, ext4, squashfs, networking, NVMe, SATA, USB storage
   - Exclude: sound, graphics (beyond framebuffer), wireless, bluetooth, media

3. **Build stock llama-server first**
   - Verify it boots and serves inference in QEMU
   - Build flags: GGML_STATIC=ON, BUILD_SHARED_LIBS=OFF, GGML_CPU_ALL_VARIANTS=ON
   - Static link against musl

4. **Extend llama-server with tools system**
   - tools.h/tools.cpp: Tool registry, JSON schema generation, dispatch
   - tools_fs.cpp: Filesystem operations (list, read, write, search, disk usage)
   - tools_process.cpp: Process management (list, start, stop via /proc)
   - tools_network.cpp: Network operations (interfaces, test, DHCP)
   - tools_system.cpp: System info (hostname, CPU, RAM, uptime, temp)
   - tools_config.cpp: Configuration CRUD + export/import
   - tools_model.cpp: Model management (list, download, load, switch)

5. **Build the agent loop**
   - agent.h/agent.cpp: Conversation state, tool dispatch, multi-turn
   - Process LLM tool_call responses → execute tool → append result → re-infer
   - Handle confirmation gates for destructive operations

6. **Build the system prompt builder**
   - prompt_builder.cpp: Dynamically describes current hardware/state
   - Includes tool definitions, system context, user preferences

7. **Build the web UI**
   - Chat interface with streaming (SSE)
   - System dashboard (CPU, RAM, disk, model info)
   - Vanilla JS, no build tools, <100 KB total

8. **Embed web UI into binary**
   - embed_web.cmake: xxd-style embedding at compile time
   - Binary serves static files from compiled-in data

9. **PID 1 bootstrap**
   - Mount /proc, /sys, /dev, /tmp, /data
   - DHCP network configuration
   - Hardware detection (CPU features, RAM tier)
   - Auto-select and load best model
   - Start HTTP server on port 80

10. **GRUB config**
    - Dual boot menu (Server / Desktop)
    - `init=/opt/llamaste llamaste.mode=server` vs `llamaste.mode=desktop`

11. **genimage config**
    - GPT: BIOS-boot(1MB) + ESP(256MB) + SYS-A(256MB) + SYS-B(256MB) + DATA(ext4, remainder)
    - A/B layout from day one (SYS-B empty initially, reserved for updates per research/20)

12. **Test in QEMU**
    ```bash
    qemu-system-x86_64 -m 8G -smp 4 \
      -drive file=llamaste.img,format=raw,if=virtio \
      -netdev user,id=net0,hostfwd=tcp::80-:80 \
      -device virtio-net-pci,netdev=net0 \
      -nographic
    ```
    - Verify: boot, web UI, tool calls, API compatibility

### Phase 1 Success Criteria
- [ ] Boots in QEMU (<30 seconds to serving inference)
- [ ] User can manage system entirely through AI chat
- [ ] OpenAI-compatible API works for external clients
- [ ] Model auto-selected based on RAM
- [ ] All filesystem, process, network, system tools functional
- [ ] Web UI chat + dashboard accessible from browser

---

## Phase 2: Desktop Mode + Voice

**Goal**: The "Llamaste Desktop" GRUB option boots to a graphical AI interface.

### What to Add

1. **Wayland compositor**
   - Cage initially: fullscreen Chromium showing llamaste web UI
   - Labwc later: multiple windows managed by the AI
   - llamaste binary detects `llamaste.mode=desktop` and fork/execs compositor

2. **App tools** embedded in llamaste binary
   - app.launch(name) → spawns process, gets window ID
   - app.close(window_id), app.focus(window_id), app.list()

3. **Pre-built app packages** on DATA partition
   - Chromium (web browsing)
   - Terminal emulator (power user shell access)
   - mpv (media playback)
   - zathura (PDF/document viewing)

4. **Kernel additions for desktop**
   - DRM/KMS display drivers (i915, amdgpu, nouveau, VESA)
   - Input drivers (evdev, libinput)
   - Sound drivers (ALSA) for voice I/O

5. **Voice I/O** (optional)
   - whisper.cpp for STT, piper for TTS
   - Voice tools: voice.listen() → text, voice.speak(text)

6. **Dual-model strategy**
   - Tiny model (0.5-1.5B) for fast tool dispatch (<200ms)
   - Main model (7-14B) for reasoning, conversation
   - Agent routes based on query complexity

### Phase 2 Success Criteria
- [ ] Desktop mode boots to graphical AI interface
- [ ] Can launch/close/switch apps via AI chat
- [ ] Voice interaction works (speak to system, hear response)
- [ ] Fast tool calls via small model (<200ms for simple operations)

---

## Phase 3: Mesh + MCP + Proactive Agent

**Goal**: Multiple nodes work together; external AI tools connect; proactive behavior.

1. **Mesh clustering**
   - UDP multicast discovery embedded in llamaste binary
   - llama-rpc-server for distributed inference
   - Coordinator election, layer distribution across nodes

2. **MCP server** embedded in llamaste
   - Streamable HTTP transport on port 8081
   - Tools, resources, prompts exposed to Claude Desktop / VS Code
   - mDNS advertisement of MCP service

3. **Proactive agent**
   - Periodic health checks ("Your disk is getting full")
   - Update notifications
   - Background task management

4. **AI desktop agent** (OpenClaw-like)
   - Optional higher-level AI layer on top of llamaste
   - Can use a cloud model to orchestrate the local LLM
   - Screenshot-based control for complex GUI tasks
   - Connects to messaging platforms (Telegram, Slack)

### Phase 3 Success Criteria
- [ ] 2+ nodes auto-discover and pool RAM
- [ ] 70B model runs across 4x 16GB machines
- [ ] Claude Desktop connects to MCP endpoint
- [ ] Proactive notifications work

---

## Phase 4: Polish + Community

1. **A/B partition updates** (atomic, rollback on failure)
2. **ARM64 support** (Raspberry Pi 5, Orange Pi)
3. **ISO builder** (custom images with pre-selected models)
4. **Community skill repository** (downloadable tool packs)
5. **Secure Boot** chain
6. **Pre-built images** for common hardware

### Phase 4 Success Criteria
- [ ] A/B updates work with automatic rollback
- [ ] ARM64 image boots on Raspberry Pi
- [ ] ISO builder produces custom images
- [ ] Community-ready documentation and repository

---

## Key Technical Risks

| Risk | Impact | Mitigation |
|------|--------|------------|
| musl + static llama.cpp build issues | Blocks Phase 1 | Test early. Alpine Docker container for validation. |
| LLM tool-call accuracy | Poor user experience | Use Qwen2.5-Instruct (best open-source tool-calling). Grammar-constrained JSON output. |
| Latency too high for OS operations | Feels sluggish | Speed optimization stack (KV cache, grammar, speculative, semantic cache). |
| Diverse hardware NIC drivers | Boots without network | Ship broad kernel with many drivers built-in. |
| Model too large for RAM | Can't load | Auto-select smaller model. Clear error messages. |
| UEFI Secure Boot | Can't boot on some machines | Phase 4 priority. Users can disable initially. |

---

## Development Environment

### Requirements
- **Linux build host** (Ubuntu 22.04+ or similar) — Buildroot requires Linux
- If developing on Windows: WSL2 or a Linux VM
- ~20 GB disk for Buildroot build tree
- QEMU for testing (no physical hardware needed initially)

### One-Command Build

```bash
#!/bin/bash
set -euo pipefail
BUILDROOT_VERSION="2024.02.1"
BR2_EXTERNAL="$(pwd)/br2-external"
OUTPUT="$(pwd)/output"

if [ ! -d buildroot ]; then
    git clone --depth 1 --branch "$BUILDROOT_VERSION" \
        https://gitlab.com/buildroot.org/buildroot.git
fi

cd buildroot
make BR2_EXTERNAL="$BR2_EXTERNAL" O="$OUTPUT" llamaste_x86_64_defconfig
make BR2_EXTERNAL="$BR2_EXTERNAL" O="$OUTPUT" -j$(nproc)

echo "Image ready: $OUTPUT/images/llamaste.img"
```

### Testing

```bash
# QEMU BIOS boot
qemu-system-x86_64 -m 8192 -smp 4 \
  -drive file=output/images/llamaste.img,format=raw,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::80-:80 \
  -device virtio-net-pci,netdev=net0 \
  -nographic

# QEMU UEFI boot (requires OVMF)
qemu-system-x86_64 -m 8192 -smp 4 \
  -bios /usr/share/OVMF/OVMF_CODE.fd \
  -drive file=output/images/llamaste.img,format=raw,if=virtio \
  -netdev user,id=net0,hostfwd=tcp::80-:80 \
  -device virtio-net-pci,netdev=net0 \
  -nographic
```

---

## Sources

See `research/` directory for detailed research documents (15 docs covering Buildroot, llama.cpp, bootable images, CPU optimization, mesh clustering, appliance patterns, model selection, GPU+RAM hybrid, CPU speed projects, API development, MCP servers, minimal OS alternatives, syscall surface, LLM-as-OS paradigm, and LLM speed optimization).
