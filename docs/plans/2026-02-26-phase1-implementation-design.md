# Phase 1 Implementation Design: Headless LLM-OS MVP

**Created**: 2026-02-26
**Status**: APPROVED (auto-approved per user preference)
**Approach**: Bottom-Up Build (Approach A)

---

## Goal

Boot a USB/disk image → single C++ binary serves AI chat with system management tools over HTTP. The LLM IS the operating system. No shell, no init system, no separate daemons.

---

## Architecture Summary

```
User (browser) ──HTTP──▶ llamaste binary (PID 1 supervisor)
                              │
                              ├── fork() child: inference + agent + tools + web UI
                              │       ├── llama-server (ggml inference engine)
                              │       ├── Agent loop (conversation + tool dispatch)
                              │       ├── Embedded tools (fs/process/network/system/config/model)
                              │       ├── Web UI (vanilla JS + SSE, embedded)
                              │       └── HTTP API (OpenAI-compatible)
                              │
                              └── PID 1 stays as minimal supervisor
                                      ├── Watchdog (/dev/watchdog)
                                      ├── Signal handling (SIGCHLD → re-fork)
                                      └── Graceful shutdown (SIGTERM → save state)

Linux Kernel (~5-8 MB, no modules, built-in drivers)
    Memory │ Threads │ Network │ Storage │ NIC drivers

Hardware (any x86-64 PC, 8-32 GB RAM)
```

### Critical Pattern: Supervisor/Child Fork (from research/17)

PID 1 does NOT run inference. It:
1. Mounts filesystems, configures network, detects hardware
2. Selects model based on RAM
3. `fork()`s a child process that runs the full llama-server + agent
4. Stays alive as supervisor — if child crashes (SIGCHLD), re-fork
5. Pings hardware watchdog every 30s
6. On SIGTERM: signals child to save state, waits, unmounts, powers off

This prevents kernel panic on inference crash.

---

## Partition Layout (5-partition, A/B ready from day one — research/20)

```
┌──────────┬──────────┬──────────┬──────────┬──────────────────┐
│ BIOS     │ ESP      │ SYS-A    │ SYS-B    │ DATA             │
│ 1 MB     │ 256 MB   │ 256 MB   │ 256 MB   │ remainder        │
│ grub_bios│ FAT32    │ squashfs │ squashfs │ ext4             │
│ MBR boot │ GRUB+EFI │ rootfs   │ (empty)  │ models, config,  │
│          │          │          │ future   │ conversations    │
└──────────┴──────────┴──────────┴──────────┴──────────────────┘
```

- **BIOS**: For legacy BIOS boot (GRUB stage 1)
- **ESP**: GRUB2 + EFI binaries, `grub.cfg`
- **SYS-A**: squashfs root with kernel + llamaste binary + busybox-less minimal rootfs
- **SYS-B**: Empty in Phase 1, reserved for A/B updates (Phase 4 or earlier)
- **DATA**: ext4, persists across updates — models, user data, config, conversations, audit log

---

## Build Order (12 Steps)

### Step 1: Buildroot Environment
- Clone Buildroot (latest LTS)
- Create `br2-external/` tree with `external.mk`, `external.desc`, `Config.in`
- Configure: x86_64, musl, no init (custom), no BusyBox (or minimal for debug)
- Verify: `make` produces a root filesystem

### Step 2: Minimal Kernel Config
- Start from `x86_64_defconfig`, strip to essentials
- Built-in (not modules): AHCI, NVMe, USB storage, e1000e, r8169, virtio (QEMU), ext4, squashfs, FAT32, proc, sysfs, devtmpfs auto-mount
- Target: ~5-8 MB bzImage
- Verify: boots to "No init found" panic in QEMU (expected)

