# Llamaste Developer Guide

This document covers the architecture, source code, APIs, and workflows for developing and extending Llamaste.

## Architecture Overview

Llamaste runs as three layers:

```
[ Linux kernel ]  — hardware drivers, memory management, networking
       |
[ llamaste binary ]  — PID 1, HTTP server, agent loop, tools, mDNS
       |
[ Web UI ]  — vanilla JS + SSE, served by the binary
```

The `llamaste` binary is a single static C++ executable that runs as PID 1 (Linux's init process). There is no init system, no shell, and no BusyBox. The binary handles everything above the kernel: mounting filesystems, detecting hardware, serving the web UI, running the agent loop, and dispatching tool calls.

### PID 1 Supervisor/Child Pattern

When running as PID 1, the binary forks into two processes:

- **Supervisor (PID 1)**: Monitors the child, kicks the hardware watchdog, handles crash recovery with backoff (2s normally, 30s after 3 rapid crashes), and manages clean shutdown (sync, unmount, power off).
- **Child**: Runs the HTTP server, agent loop, mDNS responder, and all user-facing functionality. If the child crashes, the supervisor respawns it.

When not running as PID 1 (e.g., during development on a host machine), `child_main()` is called directly without forking.

## Source Code Layout

### `src/llamaste/`

| File | Purpose |
|------|---------|
| `main.cpp` | Entry point. Mounts filesystems, detects hardware, selects model, starts supervisor or child. |
| `supervisor.cpp` | PID 1 supervisor loop: fork, watchdog, crash recovery, shutdown. |
| `supervisor.h` | `SupervisorConfig` struct and `supervisor_run()` declaration. |
| `init.cpp` | Boot-time setup: mount filesystems, mount DATA partition, tune performance, set hostname, parse boot mode. |
| `init.h` | Init function declarations. |
| `child_main.cpp` | HTTP server with all routes: static files, agent chat (SSE), dashboard, tools, conversations, OpenAI API, health, installer. |
| `hwdetect.cpp` | Hardware detection: CPU model/cores, RAM, GPU, AVX2/AVX-512 from `/proc/cpuinfo` and `/proc/meminfo`. |
| `hwdetect.h` | `HardwareInfo` struct and detection functions. |
| `agent.cpp` | Agent loop: builds inference requests, parses tool calls, dispatches tools, multi-turn conversation. |
| `agent.h` | `ConversationState`, `ToolCall`, `Message` structs and `agent_turn()` function. |
| `prompt_builder.cpp` | Builds the system prompt dynamically from hardware info, available tools, and boot mode. |
| `prompt_builder.h` | `build_system_prompt()` declaration. |
| `tools.cpp` | `ToolRegistry` implementation: register, dispatch, generate OpenAI-compatible tool definitions. |
| `tools.h` | `ToolDef`, `ToolRegistry` class, and registration function declarations. |
| `tools_fs.cpp` | Filesystem tools: `fs.list_directory`, `fs.read_file`, `fs.write_file`, `fs.delete_file`, `fs.stat`, `fs.search_files`, `fs.disk_usage`. All restricted to `/data/`. |
| `tools_process.cpp` | Process tools: `process.list`, `process.info`, `process.kill`. |
| `tools_network.cpp` | Network tools: `network.interfaces`, `network.connections`, `network.ping`, `network.dns_lookup`, `network.http_get`. |
| `tools_system.cpp` | System tools: `system.info`, `system.uptime`, `system.shutdown`, `system.reboot`, `system.logs`. |
| `tools_config.cpp` | Config tools: `config.get`, `config.set`, `config.list`, `config.reset`. Manages `/data/llamaste/config/llamaste.json`. |
| `tools_model.cpp` | Model tools: `model.info`, `model.list`, `model.select`, `model.download_status`. |
| `tools_install.cpp` | Installer tools (live mode only): `install.detect_disks`, `install.to_disk`, `install.progress`. |
| `net_mdns.cpp` | Multicast DNS responder for `llamaste.local` discovery. |
| `net_mdns.h` | `MdnsResponder` class. |
| `embed_web.cmake` | CMake script that converts web assets to C byte arrays for compile-time embedding. |
| `httplib.h` | cpp-httplib single-header HTTP library (vendored). |
| `json.hpp` | nlohmann/json single-header JSON library (vendored). |

### `src/llamaste/web/`

| File | Purpose |
|------|---------|
| `index.html` | Main page: sidebar with dashboard, chat area, navigation. |
| `chat.js` | Chat interface: message rendering, SSE streaming, conversation management. |
| `dashboard.js` | System dashboard: polls `/llamaste/system` and updates CPU, RAM, disk, temperature gauges. |
| `install.js` | Installer UI (live mode only): disk detection, installation progress, success/error states. |
| `style.css` | All styling: layout, chat bubbles, dashboard gauges, installer components. |

### `br2-external/`

Buildroot external tree:

```
br2-external/
  external.desc          # Declares this as a BR2_EXTERNAL tree ("LLAMASTE")
  external.mk            # Includes package makefiles
  Config.in              # Kconfig menu entries
  board/llamaste/
    linux.config          # Kernel configuration fragment
    grub.cfg              # GRUB config for installed system (server + desktop entries)
    grub-live.cfg         # GRUB config for ISO live boot
    genimage.cfg          # Disk image layout (5-partition GPT)
    post_build.sh         # Runs after target build: creates directories, installs grub.cfg
    post_image.sh         # Runs after image gen: builds ESP, data partition, assembles disk image
  configs/
    llamaste_x86_64_defconfig  # Buildroot defconfig
  package/llamaste/
    Config.in             # Package Kconfig entry
    llamaste.mk           # Package build recipe (cmake-package)
```

### `scripts/`

| File | Purpose |
|------|---------|
| `build-iso.sh` | Builds hybrid BIOS+UEFI ISO from Buildroot output using grub-mkrescue. |
| `qemu-boot-test.sh` | QEMU E2E test suite: boots the image and validates HTTP endpoints. |

## HTTP API Reference

All endpoints are served by the HTTP server in `child_main.cpp`.

### `GET /health`

Health check endpoint.

**Response:**
```json
{
    "status": "ok",
    "uptime_seconds": 120,
    "model_loaded": false,
    "tools_count": 25,
    "mode": "server"
}
```

### `GET /llamaste/system`

System dashboard data. Includes a ~100ms delay for CPU measurement.

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
    "disk_used_gb": 0.5
}
```

### `GET /llamaste/tools`

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

### `POST /llamaste/chat`

Agent chat endpoint with SSE streaming.

**Request:**
```json
{
    "message": "What files are in /data?",
    "conversation_id": "conv_123",
    "stream": true
}
```

If `conversation_id` is omitted, a new one is generated. If `stream` is `true` (default), the response is an SSE stream.

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

### `POST /v1/chat/completions`

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

### `GET /llamaste/conversations`

List all conversations.

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

### Installer Endpoints (Live Mode Only)

These endpoints are only registered when `llamaste.mode=live`:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/install/disks` | GET | List available target disks |
| `/install/start` | POST | Begin installation (body: `{"device":"/dev/sda"}`) |
| `/install/progress` | GET | Poll installation progress |
| `/install/progress/stream` | GET | SSE stream of progress updates |
| `/install.js` | GET | Installer JavaScript |

