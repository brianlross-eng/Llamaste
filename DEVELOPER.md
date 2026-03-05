# Llamaste 1.0.000a -- Technical Reference

This document covers the architecture, source code, APIs, build system, testing, and internals of the Llamaste LLM-OS. It is intended for developers, testers, troubleshooters, and anyone who wants to understand how the system works.

---

## Architecture Overview

Llamaste runs as three layers:

```
+------------------------------------------------------------------+
|                           Web UI                                  |
|  vanilla JS + SSE, 4 tabs (Chat, Files, Dashboard, System)       |
|  8 files: index.html, chat.js, dashboard.js, files.js,           |
|  system.js, notifications.js, install.js, style.css              |
+------------------------------------------------------------------+
                              |
+------------------------------------------------------------------+
|                      llamaste binary                              |
|  PID 1 supervisor + child process                                 |
|  HTTP server (cpp-httplib) | Agent loop | 32 tools               |
|  Threads: scheduler, mDNS, compositor (desktop), SSE             |
+------------------------------------------------------------------+
                              |
+------------------------------------------------------------------+
|                       Linux kernel                                |
|  6.6.70, all built-in, no modules                                 |
|  DRM/KMS, evdev, virtio, AHCI, NVMe, EFI, ACPI, DHCP            |
+------------------------------------------------------------------+
```