### Step 3: Stock llama-server in Buildroot
- Add llama.cpp as Buildroot package (`br2-external/package/llamaste/`)
- Build stock llama-server with `GGML_CPU_ALL_VARIANTS=ON`
- Static link with musl
- Set `init=/opt/llamaste/llama-server` temporarily
- Verify: boots in QEMU, serves on port 8080, responds to API calls from host

### Step 4: PID 1 Supervisor
- Replace stock llama-server with custom `main.cpp`
- Implement supervisor pattern:
  - Mount /proc, /sys, /dev (devtmpfs), /data (ext4)
  - Parse /proc/cmdline for `llamaste.mode=`
  - Detect hardware (CPU features, RAM, GPU)
  - Select model from DATA partition
  - `fork()` child → child `exec()`s or calls llama-server main loop
  - Parent stays as supervisor with watchdog + SIGCHLD handler
- Verify: boots, child process serves HTTP, killing child causes re-fork (not panic)

### Step 5: Tools System
- `tools.h/cpp`: Tool registry, JSON schema generation, dispatch
- `tools_fs.cpp`: list, read, write, delete, search, disk_usage (restricted to /data)
- `tools_process.cpp`: list, info (reads /proc)
- `tools_network.cpp`: interfaces, connections, dns_lookup, ping
- `tools_system.cpp`: info, uptime, memory, temperature, shutdown, reboot
- `tools_config.cpp`: get, set, list, reset (JSON config on /data)
- `tools_model.cpp`: list, info, load, unload, download (from /data/models)
- Each tool: C++ function → JSON result, registered with schema for function calling
- Verify: unit tests on host, tool dispatch works via test harness

### Step 6: Agent Loop
- `agent.h/cpp`: Conversation state, message history, tool call parsing
- Integrates with llama-server's `/chat/completions` endpoint (internal call)
- Flow: user message → build messages array (system + history + user) → inference → parse tool calls → dispatch → append result → re-infer → return final response
- System prompt builder: dynamic prompt describing hardware, available tools, current state
- Conversation persistence: save/load from /data/conversations/
- Verify: agent responds to "What CPU is this?" by calling system.info tool

### Step 7: Web UI
- `web/index.html`: Single page app
- `web/chat.js`: SSE streaming, message rendering, tool call visualization
- `web/dashboard.js`: System stats (CPU, RAM, disk, model, uptime)
- `web/style.css`: Clean, responsive design
- Embed into binary at compile time (xxd-style, CMake custom command)
- Served at `/` by the HTTP server
- Verify: chat works in browser, tool calls show results, dashboard updates

### Step 8: HTTP API (OpenAI-compatible)
- `/v1/chat/completions` — standard OpenAI format (already in llama-server)
- `/v1/models` — list loaded model
- `/llamaste/tools` — list available tools (custom endpoint)
- `/llamaste/system` — system dashboard data (custom endpoint)
- `/llamaste/conversations` — conversation management (custom endpoint)
- SSE streaming for all chat endpoints
- Verify: `curl` from host works, external clients (ChatBox, etc.) can connect

### Step 9: Network Configuration
- DHCP client: implement minimal DHCP in C++ (~300 LOC) or use a tiny static binary
  - Cannot use `udhcpc` (no BusyBox/shell) — must be built-in or a static companion
- mDNS responder: advertise `llamaste.local` on the network
- Verify: QEMU instance gets IP, accessible from host by name

### Step 10: GRUB Configuration
- `grub.cfg` with dual menu: Server / Desktop (Desktop = Phase 2, grayed out or hidden)
- 5-second timeout, Server as default
- Kernel cmdline: `init=/opt/llamaste llamaste.mode=server console=tty0`
- Both BIOS and UEFI boot paths
- Verify: GRUB menu appears, selection boots correctly

### Step 11: Genimage Configuration
- `genimage.cfg`: 5-partition layout (BIOS + ESP + SYS-A + SYS-B + DATA)
- SYS-A: squashfs from Buildroot rootfs output
- SYS-B: empty squashfs or raw partition
- DATA: ext4, pre-populated with default config
- Output: `llamaste.img` (raw disk image)
- Verify: `dd` to USB, boots on real hardware (or `llamaste.img.xz` for distribution)

