# Phase 2 Desktop Mode — Design Document

**Date**: 2026-03-03
**Status**: Design
**Scope**: Desktop compositor, enhanced web UI, proactive heartbeat scheduler

---

## 1. Overview

Transform Llamaste from a headless server into a full desktop experience. The LLM remains the primary interface — no traditional desktop environment (no window manager, no icons, no start menu). Instead, the existing web UI becomes a fullscreen kiosk application with a system status bar, tab navigation, and proactive notifications.

### Design Principles
1. Chat-first — the conversation IS the desktop
2. Ambient awareness — status bar shows system state at a glance
3. Proactive heartbeat — the AI talks to you first (alerts, briefings, reminders)
4. Zero escape — kiosk mode, no way to break out to a shell (there is no shell)
5. Minimal additions — Cage + Cog, not a full desktop stack

---

## 2. Architecture

### Runtime Stack (Desktop Mode)

```
Linux kernel (with DRM/KMS, evdev, GPU drivers)
  |
  llamaste binary (PID 1)
  |-- HTTP server on :80 (existing)
  |-- Scheduler thread (NEW — heartbeat/cron)
  |-- Notification SSE stream (NEW)
  |
  +-- fork/exec: cage -- cog http://localhost
      |
      Cage (wlroots Wayland kiosk compositor)
        |
        Cog (WPE WebKit kiosk browser)
          |
          Llamaste Web UI (fullscreen, dark theme)
```

### Why Cog over Chromium
- Cog + WPE WebKit: ~80-130 MB added to image (vs ~250+ MB for Chromium)
- Purpose-built for embedded kiosk on Wayland
- Available as Buildroot package
- Sufficient JS engine for vanilla JS + SSE

### Boot Flow (Desktop Mode)
1. Kernel boots, llamaste starts as PID 1
2. init.cpp: mount filesystems, start DHCP, detect hardware (existing)
3. supervisor.cpp: fork child (existing)
4. child_main.cpp: start HTTP server on :80 (existing)
5. child_main.cpp: start scheduler thread (NEW)
6. child_main.cpp: detect `g_boot_mode == "desktop"`, fork/exec Cage (NEW)
7. Cage launches Cog pointing at http://localhost
8. Web UI renders fullscreen

### Boot Flow (Server Mode)
Steps 1-5 only. No compositor, no browser. Same as today plus scheduler.

---

## 3. Web UI Redesign

### Layout (Desktop & Server)

The UI transforms from sidebar+chat to statusbar+tabs+content:

```
+----------------------------------------------------------------------+
| LLAMASTE  12:34 PM Mar 3  | Qwen3-8B | 23 t/s | 4.1/8GB | 192.168.1.5 |
+----------------------------------------------------------------------+
|                                                                      |
|  [active tab content — fills entire area]                            |
|                                                                      |
+----------------------------------------------------------------------+
| [ Chat ]  [ Files ]  [ Dashboard ]  [ System ]                      |
+----------------------------------------------------------------------+
```

**Top status bar** (40px, always visible):
- Left: "LLAMASTE" wordmark
- Center-left: Clock (HH:MM, date)
- Center: Active model name
- Center-right: Inference speed (tokens/sec), RAM (used/total)
- Right: IP address, volume (if audio available)
- Notification badge (count of unread alerts)

**Bottom tab bar** (48px, always visible):
- Four tabs: Chat, Files, Dashboard, System
- Active tab highlighted with accent color
- Badge counts on tabs (e.g., unread notifications on Chat)

**Content area** (fills remaining space):
- Only the active tab's content is visible
- Each tab maintains its own state

### Tab: Chat (Default)

The existing chat interface, minus the sidebar dashboard. Simplified layout:

```
+----------------------------------------------------------------+
| Conversations: [+ New] [Conv 1] [Conv 2] [Conv 3]    (horizontal scroll) |
+----------------------------------------------------------------+
|                                                                |
|  [system] Welcome to Llamaste. I'm your operating system.     |
|                                                                |
|  [user] show me my files                                       |
|  [assistant] Here are your files in /data/:                    |
|    [> fs.list  done]                                           |
|    documents/  models/  config/                                |
|                                                                |
|  [notification] System alert: RAM usage at 85%                 |
|                                                                |
+----------------------------------------------------------------+
| Type a message...                                       [Send] |
+----------------------------------------------------------------+
```