The `llamaste` binary is a single static C++ executable (musl libc, ~6 MB stripped) that runs as PID 1 (Linux's init process). There is no init system, no shell, and no BusyBox. The binary handles everything above the kernel: mounting filesystems, detecting hardware, serving the web UI, running the agent loop, dispatching tool calls, scheduling tasks, running the mDNS responder, and (in desktop mode) launching the Wayland compositor.

### PID 1 Supervisor/Child Pattern

When running as PID 1, the binary forks into two processes:

- **Supervisor (PID 1)**: Monitors the child via `waitpid()`, kicks the hardware watchdog every 10 seconds, handles crash recovery with backoff (2s normally, 30s after 3 rapid crashes within 60 seconds), and manages clean shutdown (sync, unmount, power off via `reboot(RB_POWER_OFF)`).
- **Child**: Runs the HTTP server, agent loop, mDNS responder, scheduler thread, notification SSE, and all user-facing functionality. If the child crashes, the supervisor respawns it.

When not running as PID 1 (e.g., during development on a host machine), `child_main()` is called directly without forking.

### Threads

The child process runs several background threads:

| Thread | Purpose | Lifecycle |
|--------|---------|-----------|
| HTTP server | Main thread, `svr.listen()` blocks | Entire child lifetime |
| Scheduler | Checks tasks/alerts every 10s | `g_scheduler.start()` to `stop()` |
| mDNS responder | Listens on UDP port 5353 for `.local` queries | `mdns.start()` to `stop()` |
| Compositor (desktop only) | Polls for HTTP readiness, then `fork/exec` cage | Detached, desktop mode only |
| Shutdown watcher | Polls `g_running` every 500ms, calls `svr.stop()` | Detached, entire child lifetime |
| SSE connections | One thread per connected SSE client (notifications, chat, install) | Per-connection |

---

## Source Code Layout

### `src/llamaste/`

| File | LOC | Purpose |
|------|-----|---------|
| `main.cpp` | ~200 | Entry point. Mounts filesystems, detects hardware, selects model, starts supervisor or child. |
| `supervisor.cpp` | ~150 | PID 1 supervisor loop: fork, watchdog, crash recovery, shutdown. |
| `supervisor.h` | ~30 | `SupervisorConfig` struct and `supervisor_run()` declaration. |
| `init.cpp` | ~250 | Boot-time setup: mount proc/sys/dev/tmp/run/devpts, mount DATA partition, tune perf, set hostname, parse boot mode. |
| `init.h` | ~20 | Init function declarations. |
| `child_main.cpp` | ~1070 | HTTP server with all routes: static files, agent chat (SSE), dashboard, tools, conversations, files API, schedules REST, notifications SSE, OpenAI API, health, installer, desktop compositor launch. |
| `hwdetect.cpp` | ~180 | Hardware detection: CPU model/cores, RAM, GPU, AVX2/AVX-512 from `/proc/cpuinfo` and `/proc/meminfo`. Also exports `read_meminfo_kb()` and `read_sysfs_line()` helpers used by the scheduler. |
| `hwdetect.h` | ~40 | `HardwareInfo` struct and detection functions. |
| `agent.cpp` | ~300 | Agent loop: builds inference requests, parses tool calls from OpenAI-format responses, dispatches tools, multi-turn conversation with 4000-char result truncation. |
| `agent.h` | ~115 | `ConversationState`, `ToolCall`, `Message` structs, `agent_turn()` function, `build_inference_request()`. |
| `prompt_builder.cpp` | ~150 | Builds the system prompt dynamically from hardware info, available tools, and boot mode. |
| `prompt_builder.h` | ~10 | `build_system_prompt()` declaration. |
| `tools.cpp` | ~100 | `ToolRegistry` implementation: register, dispatch, generate OpenAI-compatible tool definitions. |
| `tools.h` | ~60 | `ToolDef` struct, `ToolRegistry` class, and registration function declarations for all 8 categories. |
| `tools_fs.cpp` | ~420 | Filesystem tools: `fs.list_directory`, `fs.read_file`, `fs.write_file`, `fs.delete_file`, `fs.disk_usage`, `fs.search`. All restricted to `/data/`. |
| `tools_process.cpp` | ~220 | Process tools: `process.list`, `process.info`. |
| `tools_network.cpp` | ~380 | Network tools: `network.interfaces`, `network.connections`, `network.dns_lookup`, `network.ping`. |
| `tools_system.cpp` | ~380 | System tools: `system.info`, `system.uptime`, `system.memory`, `system.temperature`, `system.shutdown`, `system.reboot`. |
| `tools_config.cpp` | ~350 | Config tools: `config.get`, `config.set`, `config.list`, `config.reset`. Manages `/data/llamaste/config/llamaste.json`. |
| `tools_model.cpp` | ~290 | Model tools: `model.list`, `model.info`, `model.current`. |
| `tools_install.cpp` | ~1150 | Installer tools (live mode only): `install.detect_disks`, `install.to_disk`, `install.progress`. Pure C++ disk operations: GPT manipulation with packed structs + CRC32, XZ decompression, partition resize. |
| `tools_schedule.cpp` | ~165 | Schedule tools: `schedule.create`, `schedule.list`, `schedule.delete`, `schedule.update`. Thin wrappers that delegate to the `Scheduler` class. |
| `scheduler.h` | ~105 | `ScheduledTask`, `AlertConfig`, `Notification` structs, `Scheduler` class declaration. |
| `scheduler.cpp` | ~850 | Scheduler implementation: background thread, cron parser, task CRUD, alert monitoring (RAM/disk/temp), persistence, notification queue. |
| `net_mdns.cpp` | ~350 | Multicast DNS responder for `llamaste.local` discovery. Listens on UDP 5353, responds to A-record queries. Includes `get_local_ip()` static method. |
| `net_mdns.h` | ~30 | `MdnsResponder` class. |
| `embed_web.cmake` | ~95 | CMake script that converts web assets to C byte arrays (`const unsigned char[]`) for compile-time embedding. |
| `CMakeLists.txt` | ~85 | Build configuration: sources list, static linking option, liblzma detection, web embedding, install target. |
| `httplib.h` | ~8000 | cpp-httplib v0.18.3 single-header HTTP library (vendored). |
| `json.hpp` | ~24000 | nlohmann/json v3.11.3 single-header JSON library (vendored). |

### `src/llamaste/web/`

| File | Purpose |
|------|---------|
| `index.html` | Main page: status bar (clock, model, speed, RAM, IP, notifications), content area with 4 tabs, bottom tab bar (Chat, Files, Dashboard, System). |
| `chat.js` | Chat interface: message rendering, SSE streaming, conversation management, markdown formatting. |
| `dashboard.js` | Dashboard tab: CPU/temperature/RAM/disk progress bars, hardware info table, network info, model info. Also drives the status bar updates from `/llamaste/system` polling. |
| `files.js` | Files tab: file browser with directory navigation, file preview (text content display), breadcrumb path, file metadata. |
| `system.js` | System tab: system information display, scheduled tasks list with colored dot indicators, system settings. Fetches from `/llamaste/schedules` separately. |
| `notifications.js` | Notification system: SSE connection to `/llamaste/notifications`, toast popups with auto-dismiss, notification badge counter on status bar. |
| `install.js` | Installer UI (live mode only): disk detection, installation progress bar, success/error states. Self-detects live mode via `/health` endpoint. |
| `style.css` | All styling: dark theme, layout grid, status bar, tab navigation, chat bubbles, dashboard gauges, file browser, toast notifications, installer components. |

All web files use vanilla JavaScript with IIFE (Immediately Invoked Function Expression) pattern, `var` declarations (not `const`/`let`), and ES5 compatibility for maximum browser support in the embedded kiosk environment.

### `br2-external/`

Buildroot external tree:

```
br2-external/
  external.desc            # Declares this as a BR2_EXTERNAL tree ("LLAMASTE")
  external.mk              # Includes package makefiles
  Config.in                # Kconfig menu entries
  board/llamaste/
    linux.config            # Kernel configuration fragment (~190 lines)
    grub.cfg                # GRUB config for installed system (server + desktop entries)
    grub-live.cfg           # GRUB config for ISO live boot
    genimage.cfg            # Disk image layout (5-partition GPT)
    post_build.sh           # Runs after target build: creates directories, installs grub.cfg
    post_image.sh           # Runs after image gen: builds ESP, data partition, assembles disk
  configs/
    llamaste_x86_64_defconfig  # Buildroot defconfig (~75 lines)
  package/llamaste/
    Config.in               # Package Kconfig entry
    llamaste.mk             # Package build recipe (cmake-package)
```

### `scripts/`

| File | Purpose |
|------|---------|
| `host-test.sh` | Builds the binary on the host (cmake + make) and runs the HTTP server for manual testing. |
| `qemu-boot-test.sh` | QEMU E2E test suite: boots the disk image and validates 5 HTTP endpoints. |
| `qemu-run.sh` | Launches QEMU interactively with the disk image for manual testing. |
| `build-iso.sh` | Builds hybrid BIOS+UEFI ISO from Buildroot output using `grub-mkrescue`. Requires `grub-pc-bin grub-efi-amd64-bin xorriso mtools`. |
| `qemu-install-test.sh` | Full install flow test: boots ISO in QEMU, runs installer, verifies installed disk, reboots from installed disk. |
| `test-efi-boot.sh` | EFI-specific boot test: boots the disk image with QEMU + OVMF firmware to validate UEFI boot path. |
| `verify-wsl-env.sh` | Checks WSL2 environment prerequisites (gcc, cmake, qemu, etc.). |
| `buildroot-build.sh` | Automates the Buildroot build process from WSL2. |
| `qemu-test.sh` | Legacy QEMU test script (predecessor to `qemu-boot-test.sh`). |

---

## HTTP API Reference

All endpoints are served by the HTTP server in `child_main.cpp`. The server binds to port 80 when running as root, or port 8080 otherwise.

CORS headers (`Access-Control-Allow-Origin: *`) are set on all responses for development convenience. All `OPTIONS` requests return 204 No Content.

### Core Endpoints

#### `GET /health`

Health check endpoint. Returns immediately with no measurement delay.

**Response:**
```json
{
    "status": "ok",
    "uptime_seconds": 120,
    "model_loaded": false,
    "tools_count": 32,
    "mode": "server"
}
```

#### `GET /llamaste/system`

System dashboard data. Includes a ~100ms delay for CPU usage measurement (two `/proc/stat` samples).

**Response:**
```json
{
    "model": "stub (no model loaded)",
    "mode": "server",
    "uptime": 120,
    "uptime_seconds": 120,
    "ip": "192.168.1.100",
    "cpu_percent": 12.5,
    "temperature": 45.0,
    "temperature_c": 45.0,
    "ram_total_mb": 8192,
    "ram_used_mb": 1024,
    "disk_total_gb": 32.0,
    "disk_used_gb": 0.5,
    "cpu_model": "Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz",
    "cpu_cores": 6,
    "gpu_name": "",
    "gpu_detected": false,
    "has_avx2": true,
    "has_avx512": false,
    "tokens_per_sec": 0.0,
    "scheduled_tasks_count": 2,
    "next_scheduled_time": 1741036800
}
```

#### `GET /llamaste/tools`

Returns all registered tools as an OpenAI-compatible tools array.

**Response:**
```json
[
    {
        "type": "function",
        "function": {
            "name": "fs.list_directory",
            "description": "List files and directories at a given path",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {"type": "string", "description": "Directory path to list"}
                },
                "required": ["path"]
            }
        }
    }
]
```

### Chat Endpoints

#### `POST /llamaste/chat`

Agent chat endpoint with SSE streaming.

**Request:**
```json
{
    "message": "What files are in /data?",
    "conversation_id": "conv_123",
    "stream": true
}
```

If `conversation_id` is omitted, a new one is generated (format: `conv_{timestamp}_{counter}`). If `stream` is `true` (default), the response is an SSE stream.

**SSE events:**
```
data: {"type":"token","content":"I"}

data: {"type":"token","content":" can"}

data: {"type":"done","content":"I can help with that!","conversation_id":"conv_123"}

data: [DONE]
```

**Non-streaming response** (`stream: false`):
```json
{
    "conversation_id": "conv_123",
    "content": "I can help with that!",
    "message_count": 2
}
```

#### `POST /v1/chat/completions`

OpenAI-compatible chat completions API. Accepts the standard OpenAI request format.

**Request:**
```json
{
    "model": "llamaste",
    "messages": [
        {"role": "user", "content": "Hello"}
    ]
}
```

**Response:**
```json
{
    "id": "chatcmpl-stub-001",
    "object": "chat.completion",
    "model": "llamaste-stub",
    "choices": [{
        "index": 0,
        "message": {"role": "assistant", "content": "Hello! I'm Llamaste..."},
        "finish_reason": "stop"
    }],
    "usage": {"prompt_tokens": 0, "completion_tokens": 0, "total_tokens": 0}
}
```

#### `GET /llamaste/conversations`

List all conversations. Currently in-memory only (Phase 1).

**Response:**
```json
{
    "conversations": [
        {
            "id": "conv_123",
            "title": "What files are in /data?",
            "message_count": 4
        }
    ]
}
```

### Files Endpoint

#### `GET /llamaste/files`

File browser API. Delegates to `fs.list_directory` or `fs.read_file` tools.

**Parameters:**

| Param | Default | Description |
|-------|---------|-------------|
| `path` | `/data` | Path to list or read |
| `action` | `list` | `list` for directory listing, `read` for file contents |

**Example:**
```
GET /llamaste/files?path=/data/llamaste/config&action=list
GET /llamaste/files?path=/data/llamaste/config/llamaste.json&action=read
```

### Schedule Endpoints

#### `GET /llamaste/schedules`

List all scheduled tasks.

**Response:**
```json
{
    "tasks": [
        {
            "id": "a1b2c3d4",
            "enabled": true,
            "prompt": "Check disk usage and alert if above 80%",
            "cron": "0 * * * *",
            "at": 0,
            "every_seconds": 0,
            "once": false,
            "created": 1741036800,
            "last_run": 1741040400,
            "next_run": 1741044000
        }
    ],
    "count": 1
}
```

#### `POST /llamaste/schedules`

Create a new scheduled task.

**Request:**
```json
{
    "prompt": "Check system health and report issues",
    "every_seconds": 3600,
    "enabled": true
}
```

**Response:** Returns the created task object with generated `id` and computed `next_run`.

#### `DELETE /llamaste/schedules/:id`

Delete a scheduled task by ID.

**Response:**
```json
{
    "deleted": true,
    "id": "a1b2c3d4"
}
```

### Notification Endpoint

#### `GET /llamaste/notifications`

Server-Sent Events (SSE) stream for real-time notifications. Sends a heartbeat comment (`: heartbeat`) every 5 seconds to keep the connection alive. The endpoint polls `g_scheduler.drain_notifications()` every 500ms.

**Event format:**
```
data: {"type":"alert","title":"High RAM Usage","body":"RAM usage at 87% (3584 MB / 4096 MB used)","time":1741036800,"id":"e5f6a7b8"}
```

Notification types: `alert` (system health), `scheduled` (task fired), `reminder` (user-set).

### Installer Endpoints (Live Mode Only)

These endpoints are only registered when `llamaste.mode=live` is set on the kernel command line:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/install/disks` | GET | List available target disks (filters out CD-ROM, loop, dm, ram devices) |
| `/install/start` | POST | Begin installation (body: `{"device":"/dev/sda"}`) |
| `/install/progress` | GET | Poll installation progress (percent, stage, finished flag) |
| `/install/progress/stream` | GET | SSE stream of progress updates |

### Static File Routes

| Path | Source File | Content-Type |
|------|------------|-------------|
| `/` | `index.html` | `text/html` |
| `/style.css` | `style.css` | `text/css` |
| `/chat.js` | `chat.js` | `application/javascript` |
| `/dashboard.js` | `dashboard.js` | `application/javascript` |
| `/files.js` | `files.js` | `application/javascript` |
| `/system.js` | `system.js` | `application/javascript` |
| `/notifications.js` | `notifications.js` | `application/javascript` |
| `/install.js` | `install.js` | `application/javascript` |

In production builds (`LLAMASTE_HAS_EMBED` defined), these are served from byte arrays compiled into the binary. In development, they are read from the filesystem (searches `src/llamaste/web/`, `../src/llamaste/web/`, `web/`).

---

## Tools System

Tools are the LLM's interface to the operating system. Each tool has a name (dotted namespace), description, JSON Schema parameters, a handler function, and an optional confirmation flag.

### Tool Categories

| Category | Prefix | Count | File | Description |
|----------|--------|-------|------|-------------|
| Filesystem | `fs.*` | 6 | `tools_fs.cpp` | File and directory operations, restricted to `/data/` |
| Process | `process.*` | 2 | `tools_process.cpp` | Process listing and information |
| Network | `network.*` | 4 | `tools_network.cpp` | Network interfaces, connections, DNS, ping |
| System | `system.*` | 6 | `tools_system.cpp` | System info, memory, temperature, shutdown, reboot |
| Config | `config.*` | 4 | `tools_config.cpp` | Key-value configuration management |
| Model | `model.*` | 3 | `tools_model.cpp` | Model listing, info, current selection |
| Schedule | `schedule.*` | 4 | `tools_schedule.cpp` | Scheduled task CRUD |
| Install | `install.*` | 3 | `tools_install.cpp` | Disk installer (live mode only) |
| **Total** | | **32** | | **(29 always + 3 live-mode-only)** |

### Complete Tool Listing

**Filesystem (6 tools):**

| Tool | Description |
|------|-------------|
| `fs.list_directory` | List files and directories at a given path |
| `fs.read_file` | Read file contents (text, with size limit) |
| `fs.write_file` | Write or overwrite a file |
| `fs.delete_file` | Delete a file (requires confirmation) |
| `fs.disk_usage` | Check disk space usage for a path |
| `fs.search` | Search for files matching a pattern |

**Process (2 tools):**

| Tool | Description |
|------|-------------|
| `process.list` | List running processes |
| `process.info` | Get detailed info about a specific process by PID |

**Network (4 tools):**

| Tool | Description |
|------|-------------|
| `network.interfaces` | List network interfaces with IP addresses |
| `network.connections` | Show active network connections |
| `network.dns_lookup` | Resolve a hostname to IP addresses |
| `network.ping` | Ping a host and return latency |

**System (6 tools):**

| Tool | Description |
|------|-------------|
| `system.info` | Overall system information (CPU, RAM, disk, uptime) |
| `system.uptime` | System uptime and load averages |
| `system.memory` | Detailed memory usage breakdown |
| `system.temperature` | CPU temperature reading |
| `system.shutdown` | Power off the system (requires confirmation) |
| `system.reboot` | Reboot the system (requires confirmation) |

**Config (4 tools):**

| Tool | Description |
|------|-------------|
| `config.get` | Read a configuration value by key |
| `config.set` | Set a configuration value |
| `config.list` | List all configuration keys and values |
| `config.reset` | Reset configuration to defaults |

**Model (3 tools):**

| Tool | Description |
|------|-------------|
| `model.list` | List available model files in `/data/models/` |
| `model.info` | Get info about a specific model file (size, quantization) |
| `model.current` | Show the currently loaded model |

**Schedule (4 tools):**

| Tool | Description |
|------|-------------|
| `schedule.create` | Create a recurring or one-shot scheduled task |
| `schedule.list` | List all scheduled tasks with next run times |
| `schedule.delete` | Delete a scheduled task by ID |
| `schedule.update` | Update fields on an existing task |

**Install (3 tools, live mode only):**

| Tool | Description |
|------|-------------|
| `install.detect_disks` | Scan for available target disks |
| `install.to_disk` | Write the system image to a target disk (requires confirmation) |
| `install.progress` | Check installation progress |

### Tool Definition

```cpp
ToolDef tool;
tool.name = "custom.hello";
tool.description = "Say hello to a user";
tool.parameters = R"json({
    "type": "object",
    "properties": {
        "name": {
            "type": "string",
            "description": "Name of the person to greet"
        }
    },
    "required": ["name"]
})json";
tool.handler = [](const std::string& args_json) -> std::string {
    auto args = json::parse(args_json);
    std::string name = args.value("name", "world");
    json result;
    result["greeting"] = "Hello, " + name + "!";
    return result.dump();
};
tool.requires_confirmation = false;
```

**Important:** Use `R"json(...)json"` delimiters for raw string literals, NOT `R"(...)"`. Bare `)"` sequences inside JSON will break the parser.