### Step 12: End-to-End Testing in QEMU
- Boot `llamaste.img` in QEMU with 8GB RAM
- Verify: web UI at http://localhost from host
- Test: "Show disk usage" → tool call → result
- Test: "List network interfaces" → tool call → result
- Test: "What CPU is this?" → tool call → result
- Test: API endpoint with curl
- Test: conversation persistence across reboot
- Test: kill child process → supervisor re-forks
- Test: model auto-selection based on RAM

---

## Speed Optimizations (Phase 1 Subset)

| Technique | Expected Impact | Effort |
|-----------|----------------|--------|
| KV cache reuse for system prompt | ~200ms saved per request | Free (llama.cpp built-in) |
| Grammar-constrained JSON for tools | 100x faster tool output parsing | Low (~50 LOC) |
| Thread tuning (latency not throughput) | 10-30% faster single-user | Low (config) |
| GGML_CPU_ALL_VARIANTS | Best SIMD for hardware | Free (build flag) |

Semantic cache + speculative decoding deferred to Phase 1.5 or Phase 2.

---

## Security Model (Phase 1)

- No authentication by default (single-user appliance assumption)
- Optional API key for remote access (set via web UI config)
- Destructive tools (delete, shutdown, factory_reset) require confirmation in chat UI
- Filesystem tools restricted to /data partition
- Audit log at /data/llamaste/audit.log (all tool executions)
- Tool results truncated to 500 chars in logs (research/19)
- No arbitrary code execution — tools are a fixed compiled set

---

## RAM Budget (Phase 1, Server Mode Only)

| System RAM | Model | Context | OS/Agent Overhead |
|-----------|-------|---------|-------------------|
| 8 GB | Qwen2.5-7B Q4_K_M (4.5 GB) | ~2 GB KV cache | ~1.5 GB |
| 16 GB | Qwen2.5-14B Q4_K_M (8.7 GB) | ~4 GB KV cache | ~3 GB |
| 32 GB | Qwen2.5-32B Q4_K_M (19.5 GB) | ~8 GB KV cache | ~4.5 GB |

---

## Success Criteria

1. Boots from raw disk image to serving in <30 seconds
2. Web UI chat works — user can converse with the AI
3. Tool calls work — "Show disk usage", "List processes", "What's my IP?"
4. OpenAI-compatible API works — external clients can connect
5. Model auto-selected based on RAM
6. Supervisor pattern works — killing inference child doesn't crash the system
7. Conversations persist across reboot
8. Image size < 100 MB (excluding models)

---

## Development Environment

- **Build machine**: Any Linux (WSL2 on Windows works for Buildroot)
- **Test**: QEMU x86_64 with 8-16 GB RAM
- **Source**: `D:\Llamaste\src\llamaste\` (main code) + `D:\Llamaste\br2-external\` (Buildroot integration)
- **Models for testing**: Qwen2.5-1.5B-Instruct Q4_K_M (~1 GB, fits in any QEMU config)

---

## Dependencies on Research Findings

| Research Doc | Finding | Impact on Phase 1 |
|-------------|---------|-------------------|
| 17 (Failure Modes) | Supervisor/child fork pattern | Core architecture pattern |
| 20 (Updates) | 5-partition A/B layout from day one | Partition layout |
| 16 (Licensing) | Apache 2.0 for our code, ship Apache models only | License file in image |
| 19 (Privacy) | Audit log redaction, 500-char truncation | Tool logging |
| 21 (Power) | Thermal state machine basics | CPU temp monitoring |
| 18 (Multi-user) | No-auth default, API key optional | Security model |
| 13 (Syscalls) | ~15-20 syscalls needed | Kernel config validation |