### Static File Routes

| Path | File |
|------|------|
| `/` | `index.html` |
| `/style.css` | `style.css` |
| `/chat.js` | `chat.js` |
| `/dashboard.js` | `dashboard.js` |

In production builds (`LLAMASTE_HAS_EMBED` defined), these are served from byte arrays compiled into the binary. In development, they're read from the filesystem.

## Tools System

Tools are the LLM's interface to the operating system. Each tool has a name, description, JSON Schema parameters, and a handler function.

### Tool Definition

```cpp
ToolDef tool;
tool.name = "custom.hello";
tool.description = "Say hello to a user";
tool.parameters = R"({
    "type": "object",
    "properties": {
        "name": {
            "type": "string",
            "description": "Name of the person to greet"
        }
    },
    "required": ["name"]
})";
tool.handler = [](const std::string& args_json) -> std::string {
    auto args = json::parse(args_json);
    std::string name = args.value("name", "world");
    json result;
    result["greeting"] = "Hello, " + name + "!";
    return result.dump();
};
tool.requires_confirmation = false;
```

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
        "custom.hello",
        "Say hello to a user",
        R"({"type":"object","properties":{"name":{"type":"string","description":"Name to greet"}},"required":["name"]})",
        handle_hello
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

### Tool Categories

| Category | Prefix | Tools | File |
|----------|--------|-------|------|
| Filesystem | `fs.*` | 7 tools | `tools_fs.cpp` |
| Process | `process.*` | 3 tools | `tools_process.cpp` |
| Network | `network.*` | 5 tools | `tools_network.cpp` |
| System | `system.*` | 5 tools | `tools_system.cpp` |
| Config | `config.*` | 4 tools | `tools_config.cpp` |
| Model | `model.*` | 4 tools | `tools_model.cpp` |
| Install | `install.*` | 3 tools (live mode only) | `tools_install.cpp` |