Changes from current:
- Conversation list moves from sidebar to horizontal strip at top of chat
- Proactive notifications/alerts appear inline as system messages
- No sidebar at all — dashboard is its own tab

### Tab: Files

Browse and manage `/data/`:

```
+----------------------------------------------------------------+
| /data/documents/                          [Upload] [New Folder] |
+----------------------------------------------------------------+
| ..                                                    (parent)  |
| notes.txt                    1.2 KB    Mar 2 14:30   [actions] |
| report.md                    4.5 KB    Mar 1 09:15   [actions] |
| images/                      (dir)     Mar 3 08:00   [actions] |
+----------------------------------------------------------------+
| Preview: (selected file content shown here)                    |
+----------------------------------------------------------------+
```

- Uses existing `fs.list`, `fs.read` tools via `/llamaste/tools` dispatch
- File actions: view, download, rename, delete (with confirmation)
- Text file preview in bottom panel
- Upload via drag-drop or button

### Tab: Dashboard

Expanded system monitoring (currently in sidebar, now full-page):

```
+-------------------------------+-------------------------------+
| CPU Usage          45%        | Temperature        52 C       |
| [====================      ]  | [===============           ]  |
|                               |                               |
| RAM               4.1/8.0 GB | Disk             2.3/14.0 GB  |
| [=============            ]   | [====                      ]  |
+-------------------------------+-------------------------------+
| Hardware                      | Network                       |
| CPU: AMD Ryzen 5 3600         | IP: 192.168.1.105             |
| Cores: 6                      | Hostname: llamaste.local      |
| GPU: Intel UHD 630            | Mode: desktop                 |
| AVX2: Yes  AVX512: No         | Uptime: 2h 34m                |
+-------------------------------+-------------------------------+
| Model                         | Scheduler                     |
| Active: qwen3-8b-q4_k_m      | Active tasks: 3               |
| Context: 8192 tokens          | Next: morning-briefing (8:00) |
| Speed: 23 tok/s               | Last: health-check (5 min ago)|
| RAM usage: 3.2 GB             |                               |
+-------------------------------+-------------------------------+
```

### Tab: System

Settings and management:

```
+----------------------------------------------------------------+
| Model Management                                               |
| Active: qwen3-8b-q4_k_m.gguf                                  |
| Available: (scan /data/models/)                                |
| [Switch Model]                                                 |
+----------------------------------------------------------------+
| Scheduled Tasks                                                |
| [x] morning-briefing   "0 8 * * *"    System status report    |
| [x] health-check       every 1h       Alert on issues         |
| [ ] disk-watch          every 6h       Monitor disk usage      |
| [+ Add Schedule]  [Edit]  [Delete]                             |
+----------------------------------------------------------------+
| Network                                                        |
| IP: 192.168.1.105    Gateway: 192.168.1.1                      |
| DNS: 8.8.8.8         mDNS: llamaste.local                     |
+----------------------------------------------------------------+
| Logs                                                           |
| (scrollable log viewer — last 100 lines of system log)         |
+----------------------------------------------------------------+
| About                                                          |
| Llamaste v0.2.0 — LLM-OS                                      |
| Kernel: Linux 6.6.70    Mode: desktop                          |
+----------------------------------------------------------------+
```

---

## 4. Proactive Heartbeat System

### Concept

Unlike traditional chatbots that only respond, Llamaste proactively:
- Sends morning/evening briefings
- Alerts on system conditions (high RAM, disk full, overheating)
- Executes user-defined scheduled tasks
- Runs reminders

### Architecture

A dedicated `scheduler` thread runs inside the llamaste binary:

```cpp
class Scheduler {
    std::vector<ScheduledTask> tasks;    // persisted to /data/llamaste/schedules.json
    std::vector<AlertRule> alert_rules;  // built-in system monitors
    std::mutex mutex;
    bool running = true;

    void run();          // main loop, checks every 10 seconds
    void check_alerts(); // system condition monitors
    void fire_task(ScheduledTask& t);  // runs agent_turn with task prompt
};
```