### Adding a New Tool

1. Create `src/llamaste/tools_custom.cpp`:

```cpp
#include "tools.h"
#include "json.hpp"
using json = nlohmann::json;

static std::string handle_hello(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    std::string name = args.value("name", "world");
    json result;
    result["greeting"] = "Hello, " + name + "!";
    return result.dump();
}

void register_custom_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "custom.hello",
        .description = "Say hello to a user",
        .parameters = R"json({"type":"object","properties":{"name":{"type":"string","description":"Name to greet"}},"required":["name"]})json",
        .handler = handle_hello
    });
}
```

2. Add declaration to `tools.h`:
```cpp
void register_custom_tools(ToolRegistry& reg);
```

3. Call it in `tools.cpp` inside `register_all_tools()`:
```cpp
void register_all_tools(ToolRegistry& reg) {
    register_fs_tools(reg);
    // ...existing categories...
    register_custom_tools(reg);
}
```

4. Add to `CMakeLists.txt`:
```cmake
set(LLAMASTE_SOURCES
    # ...existing files...
    tools_custom.cpp
)
```

### Tool Security

- All `fs.*` tools validate paths against `/data/` -- path traversal (`../`) is rejected
- Destructive tools (`fs.delete_file`, `system.shutdown`, `system.reboot`) have `requires_confirmation = true`
- The agent's system prompt instructs the LLM to ask the user before calling confirmed tools
- Tool handlers catch all exceptions and return `{"error": "..."}` JSON
- Tool results are truncated to 4000 characters before being added to conversation history
- The agent loop limits tool-calling rounds to 5 (configurable via `max_rounds` parameter) to prevent infinite loops