### Tool Security

- All `fs.*` tools validate paths against `/data/` -- path traversal (`../`) is rejected
- Destructive tools (`fs.delete_file`, `system.shutdown`, `system.reboot`) have `requires_confirmation = true`
- The agent's system prompt instructs the LLM to ask the user before calling confirmed tools
- Tool handlers catch all exceptions and return `{"error": "..."}` JSON

## Agent Loop

The agent loop (`agent_turn()` in `agent.cpp`) processes user messages through the LLM with tool support:

1. Build an inference request containing the conversation history and tool definitions
2. Call the inference function (stub in Phase 1, llama.cpp in production)
3. If the LLM response contains `tool_calls`:
   a. Dispatch each tool call via `ToolRegistry::dispatch()`
   b. Add tool results to the conversation
   c. Re-infer (go back to step 2)
4. If the LLM returns text content, return it as the final response
5. Limit to `max_rounds` (default 5) to prevent infinite tool loops

### System Prompt

The system prompt is built dynamically by `prompt_builder.cpp` and includes:
- Identity: "You are Llamaste, an AI that IS the operating system"
- Boot mode: server or desktop
- Hardware context: CPU model, core count, RAM, GPU, SIMD capabilities
- Available tools: grouped by category prefix
- Rules: confirmation for destructive ops, path restriction to `/data/`, conciseness, no fabrication

## Web UI Development

### Embedded Assets

In production, web files are compiled into the binary as C byte arrays via `embed_web.cmake`. The CMake script:
1. Reads each web file as hex bytes
2. Generates `web_embed.cpp` with `const unsigned char WEB_*[]` arrays
3. Generates `web_embed.h` with extern declarations
4. Appends a null terminator so arrays can double as C strings

The `WEB_*_LEN` constants report the original file size (excluding the null terminator).

### Development Mode

When `LLAMASTE_HAS_EMBED` is not defined, files are read from disk at runtime. The server searches:
- `src/llamaste/web/<filename>`
- `../src/llamaste/web/<filename>`
- `web/<filename>`

### Adding a New Web Page

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
           ${WEB_DIR}/dashboard.js ${WEB_DIR}/install.js
           ${WEB_DIR}/settings.js
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

## Build System

### Buildroot BR2_EXTERNAL

Llamaste uses Buildroot's external tree mechanism (`BR2_EXTERNAL`). The external tree adds:
- A custom `llamaste` package (cmake-package)
- Board-specific files (kernel config, GRUB config, disk layout, build scripts)
- A defconfig for x86_64

### Build Commands

```bash
# First-time setup
git clone https://github.com/buildroot/buildroot.git /root/llamaste-build
cd /root/llamaste-build

# Configure
make BR2_EXTERNAL=/path/to/llamaste/br2-external llamaste_x86_64_defconfig

# Build (first build: 20-40 minutes, rebuilds: 1-3 minutes)
make -j$(nproc)

# Build ISO
/path/to/llamaste/scripts/build-iso.sh /root/llamaste-build
```

### Package Recipe

`llamaste.mk` uses `cmake-package`:
- `LLAMASTE_SITE_METHOD = local` — sources are on the local filesystem
- `LLAMASTE_CONF_OPTS` — Release build, static linking, embedded web assets
- Custom `INSTALL_TARGET_CMDS` — installs to `/opt/llamaste/llamaste`

### Key Build Flags

| Flag | Purpose |
|------|---------|
| `LLAMASTE_STATIC=ON` | Build fully static binary (`-static`) |
| `LLAMASTE_EMBED_WEB=ON` | Embed web assets into binary |
| `CMAKE_BUILD_TYPE=Release` | Optimized build |

### Kernel Configuration

`linux.config` is a kernel configuration fragment (not a full `.config`). Key choices:

- `CONFIG_MODULES=n` — No loadable modules; everything built-in
- `CONFIG_TRANSPARENT_HUGEPAGE_MADVISE=y` — Huge pages for mmap-heavy LLM inference
- `CONFIG_IP_PNP_DHCP=y` — Kernel-level DHCP (no userspace DHCP client)
- `CONFIG_ISO9660_FS=y` — ISO filesystem support for live boot
- Drivers: AHCI SATA, NVMe, USB storage, Intel/Realtek NICs, virtio (QEMU)
- Disabled: sound, graphics, wireless, Bluetooth, security framework, audit

### Disk Image Assembly

`post_image.sh` assembles the final disk image:
1. Creates data partition overlay with default config
2. Builds GRUB BIOS boot image
3. Creates ESP (FAT32) with kernel and GRUB config
4. Creates DATA partition (ext4) with overlay
5. Runs `genimage` with `genimage.cfg` to produce the 5-partition GPT image

## Testing

### Host Tests

The binary compiles on the development host (Linux, macOS, or WSL2) with g++:

```bash
cd src/llamaste/build-host
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
./llamaste  # Runs HTTP server on localhost:8080
```

On the host, PID 1 detection is false, so `child_main()` runs directly without forking.

### QEMU E2E Tests

`scripts/qemu-boot-test.sh` boots the image in QEMU and runs HTTP endpoint validation:

```bash
./scripts/qemu-boot-test.sh /root/llamaste-build/output/images/llamaste.img
```

Tests:
1. Health endpoint returns `status: ok`
2. System info returns valid CPU/RAM data
3. Chat endpoint responds with text content
4. Tools endpoint returns a non-empty tools array
5. Dashboard data has numeric CPU/RAM values

The script boots QEMU with serial console, waits for the HTTP server, runs `curl` tests, and reports pass/fail.

## Boot Sequence

```
GRUB
  |
  v
bzImage (Linux kernel, ~5 MB)
  |  — mounts devtmpfs, detects hardware
  v
/opt/llamaste/llamaste (PID 1)
  |
  |— init_mount_filesystems()    # mount proc, sys, dev, tmp, run, devpts
  |— init_parse_boot_mode()      # read /proc/cmdline for server/desktop/live
  |— init_mount_data()           # mount DATA partition (or tmpfs in live mode)
  |— init_create_data_dirs()     # create /data/models, /data/llamaste/*, etc.
  |— detect_hardware()           # read /proc/cpuinfo, /proc/meminfo
  |— init_tune_performance()     # CPU governor=performance, THP=madvise, swappiness=1
  |— init_set_hostname()         # set kernel hostname
  |— select_model()              # pick best model for available RAM
  |
  v
supervisor_run()  (PID 1 stays here)
  |
  |— fork()
  |     |
  |     v
  |   child_main()
  |     |— register_all_tools()      # 25 tools across 6 categories
  |     |— detect_hardware()         # for system prompt
  |     |— build_system_prompt()     # dynamic prompt with hw context
  |     |— MdnsResponder::start()    # llamaste.local
  |     |— svr.listen("0.0.0.0", 80) # HTTP server
  |
  |— watchdog loop (kick every 10s)
  |— crash recovery (respawn child on exit)
```

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

Models are stored in `/data/models/`. If no model file is found, the system runs in stub mode.

## Extending the System

### Adding Buildroot Packages

Edit the defconfig to include packages:

```bash
cd /root/llamaste-build
make menuconfig  # Select packages
make savedefconfig BR2_DEFCONFIG=/path/to/llamaste/br2-external/configs/llamaste_x86_64_defconfig
```

### Modifying Kernel Config

Edit `br2-external/board/llamaste/linux.config` and rebuild:

```bash
make linux-rebuild
make
```

### Custom Models

Place any GGUF model file in `/data/models/` on the running system. Set `"model": "/data/models/your-model.gguf"` in `llamaste.json`, or use `"auto"` for RAM-based selection.

### Disk Layout

The 5-partition GPT layout is defined in `genimage.cfg`:

| # | Name | Type | Size | Purpose |
|---|------|------|------|---------|
| 1 | BIOS Boot | BIOS boot | 1 MB | GRUB legacy BIOS boot |
| 2 | ESP | FAT32 | 32 MB | UEFI boot, kernel, GRUB config |
| 3 | SYS-A | squashfs | ~6 MB | Active root filesystem (read-only, immutable) |
| 4 | SYS-B | (empty) | 256 MB | Standby root for A/B updates |
| 5 | DATA | ext4 | Remainder | Persistent data, models, config |

The root filesystem is immutable. All persistent state lives on the DATA partition at `/data/`.