### Schedule Storage

`/data/llamaste/schedules.json`:
```json
{
  "tasks": [
    {
      "id": "morning-briefing",
      "enabled": true,
      "cron": "0 8 * * *",
      "prompt": "Give a brief morning status: system health, disk usage, any issues since last check.",
      "created": "2026-03-03T10:00:00Z"
    },
    {
      "id": "reminder-abc123",
      "enabled": true,
      "at": "2026-03-03T15:00:00Z",
      "prompt": "Remind the user to check the server logs.",
      "once": true,
      "created": "2026-03-03T10:30:00Z"
    },
    {
      "id": "health-check",
      "enabled": true,
      "every_seconds": 3600,
      "prompt": "Check system health. Only report if something needs attention.",
      "created": "2026-03-03T10:00:00Z"
    }
  ],
  "alerts": {
    "ram_threshold_percent": 85,
    "disk_threshold_percent": 90,
    "temp_threshold_c": 80,
    "cooldown_seconds": 300
  }
}
```

### Schedule Types

| Type | Field | Example | Behavior |
|------|-------|---------|----------|
| Cron | `cron` | `"0 8 * * *"` | Standard cron expression, recurring |
| One-shot | `at` + `once: true` | `"2026-03-03T15:00:00Z"` | Fires once, then auto-deletes |
| Interval | `every_seconds` | `3600` | Repeats every N seconds |

### Built-in System Alerts (No Schedule Needed)

These always run, with configurable thresholds:
- RAM usage above threshold → alert
- Disk usage above threshold → alert
- CPU temperature above threshold → alert
- Network interface down → alert

Alerts have a cooldown to avoid spam (default 5 minutes).

### Tool Family: schedule.*

| Tool | Description |
|------|-------------|
| `schedule.create` | Create a new scheduled task (cron, one-shot, or interval) |
| `schedule.list` | List all scheduled tasks with next-run times |
| `schedule.delete` | Delete a scheduled task by ID |
| `schedule.update` | Enable/disable or modify a scheduled task |

Users interact via natural language:
- "Every morning at 8am, tell me how the system is doing" → `schedule.create` with cron
- "Remind me in 2 hours to check the logs" → `schedule.create` with one-shot
- "Stop the morning briefing" → `schedule.delete`
- "What's scheduled?" → `schedule.list`

### Notification Delivery

**SSE endpoint**: `GET /llamaste/notifications`
- Persistent connection, kept open
- Pushes events:
  ```
  data: {"type":"alert","title":"High RAM Usage","body":"RAM at 87% (6.9/8.0 GB)","time":"2026-03-03T14:32:00Z","id":"alert-ram-001"}\n\n
  data: {"type":"scheduled","title":"Morning Briefing","body":"System healthy. Disk at 23%. No errors overnight.","time":"2026-03-03T08:00:00Z","id":"sched-morning-001"}\n\n
  data: {"type":"reminder","title":"Reminder","body":"Check the server logs.","time":"2026-03-03T15:00:00Z","id":"remind-abc123"}\n\n
  ```

**Web UI notification display**:
- Toast popup (top-right, auto-dismiss after 8 seconds)
- Badge count on Chat tab
- Notifications also appear inline in the chat as system messages
- Notification sound (optional, configurable)

---

## 5. Backend Changes

### New C++ Files

| File | Purpose |
|------|---------|
| `scheduler.h/cpp` | Scheduler thread, cron parser, task persistence, alert monitors |
| `tools_schedule.cpp` | schedule.create, schedule.list, schedule.delete, schedule.update |

### Modified C++ Files