---

## Scheduler Subsystem

The scheduler manages recurring and one-shot tasks that trigger LLM inference at specific times. It also monitors system health (RAM, disk, temperature) and pushes alerts to connected clients via SSE.

### Architecture

- **Files:** `scheduler.h` (declarations), `scheduler.cpp` (implementation)
- **Background thread:** runs `check_tasks()` and `check_alerts()` in a loop
- **Sleep interval:** 10 seconds, implemented as 10 x 1-second sleeps for responsive shutdown
- **Persistence:** tasks saved to `/data/llamaste/schedules.json` on every mutation
- **Atomic writes:** uses write-to-tmp + rename pattern (POSIX `rename()` is atomic on same filesystem)

### Task Types

| Type | Field | Behavior |
|------|-------|----------|
| Cron | `cron` | 5-field cron expression: `minute hour day month weekday`. Supports `*` (wildcard) and single integer values. Next run computed by scanning forward up to ~2 years. |
| Interval | `every_seconds` | Fires every N seconds. After firing, `next_run = now + every_seconds`. |
| One-shot | `at` | Fires once at the given Unix timestamp. After firing, the task is disabled (`enabled = false`) unless `once = true` (auto-delete). |

A task marked `once = true` is automatically removed from the task list after it fires.

### Alert Monitoring

The scheduler checks system health on every 10-second cycle:

| Alert | Source | Default Threshold | Cooldown |
|-------|--------|-------------------|----------|
| High RAM | `read_meminfo_kb("MemTotal"/"MemAvailable")` | 85% used | 300s (5 min) |
| High Disk | `statvfs("/data")` | 90% used | 300s (5 min) |
| High Temperature | `/sys/class/thermal/thermal_zone0/temp` | 80 C | 300s (5 min) |

Alert thresholds and cooldowns are configurable via `AlertConfig`. Each alert type has an independent cooldown timer (`last_ram_alert_`, `last_disk_alert_`, `last_temp_alert_`).

### Notification Queue

When a scheduled task fires or an alert triggers, a `Notification` struct is pushed to `pending_notifications_`. The SSE endpoint (`/llamaste/notifications`) calls `drain_notifications()` every 500ms to deliver them to connected clients.

Notification structure:
```json
{
    "id": "e5f6a7b8",
    "type": "alert|scheduled|reminder",
    "title": "High RAM Usage",
    "body": "RAM usage at 87% (3584 MB / 4096 MB used)",
    "time": 1741036800
}
```

### Thread Safety

The scheduler uses a two-mutex pattern:

- `mutex_` protects `tasks_`, `alerts_`, `inference_fn_`, `notify_fn_`, and `data_dir_`
- `notif_mutex_` protects `pending_notifications_`

The `check_tasks()` method uses a three-phase pattern to avoid holding `mutex_` during inference (which can take minutes):

1. **Collect phase** (under `mutex_`): snapshot due tasks into a local vector
2. **Fire phase** (no locks): call `fire_task()` for each due task, which runs inference and pushes notifications
3. **Update phase** (under `mutex_`): update `last_run`, recompute `next_run`, remove one-shot tasks, save to disk

`fire_task()` acquires `notif_mutex_` internally. The `inference_fn_` and `notify_fn_` callbacks are copied under `mutex_` then invoked without locks.

### Cron Parser

The `compute_next_cron()` method implements a simple 5-field cron scanner:

- Fields: `minute(0-59) hour(0-23) day(1-31) month(1-12) weekday(0-6, 0=Sunday)`
- Supports `*` (any) and single integer values (no ranges, lists, or step values)
- Searches forward from the given time, jumping by field increments (month -> day -> hour -> minute)
- Maximum search window: ~2 years (366 * 24 * 60 iterations)
- Returns 0 on parse error or no match within the search window

---

## Desktop Mode

Desktop mode launches a fullscreen Wayland kiosk browser pointed at the local HTTP server, turning the machine into an AI-powered desktop appliance.

### Kernel Configuration

`linux.config` includes these desktop-specific sections:

```
# DRM/KMS drivers (all built-in)
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_FBDEV_EMULATION=y
CONFIG_DRM_VIRTIO_GPU=y       # QEMU virtio-gpu
CONFIG_DRM_VMWGFX=y           # VMware
CONFIG_DRM_VBOXVIDEO=y        # VirtualBox
CONFIG_DRM_BOCHS=y            # QEMU bochs VGA
CONFIG_DRM_SIMPLEDRM=y        # EFI framebuffer fallback (any hardware)
CONFIG_DRM_I915=y             # Intel integrated graphics

# Input subsystem
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_MOUSEDEV=y

# IPC primitives required by Wayland/Mesa
CONFIG_FUTEX=y
CONFIG_MULTIUSER=y
CONFIG_SYSVIPC=y

# Framebuffer fallback console
CONFIG_FB=y
CONFIG_FB_VESA=y
CONFIG_FB_EFI=y
CONFIG_FRAMEBUFFER_CONSOLE=y
```

### Buildroot Packages

The defconfig enables these packages for desktop mode:

| Package | Purpose |
|---------|---------|
| `BR2_PACKAGE_CAGE` | Single-window Wayland compositor (kiosk mode) |
| `BR2_PACKAGE_WAYLAND` | Wayland protocol libraries |
| `BR2_PACKAGE_WAYLAND_PROTOCOLS` | Standard Wayland protocol extensions |
| `BR2_PACKAGE_MESA3D` | OpenGL implementation (software + virgl GPU drivers) |
| `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_SWRAST` | Software rendering (works everywhere) |
| `BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL` | QEMU virtio-gpu 3D acceleration |
| `BR2_PACKAGE_MESA3D_OPENGL_EGL` | EGL interface for Wayland |
| `BR2_PACKAGE_MESA3D_OPENGL_ES` | OpenGL ES for embedded browsers |
| `BR2_PACKAGE_LIBDRM` | Direct Rendering Manager library |
| `BR2_PACKAGE_LIBINPUT` | Input device handling |
| `BR2_PACKAGE_EUDEV` | Device manager (udev fork, needed by libinput) |
| `BR2_PACKAGE_LIBXKBCOMMON` | Keyboard keymap handling |
| `BR2_PACKAGE_PIXMAN` | Pixel manipulation library |
| `BR2_PACKAGE_COG` | WPE WebKit kiosk browser (primary) |
| `BR2_PACKAGE_WPEWEBKIT` | WPE WebKit rendering engine |

The `sys-a` partition is sized at 256 MB to accommodate these additional packages.

### Compositor Launch Sequence

In `child_main.cpp`, when `g_boot_mode == "desktop"`:

1. Create XDG runtime directory: `mkdir("/run/user/0")`, `setenv("XDG_RUNTIME_DIR", "/run/user/0")`
2. Set `WLR_LIBINPUT_NO_DEVICES=1` to allow cage to start without physical input devices (QEMU/VM)
3. Spawn a detached thread that:
   a. **Polls for HTTP readiness**: attempts TCP `connect()` to `127.0.0.1:80` every 200ms, up to 15 seconds
   b. **Probes for browser binary**: checks `/usr/bin/cog`, then `/usr/bin/midori`, then `/usr/bin/chromium` via `access(path, X_OK)`
   c. **Forks**: the child process `execlp()` the compositor:
      - Primary: `cage -s -- cog http://localhost`
      - Fallback 1: `cage -s -- midori -e Fullscreen -a http://localhost`
      - Fallback 2: `cage -s -- chromium --no-sandbox --kiosk http://localhost`
      - Last resort: `weston --shell=kiosk --continue-without-input`
   d. **Tracks PID**: `g_cage_pid` is an `std::atomic<pid_t>` set when the compositor starts and cleared when it exits

The compositor thread calls `waitpid()` to monitor the compositor process and logs exit status.

---

## Agent Loop

The agent loop (`agent_turn()` in `agent.cpp`) processes user messages through the LLM with tool support:

1. Build an inference request via `build_inference_request()` containing conversation history and tool definitions
2. Call the inference function (stub in Phase 1, llama.cpp in production)
3. Parse the response with `parse_tool_calls()` and `parse_content()`
4. If the LLM response contains `tool_calls`:
   a. Add the assistant's tool-calling message to the conversation
   b. Dispatch each tool call via `ToolRegistry::dispatch()`
   c. Truncate tool results to 4000 characters
   d. Add tool results to the conversation
   e. Re-infer (go back to step 2)
5. If the LLM returns text content, add it to conversation history and return it
6. Limit to `max_rounds` (default 5) to prevent infinite tool loops

### Inference Function Interface

The inference function has the signature:

```cpp
std::function<std::string(const std::string& request_json)>
```

It receives an OpenAI-compatible `/v1/chat/completions` request body and returns the full response body. In Phase 1, `stub_inference()` returns canned responses based on keyword matching against the user message.

### System Prompt

The system prompt is built dynamically by `prompt_builder.cpp` and includes:

- Identity: "You are Llamaste, an AI that IS the operating system"
- Boot mode: server, desktop, or live
- Hardware context: CPU model, core count, RAM, GPU, SIMD capabilities (AVX2/AVX-512)
- Available tools: grouped by category prefix (e.g., `fs.*`, `system.*`, `schedule.*`)
- Rules: ask confirmation for destructive ops, restrict paths to `/data/`, be concise, do not fabricate

---

## Web UI Development

### Embedded Assets

In production, web files are compiled into the binary as C byte arrays via `embed_web.cmake`. The CMake script:

1. Reads each web file as hex bytes
2. Generates `web_embed.cpp` with `const unsigned char WEB_*[]` arrays
3. Generates `web_embed.h` with extern declarations
4. Appends a null terminator so arrays can double as C strings

The `WEB_*_LEN` constants report the original file size (excluding the null terminator).

Embedded files (8 total):

| C Identifier | Source File |
|-------------|-------------|
| `WEB_INDEX_HTML` | `index.html` |
| `WEB_CHAT_JS` | `chat.js` |
| `WEB_DASHBOARD_JS` | `dashboard.js` |
| `WEB_FILES_JS` | `files.js` |
| `WEB_SYSTEM_JS` | `system.js` |
| `WEB_NOTIFICATIONS_JS` | `notifications.js` |
| `WEB_INSTALL_JS` | `install.js` |
| `WEB_STYLE_CSS` | `style.css` |

### Development Mode

When `LLAMASTE_HAS_EMBED` is not defined, files are read from disk at runtime. The server searches:
- `src/llamaste/web/<filename>`
- `../src/llamaste/web/<filename>`
- `web/<filename>`

### Tab Architecture

The web UI uses a bottom tab bar with 4 tabs: Chat, Files, Dashboard, System. Each tab's JavaScript is isolated in an IIFE (Immediately Invoked Function Expression) to prevent global namespace pollution:

```javascript
(function() {
    var state = {};
    // ... tab implementation ...
    window.TabName = { init: function() { ... } };
})();
```

Key conventions:
- Use `var` declarations, not `const`/`let` (ES5 compatibility for embedded kiosk browsers)
- Each tab module exposes an `init()` function on `window`
- The status bar is driven by `dashboard.js` via periodic polling of `/llamaste/system`

### SSE Notification Connection

`notifications.js` establishes a persistent SSE connection to `/llamaste/notifications`:

```javascript
var source = new EventSource('/llamaste/notifications');
source.onmessage = function(e) {
    var data = JSON.parse(e.data);
    // Show toast notification, update badge counter
};
```

The connection auto-reconnects on failure (SSE specification behavior). Heartbeat comments (`: heartbeat`) every 5 seconds keep the connection alive through proxies and load balancers.

### Adding a New Web Asset

1. Create the file in `src/llamaste/web/` (e.g., `settings.js`)
2. Add it to `embed_web.cmake`'s `WEB_FILES` list:
   ```cmake
   set(WEB_FILES
     # ...existing files...
     "settings.js:WEB_SETTINGS_JS"
   )
   ```
3. Add the file to CMakeLists.txt DEPENDS:
   ```cmake
   DEPENDS ${WEB_DIR}/index.html ${WEB_DIR}/chat.js
           ${WEB_DIR}/dashboard.js ${WEB_DIR}/files.js
           ${WEB_DIR}/system.js ${WEB_DIR}/notifications.js
           ${WEB_DIR}/install.js ${WEB_DIR}/settings.js
           ${WEB_DIR}/style.css
   ```
4. Add a route in `child_main.cpp`:
   ```cpp
   svr.Get("/settings.js", [](const httplib::Request& req, httplib::Response& res) {
   #ifdef LLAMASTE_HAS_EMBED
       extern const unsigned char WEB_SETTINGS_JS[];
       extern const unsigned int WEB_SETTINGS_JS_LEN;
       serve_static_file(req, res, "settings.js", WEB_SETTINGS_JS, WEB_SETTINGS_JS_LEN);
   #else
       serve_static_file(req, res, "settings.js", nullptr, 0);
   #endif
   });
   ```