| File | Changes |
|------|---------|
| `child_main.cpp` | New SSE endpoint `/llamaste/notifications`. Expand `/llamaste/system` with `cpu_model`, `gpu_name`, `has_avx2`, `has_avx512`, `tokens_per_sec`, `scheduled_tasks_count`. Desktop mode: fork/exec Cage after HTTP server starts. |
| `init.cpp` | Desktop mode: set `XDG_RUNTIME_DIR`, ensure `/tmp` is writable for Wayland socket |
| `hwdetect.cpp` | No changes needed — already detects everything |
| `agent.cpp` | Add inference speed tracking (tokens_per_sec calculation) |
| `prompt_builder.cpp` | Add schedule tools to system prompt |
| `tools.cpp` | Register schedule.* tools |

### New Web Files

| File | Purpose |
|------|---------|
| `files.js` | Files tab — browser for /data/ |
| `system.js` | System tab — settings, schedules, logs, about |
| `notifications.js` | Notification SSE connection, toast display, badge counts |

### Modified Web Files

| File | Changes |
|------|---------|
| `index.html` | New layout: status bar + tab bar + content panels. Load new JS files. |
| `style.css` | Status bar styles, tab bar styles, tab content panels, toast notifications, files browser, dashboard grid |
| `chat.js` | Remove sidebar references, adapt to tab panel container, show inline notifications |
| `dashboard.js` | Split: status-bar updater (always runs) + expanded dashboard tab content |

### New API Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/llamaste/notifications` | SSE stream for proactive notifications |
| GET | `/llamaste/schedules` | List scheduled tasks (JSON) |
| POST | `/llamaste/schedules` | Create scheduled task |
| DELETE | `/llamaste/schedules/:id` | Delete scheduled task |

### Modified API Endpoints

| Method | Path | Changes |
|--------|------|---------|
| GET | `/llamaste/system` | Add: `cpu_model`, `gpu_name`, `has_avx2`, `has_avx512`, `tokens_per_sec`, `volume_percent`, `scheduled_tasks_count`, `next_scheduled_time`, `active_alerts[]` |

---

## 6. Buildroot / Kernel Changes

### Kernel Config Additions (`linux.config`)

```
# Graphics (required for Wayland)
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_FBDEV_EMULATION=y
CONFIG_DRM_VIRTIO_GPU=y       # QEMU testing
CONFIG_DRM_VMWGFX=y           # VirtualBox
CONFIG_DRM_I915=y             # Intel (common for appliance hardware)
CONFIG_DRM_AMDGPU=y           # AMD

# Input
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_MOUSEDEV=y

# Required by Wayland compositors
CONFIG_FUTEX=y
CONFIG_UNIX=y                  # Unix domain sockets
CONFIG_MULTIUSER=y
CONFIG_SYSVIPC=y
CONFIG_TMPFS=y                 # Already likely enabled
```

### Buildroot Package Additions (`llamaste_x86_64_defconfig`)

```
# Wayland compositor
BR2_PACKAGE_CAGE=y
BR2_PACKAGE_WLROOTS=y
BR2_PACKAGE_WAYLAND=y
BR2_PACKAGE_WAYLAND_PROTOCOLS=y

# Graphics
BR2_PACKAGE_MESA3D=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_SWRAST=y    # Software rendering fallback
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y     # QEMU virtio-gpu
BR2_PACKAGE_MESA3D_OPENGL_EGL=y
BR2_PACKAGE_MESA3D_OPENGL_ES=y
BR2_PACKAGE_LIBDRM=y

# Input & device management
BR2_PACKAGE_LIBINPUT=y
BR2_PACKAGE_EUDEV=y
BR2_PACKAGE_LIBXKBCOMMON=y
BR2_PACKAGE_PIXMAN=y

# Kiosk browser
BR2_PACKAGE_COG=y
BR2_PACKAGE_WPEWEBKIT=y
```

### Image Size Impact

| Component | Current | With Desktop |
|-----------|---------|-------------|
| Root squashfs | ~6 MB | ~100-150 MB |
| Kernel | ~8 MB | ~12-15 MB |
| Total image | ~360 MB | ~450-520 MB |

The sys-a partition in genimage.cfg must grow from current size to accommodate the larger squashfs. Recommend 200 MB for sys-a.

### genimage.cfg Changes

```
partition sys-a {
    partition-type-uuid = 0FC63DAF-6483-4772-8E79-3D69D8477DE4
    image = "rootfs.squashfs"
    size = 200M          # was implicit/small, now needs room for desktop packages
}
```

---

## 7. Desktop Compositor Launch

In `child_main.cpp`, after the HTTP server starts:

```cpp
if (g_boot_mode == "desktop") {
    // Set required environment for Wayland
    setenv("XDG_RUNTIME_DIR", "/tmp", 1);
    setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);  // don't fail if no input devices yet

    pid_t cage_pid = fork();
    if (cage_pid == 0) {
        // Child: exec cage with cog as the client
        execlp("cage", "cage", "-s", "--",
               "cog", "http://localhost", nullptr);
        _exit(1);
    }
    // Parent: store cage_pid for monitoring
    g_cage_pid = cage_pid;
}
```

Cage flags:
- `-s` — allow VT switching (optional, can omit for true kiosk)
- `--` — separator between cage args and client command
- `cog http://localhost` — the kiosk browser pointing at the web UI

If Cage exits (crash, display lost), the supervisor can restart it.

---

## 8. Phased Implementation

### Phase 2a: Web UI Redesign (no Buildroot changes needed)
1. Restructure index.html to status-bar + tabs layout
2. Implement status bar with clock, model, RAM, IP
3. Implement tab navigation (Chat, Files, Dashboard, System)
4. Move chat to tab panel, add conversation strip
5. Build Files tab with fs.* tool integration
6. Build expanded Dashboard tab
7. Build System tab (model info, network, about)
8. All testable in a regular browser on the dev machine

### Phase 2b: Heartbeat Scheduler
1. Implement scheduler.cpp (timer thread, cron parser, persistence)
2. Implement tools_schedule.cpp (create, list, delete, update)
3. Implement /llamaste/notifications SSE endpoint
4. Implement notifications.js (SSE client, toast display, badges)
5. Add built-in system alerts (RAM, disk, temp)
6. Wire into agent prompt (schedule tools in system prompt)

### Phase 2c: Desktop Compositor (Buildroot changes)
1. Update kernel config (DRM, evdev, futex, GPU drivers)
2. Add Buildroot packages (cage, wlroots, wayland, mesa, cog, etc.)
3. Add compositor launch code to child_main.cpp
4. Update genimage.cfg for larger rootfs
5. Test in QEMU with virtio-gpu
6. Test in VirtualBox with VMSVGA
7. Build new ISO

### Phase 2d: Real Inference Integration
1. Build llama.cpp into the binary (replace stub_inference)
2. Download test model (qwen2.5-0.5b-instruct-q4_k_m.gguf)
3. Wire tokens_per_sec tracking
4. Test actual tool-calling agent loop
5. Test scheduled tasks with real inference

---

## 9. Testing Strategy

### Host Tests (Windows/WSL, no QEMU)
- Web UI: Open in browser, test all tabs, verify SSE streams
- Scheduler: Unit tests for cron parsing, task persistence, alert thresholds
- API: Test new /llamaste/system fields, /llamaste/notifications SSE, /llamaste/schedules CRUD

### QEMU Tests
- Boot with `llamaste.mode=desktop` + virtio-gpu → verify Cage launches
- Boot with `llamaste.mode=server` → verify no compositor
- Scheduler fires a test task → verify notification arrives via SSE
- System alert triggers → verify toast appears in web UI

### VirtualBox Tests
- Full desktop mode in VM with VMSVGA graphics
- Web UI accessible both locally (kiosk) and remotely (http://localhost:8080 via NAT)

---

## 10. Open Questions / Deferred

- **Audio**: Volume control requires ALSA/PulseAudio packages. Defer to Phase 2 voice I/O.
- **Cog availability in Buildroot**: Verify Cog is packaged in Buildroot 2024.02.9. Fallback: build as external package, or use `weston --kiosk` as alternative.
- **Touch input**: Not needed for Phase 2, but Cage supports it natively.
- **Multi-monitor**: Cage is single-output only. Fine for kiosk; Labwc if needed later.
- **Real inference RAM budget**: Desktop packages add ~100-150 MB. Combined with 500 MB compositor reservation, smaller models may be needed on low-RAM systems.