5. Include it in `index.html`:
   ```html
   <script src="/settings.js"></script>
   ```

---

## Boot Sequence

```
GRUB (BIOS or UEFI)
  |
  v
bzImage (Linux kernel 6.6.70, ~5 MB)
  |  -- mounts devtmpfs, detects hardware, runs init=
  v
/opt/llamaste/llamaste (PID 1)
  |
  |-- init_mount_filesystems()    # mount proc, sys, dev, tmp, run, devpts
  |-- init_parse_boot_mode()      # read /proc/cmdline: server|desktop|live
  |-- init_mount_data()           # mount DATA partition (or tmpfs in live mode)
  |-- init_create_data_dirs()     # create /data/models, /data/llamaste/*, etc.
  |-- detect_hardware()           # read /proc/cpuinfo, /proc/meminfo
  |-- init_tune_performance()     # CPU governor=performance, THP=madvise, swappiness=1
  |-- init_set_hostname()         # set kernel hostname to "llamaste"
  |-- select_model()              # pick best GGUF model for available RAM
  |
  v
supervisor_run()  (PID 1 stays here, watchdog loop)
  |
  |-- fork()
  |     |
  |     v
  |   child_main()
  |     |-- register_all_tools()       # 29 tools across 7 categories
  |     |-- register_install_tools()   # +3 tools if live mode
  |     |-- register_schedule_tools()  # +4 schedule.* tools
  |     |-- detect_hardware()          # for system prompt / dashboard
  |     |-- build_system_prompt()      # dynamic prompt with hw context
  |     |-- MdnsResponder::start()     # llamaste.local on UDP 5353
  |     |-- Scheduler::start()         # background thread for tasks/alerts
  |     |-- [desktop] compositor thread # if boot_mode == "desktop"
  |     |     |-- poll TCP:80 readiness
  |     |     |-- probe browser binary
  |     |     |-- fork/exec cage+cog
  |     |-- svr.listen("0.0.0.0", 80)  # HTTP server (blocks)
  |
  |-- watchdog loop (kick every 10s, crash recovery with backoff)
  |-- clean shutdown: scheduler.stop(), mdns.stop(), sync, unmount, poweroff
```

---

## MCP Server (Model Context Protocol)

Llamaste exposes all its tools as an MCP server, allowing Claude Desktop and other MCP clients to invoke system management capabilities directly.

### Transport

**Streamable HTTP** (MCP spec 2025-03-26) — single endpoint, no separate SSE stream:

| Method | Path | Purpose |
|--------|------|---------|
| `GET`  | `/mcp` | Discovery / health check |
| `POST` | `/mcp` | All JSON-RPC 2.0 MCP requests |
| `OPTIONS` | `/mcp` | CORS preflight |

### Supported MCP Methods

| Method | Status |
|--------|--------|
| `initialize` | ✓ Returns session ID in `Mcp-Session-Id` header |
| `ping` | ✓ Returns `{}` |
| `tools/list` | ✓ All 44 registered tools |
| `tools/call` | ✓ Dispatches to ToolRegistry |
| `resources/list` | ✓ 2 resources |
| `resources/read` | ✓ `llamaste://system/status`, `llamaste://tools/catalog` |
| `resources/templates/list` | ✓ Returns empty array |
| `prompts/list` | ✓ Returns empty array |
| `notifications/initialized` | ✓ 202 Accepted (no body) |

### Authentication

The `/mcp` endpoint uses the same `require_auth` cookie middleware as all other protected routes. For Claude Desktop:

1. Log in to Llamaste web UI: `http://llamaste.local/`
2. Open DevTools → Application → Cookies → copy `llamaste_sid` value
3. Add to Claude Desktop config with a `Cookie` header (see below)

### Claude Desktop Configuration

Edit `~/.config/Claude/claude_desktop_config.json` (Linux/Mac) or `%APPDATA%\Claude\claude_desktop_config.json` (Windows):

```json
{
  "mcpServers": {
    "llamaste": {
      "type": "http",
      "url": "http://llamaste.local/mcp",
      "headers": {
        "Cookie": "llamaste_sid=<your-session-token>"
      }
    }
  }
}
```

Replace `llamaste.local` with the device IP if mDNS isn't available. The System panel in the Llamaste web UI shows a pre-filled config snippet for your device.

### Protocol Example

```bash
# Initialize a session
curl -X POST http://llamaste.local/mcp \
  -H 'Content-Type: application/json' \
  -H 'Cookie: llamaste_sid=<token>' \
  -D - \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize",
       "params":{"protocolVersion":"2025-03-26",
                 "clientInfo":{"name":"test","version":"1.0"},
                 "capabilities":{}}}'
# Response headers include: Mcp-Session-Id: <32-hex-char-id>

# List tools (use session ID from initialize)
curl -X POST http://llamaste.local/mcp \
  -H 'Content-Type: application/json' \
  -H 'Cookie: llamaste_sid=<token>' \
  -H 'Mcp-Session-Id: <session-id>' \
  -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
# Returns: {"result":{"tools":[{"name":"fs.list_directory",...},...44 total]}}

# Call a tool
curl -X POST http://llamaste.local/mcp \
  -H 'Content-Type: application/json' \
  -H 'Cookie: llamaste_sid=<token>' \
  -H 'Mcp-Session-Id: <session-id>' \
  -d '{"jsonrpc":"2.0","id":3,"method":"tools/call",
       "params":{"name":"system.info","arguments":{}}}'
```

### Session Management

- Sessions created on `initialize`, keyed by 32-hex-char ID
- Session ID sent in `Mcp-Session-Id` response header
- Idle sessions expired after 30 minutes
- Session state is in-memory only (not persisted across restarts)

### Tool Schema Mapping

Tools are derived from the same `ToolRegistry` used by the agent loop. The OpenAI-format `parameters` field is mapped to MCP's `inputSchema`:

| OpenAI format | MCP format |
|---------------|------------|
| `function.name` | `name` |
| `function.description` | `description` |
| `function.parameters` | `inputSchema` |

---

## Configuration

### llamaste.json

Located at `/data/llamaste/config/llamaste.json`:

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

| Key | Type | Default | Description |
|-----|------|---------|-------------|
| `model` | string | `"auto"` | Model selection: `"auto"` picks by RAM, or path to .gguf |
| `listen` | string | `"0.0.0.0"` | HTTP bind address |
| `port` | int | `80` | HTTP port (8080 if not root) |
| `threads` | int | `0` | CPU threads for inference (0 = auto) |
| `context_size` | int | `2048` | LLM context window size |
| `log_level` | string | `"info"` | Logging verbosity |

### Kernel Command Line

Set in GRUB config (`grub.cfg` or `grub-live.cfg`):

| Parameter | Description |
|-----------|-------------|
| `llamaste.mode=server` | Headless server mode (default) |
| `llamaste.mode=desktop` | Desktop mode with Wayland compositor |
| `llamaste.mode=live` | Live ISO mode with installer |
| `ip=dhcp` | Kernel-level DHCP |
| `console=ttyS0` | Serial console output |
| `init=/opt/llamaste/llamaste` | Use llamaste as init |
| `rootfstype=squashfs` | Root filesystem type (installed) |
| `rootfstype=iso9660` | Root filesystem type (live ISO) |

### Model Auto-Selection

When `model` is `"auto"`, the binary selects the largest model that fits in available RAM:

| Available RAM | Model | File |
|---------------|-------|------|
| 22+ GB | Qwen2.5-32B | `qwen2.5-32b-instruct-q4_k_m.gguf` |
| 11+ GB | Qwen2.5-14B | `qwen2.5-14b-instruct-q4_k_m.gguf` |
| 6.5+ GB | Qwen2.5-7B | `qwen2.5-7b-instruct-q4_k_m.gguf` |
| 4+ GB | Qwen2.5-3B | `qwen2.5-3b-instruct-q4_k_m.gguf` |
| 2.5+ GB | Qwen2.5-1.5B | `qwen2.5-1.5b-instruct-q4_k_m.gguf` |
| 1.5+ GB | Qwen2.5-0.5B | `qwen2.5-0.5b-instruct-q4_k_m.gguf` |

Models are stored in `/data/models/`. If no model file is found, the system runs in stub mode with canned responses.

### Schedules Persistence

Scheduled tasks are stored at `/data/llamaste/schedules.json`:

```json
[
    {
        "id": "a1b2c3d4",
        "enabled": true,
        "prompt": "Check disk usage and alert if above 80%",
        "cron": "0 * * * *",
        "at": 0,
        "every_seconds": 0,
        "once": false,
        "created": 1741036800,
        "last_run": 1741040400
    }
]
```

---

## Build System

### Buildroot BR2_EXTERNAL

Llamaste uses Buildroot's external tree mechanism (`BR2_EXTERNAL`). The external tree adds:
- A custom `llamaste` package (cmake-package)
- Board-specific files (kernel config, GRUB config, disk layout, build scripts)
- A defconfig for x86_64

### Build Commands

```bash
# First-time setup
git clone https://github.com/buildroot/buildroot.git ~/llamaste-build/buildroot
cd ~/llamaste-build/buildroot
mkdir -p ../output
make O=../output BR2_EXTERNAL=/path/to/llamaste/br2-external llamaste_x86_64_defconfig

# Build (first build: 30-60 minutes, rebuilds: 1-3 minutes)
cd ~/llamaste-build/output
make -j$(nproc)

# Rebuild just the llamaste package (after source changes)
cd ~/llamaste-build/output
make llamaste-dirclean && make llamaste && make

# Build ISO
bash /path/to/llamaste/scripts/build-iso.sh ~/llamaste-build

# WSL2 build from Windows
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c \
  "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && \
   export FORCE_UNSAFE_CONFIGURE=1 && \
   cd /root/llamaste-build/output && make"
```

**Important:** Buildroot MUST build on a native Linux ext4 filesystem, NOT NTFS. The Buildroot output directory must be on a Linux filesystem inside WSL2.

### Package Recipe

`llamaste.mk` uses `cmake-package`:
- `LLAMASTE_SITE_METHOD = local` -- sources are on the local filesystem
- `LLAMASTE_CONF_OPTS` -- Release build, static linking, embedded web assets
- Custom `INSTALL_TARGET_CMDS` -- installs to `/opt/llamaste/llamaste`

### Key Build Flags

| Flag | Purpose |
|------|---------|
| `LLAMASTE_STATIC=ON` | Build fully static binary (`-static`) |
| `LLAMASTE_EMBED_WEB=ON` | Embed web assets into binary (byte arrays) |
| `CMAKE_BUILD_TYPE=Release` | Optimized build (`-O2 -DNDEBUG`) |
| `HAVE_LZMA` | Defined when liblzma is found (enables XZ decompression in installer) |
| `LLAMASTE_HAS_EMBED` | Defined when web embedding is active (switches static file serving to embedded arrays) |

### Kernel Configuration

`linux.config` is a kernel configuration fragment (~190 lines, not a full `.config`). Buildroot merges it with kernel defaults. Key sections:

| Section | Notable Entries |
|---------|----------------|
| Core | 64-bit, SMP, voluntary preemption, 250 Hz, no modules |
| Boot | EFI stub, ACPI, PCI with MSI |
| Memory | Transparent hugepages (madvise mode), HugeTLBFS |
| Filesystems | ext4, squashfs (zstd), ISO 9660, VFAT |
| Storage | AHCI SATA, NVMe, USB storage, SCSI, ATA PIIX (QEMU IDE) |
| Virtio | PCI, block, net, console |
| Networking | IPv4, DHCP, multicast, Intel/Realtek NIC drivers |
| Graphics | DRM (virtio, vbox, bochs, simpledrm, vmwgfx, i915), evdev, framebuffer |
| IPC | futex, sysvipc, multiuser (required by Wayland/Mesa) |
| Monitoring | Thermal zones, hwmon, coretemp, watchdog |
| Power | CPU freq governors (performance, powersave, ondemand), ACPI battery/AC |
| Disabled | Sound, wireless, Bluetooth, security framework, audit, debug |

### Buildroot Defconfig

`llamaste_x86_64_defconfig` (~75 lines) configures:

- x86_64 architecture, musl toolchain with C++ support
- No init system (`BR2_INIT_NONE`), no shell (`BR2_SYSTEM_BIN_SH_NONE`)
- Linux 6.6.70 with custom config fragment
- GRUB2 (x86_64 EFI + i386 PC BIOS)
- squashfs root with zstd compression
- No BusyBox
- ccache for faster rebuilds
- Desktop packages: Cage, Wayland, Mesa (swrast + virgl), Cog + WPEWebKit, libdrm, libinput, eudev, libxkbcommon, pixman

### Disk Image Assembly

`post_image.sh` assembles the final disk image:
1. Creates data partition overlay with default config
2. Builds GRUB BIOS boot image
3. Creates ESP (FAT, auto-selected type) with kernel and GRUB config
4. Creates DATA partition (ext4) with overlay
5. Runs `genimage` with `genimage.cfg` to produce the 5-partition GPT image

**ESP gotcha:** Do NOT force FAT32 on volumes < 512 MB. FAT32 requires >= 65525 clusters, which is impossible on a 32 MB volume. Let `mkdosfs` auto-select (FAT16 for 32 MB per UEFI specification).

---

## Disk Layout

The 5-partition GPT layout is defined in `genimage.cfg`:

| # | Name | Type | Size | Filesystem | Purpose |
|---|------|------|------|------------|---------|
| 1 | bios-boot | BIOS boot | 1 MB | Raw | GRUB legacy BIOS boot image |
| 2 | esp | EFI System | 32 MB | FAT (auto) | UEFI boot: kernel, GRUB EFI binary, grub.cfg |
| 3 | sys-a | Linux | 256 MB | squashfs | Active root filesystem (read-only, immutable) |
| 4 | sys-b | Linux | 256 MB | (empty) | Standby root for A/B updates (future) |
| 5 | data | Linux | Remainder | ext4 | Persistent data, models, config |

All partitions are aligned to 1 MiB boundaries (`align = 1M`) for EFI compatibility and SSD/4K-sector performance.

The root filesystem is immutable (squashfs). All persistent state lives on the DATA partition mounted at `/data/`.

---

## Testing

### Host Tests

The binary compiles on the development host (Linux, macOS, or WSL2) for rapid iteration:

```bash
cd src/llamaste/build-host
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./llamaste  # Runs HTTP server on localhost:8080
```

On the host, PID 1 detection is false, so `child_main()` runs directly without forking. Linux-specific system calls (`mount`, `statvfs`, `fork` for compositor) are guarded with `#ifndef _WIN32`.

**Host test suites (93 tests across 6 suites):**

| Suite | Tests | What It Covers |
|-------|-------|----------------|
| hwdetect | 3 | CPU detection, RAM reading, GPU detection |
| tools | 10 | Tool registration, dispatch, unknown tool handling, path validation |
| agent | 19 | Conversation state, tool call parsing, multi-round agent loop, truncation |
| integration | 27 | Cross-module interactions, tool-agent integration, prompt building |
| http | 15 | HTTP endpoint responses, SSE streaming, CORS headers, error handling |
| mdns | 17+ | DNS packet parsing, response formatting, local IP detection |

### QEMU E2E Tests

`scripts/qemu-boot-test.sh` boots the image in QEMU and runs HTTP endpoint validation:

```bash
./scripts/qemu-boot-test.sh ~/llamaste-build/output/images/llamaste.img
```

Tests (5 total):
1. Health endpoint returns `status: ok`
2. System info returns valid CPU/RAM data
3. Chat endpoint responds with text content
4. Tools endpoint returns a non-empty tools array
5. Dashboard data has numeric CPU/RAM values

The script boots QEMU with serial console, port-forwards host 8080 to guest 80, waits for the HTTP server to be ready, runs `curl` tests, and reports pass/fail.

### EFI Boot Test

```bash
./scripts/test-efi-boot.sh
```

Boots the disk image with QEMU + OVMF UEFI firmware. Validates the EFI boot path: GRUB EFI binary loads from ESP, kernel boots, HTTP server starts.

### Install Flow Test

```bash
./scripts/qemu-install-test.sh
```

Full end-to-end install test:
1. Boot from ISO in QEMU
2. Wait for HTTP server on live ISO
3. Trigger installation to a blank disk via API
4. Wait for installation to complete
5. Reboot from installed disk
6. Verify HTTP server starts on the installed system

---

## Build Artifacts

| Artifact | Size | Details |
|----------|------|---------|
| `llamaste` binary | ~6 MB | Static ELF, x86-64, musl, stripped |
| `bzImage` kernel | ~5 MB | All drivers built-in, no modules |
| `rootfs.squashfs` | ~6 MB (will grow with desktop packages) | llamaste + libc + web UI |
| `llamaste.img` | ~360 MB | 5-partition GPT disk image |
| `llamaste.iso` | ~400 MB | Hybrid BIOS+UEFI live ISO with installer |
| Boot time | ~2 seconds | From GRUB to HTTP server ready |

---

## Extending the System

### Adding Buildroot Packages

Edit the defconfig to include packages:

```bash
cd ~/llamaste-build/output
make menuconfig  # Select packages
make savedefconfig BR2_DEFCONFIG=/path/to/llamaste/br2-external/configs/llamaste_x86_64_defconfig
```

### Modifying Kernel Config

Edit `br2-external/board/llamaste/linux.config` and rebuild:

```bash
cd ~/llamaste-build/output
make linux-rebuild
make
```

### Custom Models

Place any GGUF model file in `/data/models/` on the running system. Set `"model": "/data/models/your-model.gguf"` in `llamaste.json`, or use `"auto"` for RAM-based selection.

### VirtualBox VM

A pre-configured VM exists for testing:

- **VM Name:** "Llamaste", Location: `vm/Llamaste/`
- 4 GB RAM, 2 CPUs, EFI64, VMSVGA, NAT (host 8080 -> guest 80)
- 16 GB VDI with installed system
- Start: `"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm Llamaste --type gui`
- Web UI: `http://localhost:8080`

---

## Code Conventions and Gotchas

| Topic | Rule |
|-------|------|
| Raw string literals | Use `R"json(...)json"` delimiter. Bare `R"(...)"` breaks when JSON contains `)"`. |
| Tool registration | Use designated initializers (`.name = ..., .handler = ...`) to match existing pattern. |
| Declaration order | In `child_main.cpp`, globals must appear before functions that reference them. |
| Linux-only code | Guard with `#ifndef _WIN32`. Include `<sys/mount.h>` for `mount()`/`umount()`. |
| New web assets | Add to `embed_web.cmake` WEB_FILES + `CMakeLists.txt` DEPENDS + route in `child_main.cpp` + `<script>` in `index.html`. |
| ESP filesystem | Never force FAT32 on volumes < 512 MB. Let mkdosfs auto-select (FAT16 for 32 MB). |
| Partition alignment | Always use `align = 1M` in `genimage.cfg` for EFI compatibility. |
| PMBR after resize | After resizing GPT partitions, must update Protective MBR size (offset 458) and CHS end (offsets 451-453). |
| JSON parsing | Always use `json::parse(str, nullptr, false)` to get a discarded value on error instead of an exception. |
| Thread safety | Never hold `mutex_` during inference calls. Use the collect-fire-update three-phase pattern. |
| CRLF | `.gitattributes` enforces LF for Linux scripts and config files. |
| Buildroot filesystem | Buildroot MUST build on native Linux ext4, NOT NTFS/Windows filesystems. |

---

## Version History

### 1.0.000a (Alpha) -- Current

**Phase 1: Bootable Image (12 tasks, complete)**
- PID 1 supervisor/child architecture with crash recovery
- 25 base tools across 6 categories (fs, process, network, system, config, model)
- Agent loop with multi-round tool calling
- Web UI with chat, dashboard
- HTTP API (chat SSE, OpenAI-compatible, health, tools, conversations)
- mDNS responder (llamaste.local)
- Hardware detection and model auto-selection
- 5-partition GPT disk image with squashfs root
- Hybrid BIOS+UEFI live ISO with C++ installer (no shell)
- EFI boot with FAT16 ESP, 1 MiB alignment, PMBR updates
- 93 host tests, 5 QEMU E2E tests

**Phase 2a: Web UI Redesign (Tasks 1-9, complete)**
- Status bar: clock, model name, inference speed, RAM usage, IP address, notification badge
- 4-tab navigation: Chat, Files, Dashboard, System
- File browser with directory navigation and file preview
- Dashboard with CPU/temp/RAM/disk gauges, hardware info, network info
- System tab with system details and scheduled tasks display
- Dark theme redesign

**Phase 2b: Scheduler (Tasks 10-15, complete)**
- Scheduler background thread with 10-second check interval
- 4 schedule tools: create, list, delete, update
- Task types: cron, interval (every_seconds), one-shot (at timestamp)
- Alert monitoring: RAM/disk/temp thresholds with cooldown
- SSE notification endpoint with heartbeat keepalive
- Toast notification UI with badge counter
- Persistence to /data/llamaste/schedules.json
- Thread-safe three-phase task execution

**Phase 2c: Desktop Mode (Tasks 16-18, source complete, untested)**
- Kernel DRM drivers: virtio, vbox, bochs, simpledrm, vmwgfx, i915
- Kernel input: evdev, mousedev
- Kernel IPC: futex, sysvipc (required by Wayland/Mesa)
- Buildroot packages: Cage, Cog/WPEWebKit, Wayland, Mesa, libdrm, libinput, eudev
- Compositor launch: TCP poll, browser probe, fork/exec with fallback chain
- sys-a partition increased to 256 MB

**Pending:**
- Task 19: WSL2 Buildroot build with desktop packages, QEMU desktop mode testing
- Task 20: Real llama.cpp inference integration (replace stub_inference)
