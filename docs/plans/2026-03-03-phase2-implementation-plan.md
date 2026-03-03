# Phase 2 Desktop Mode Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Transform Llamaste from a headless server into a full desktop experience with kiosk compositor, enhanced web UI (status bar, tabs, file browser, dashboard, system settings), and a proactive heartbeat scheduler.

**Architecture:** Web UI restructured from sidebar+chat to status-bar+tabs+content. New scheduler thread for proactive alerts and cron tasks. Desktop mode launches Cage Wayland compositor with Cog kiosk browser pointing at localhost. All changes maintain server mode compatibility.

**Tech Stack:** C++17 (musl static), vanilla JS/CSS, cpp-httplib SSE, Cage/wlroots/Cog (Buildroot packages), nlohmann/json

---

## Phase 2a: Web UI Redesign

All tasks in this phase are pure web (HTML/JS/CSS) + minor C++ backend additions. Testable in a regular browser on the dev machine — no Buildroot changes needed.

---

### Task 1: Expand `/llamaste/system` API with hardware details

**Files:**
- Modify: `src/llamaste/child_main.cpp:222-337` (gather_system_info function)

**Step 1: Add hardware fields to gather_system_info**

In `gather_system_info()`, after the disk section (~line 334), add:

```cpp
    // Hardware details (from startup detection)
    info["cpu_model"] = g_hwinfo.cpu_model;
    info["cpu_cores"] = g_hwinfo.cpu_cores;
    info["gpu_name"] = g_hwinfo.gpu_detected ? g_hwinfo.gpu_name : "";
    info["gpu_detected"] = g_hwinfo.gpu_detected;
    info["has_avx2"] = g_hwinfo.has_avx2;
    info["has_avx512"] = g_hwinfo.has_avx512;

    // Placeholder fields for Phase 2 features
    info["tokens_per_sec"] = 0.0;  // will be real when inference is wired
    info["scheduled_tasks_count"] = 0;  // will be real when scheduler is wired
    info["active_alerts"] = json::array();
```

**Step 2: Test**

Run locally (or `curl http://localhost:8080/llamaste/system | python3 -m json.tool`). Verify `cpu_model`, `cpu_cores`, `gpu_name`, `has_avx2`, `has_avx512`, `tokens_per_sec`, `scheduled_tasks_count` fields appear in the JSON response.

**Step 3: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: expose hardware details and phase 2 fields in /llamaste/system"
```

---

### Task 2: Restructure index.html — status bar + tabs + content panels

**Files:**
- Modify: `src/llamaste/web/index.html`

**Step 1: Replace the entire body content**

Replace the current sidebar+main layout with the new status-bar+tabs+content layout:

```html
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Llamaste</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <div id="app">
    <!-- Status Bar -->
    <header id="status-bar">
      <div class="status-left">
        <span class="wordmark">LLAMASTE</span>
        <span id="status-clock" class="status-item">--:--</span>
      </div>
      <div class="status-center">
        <span id="status-model" class="status-item">--</span>
        <span class="status-sep">|</span>
        <span id="status-speed" class="status-item">-- t/s</span>
        <span class="status-sep">|</span>
        <span id="status-ram" class="status-item">-- / -- GB</span>
      </div>
      <div class="status-right">
        <span id="status-ip" class="status-item">--</span>
        <span id="connection-status" class="connecting"></span>
        <span id="notification-badge" class="notif-badge hidden">0</span>
      </div>
    </header>

    <!-- Tab Content Panels -->
    <div id="tab-content">
      <!-- Chat Tab (default) -->
      <div id="panel-chat" class="tab-panel active">
        <div id="conv-strip">
          <button id="new-conv-btn" onclick="newConversation()">+ New</button>
          <div id="conv-list"></div>
        </div>
        <div id="messages">
          <div class="message system">
            Welcome to Llamaste. The LLM is your operating system. Ask anything.
          </div>
        </div>
        <div id="input-area">
          <div id="input-wrapper">
            <textarea id="message-input"
                      placeholder="Ask Llamaste anything..."
                      rows="1"
                      autofocus></textarea>
            <button id="send-btn" onclick="sendMessage()">Send</button>
          </div>
        </div>
      </div>

      <!-- Files Tab -->
      <div id="panel-files" class="tab-panel">
        <div id="files-toolbar">
          <span id="files-path">/data/</span>
          <button onclick="filesUpload()" class="btn-small">Upload</button>
          <button onclick="filesNewFolder()" class="btn-small">New Folder</button>
        </div>
        <div id="files-list">
          <div class="loading">Loading...</div>
        </div>
        <div id="files-preview">
          <div class="preview-placeholder">Select a file to preview</div>
        </div>
      </div>

      <!-- Dashboard Tab -->
      <div id="panel-dashboard" class="tab-panel">
        <div class="dash-grid">
          <div class="dash-card">
            <h3>CPU</h3>
            <div class="dash-row">
              <span class="label">Usage</span>
              <span class="value" id="dash-cpu">--%</span>
            </div>
            <div class="dash-bar"><div class="dash-bar-fill" id="dash-cpu-bar" style="width:0%"></div></div>
            <div class="dash-row">
              <span class="label">Temp</span>
              <span class="value" id="dash-temp">--</span>
            </div>
          </div>
          <div class="dash-card">
            <h3>Memory</h3>
            <div class="dash-row">
              <span class="label">RAM</span>
              <span class="value" id="dash-ram">-- / -- MB</span>
            </div>
            <div class="dash-bar"><div class="dash-bar-fill" id="dash-ram-bar" style="width:0%"></div></div>
          </div>
          <div class="dash-card">
            <h3>Storage</h3>
            <div class="dash-row">
              <span class="label">/data</span>
              <span class="value" id="dash-disk">-- / -- GB</span>
            </div>
            <div class="dash-bar"><div class="dash-bar-fill" id="dash-disk-bar" style="width:0%"></div></div>
          </div>
          <div class="dash-card">
            <h3>Hardware</h3>
            <div class="dash-row"><span class="label">CPU</span><span class="value" id="dash-cpu-model">--</span></div>
            <div class="dash-row"><span class="label">Cores</span><span class="value" id="dash-cpu-cores">--</span></div>
            <div class="dash-row"><span class="label">GPU</span><span class="value" id="dash-gpu">--</span></div>
            <div class="dash-row"><span class="label">AVX2</span><span class="value" id="dash-avx2">--</span></div>
          </div>
          <div class="dash-card">
            <h3>Network</h3>
            <div class="dash-row"><span class="label">IP</span><span class="value" id="dash-ip">--</span></div>
            <div class="dash-row"><span class="label">Hostname</span><span class="value">llamaste.local</span></div>
            <div class="dash-row"><span class="label">Mode</span><span class="value" id="dash-mode">--</span></div>
            <div class="dash-row"><span class="label">Uptime</span><span class="value" id="dash-uptime">--</span></div>
          </div>
          <div class="dash-card">
            <h3>Model</h3>
            <div class="dash-row"><span class="label">Active</span><span class="value" id="dash-model">--</span></div>
            <div class="dash-row"><span class="label">Speed</span><span class="value" id="dash-speed">-- t/s</span></div>
          </div>
        </div>
      </div>

      <!-- System Tab -->
      <div id="panel-system" class="tab-panel">
        <div class="system-sections">
          <div class="system-card">
            <h3>Model Management</h3>
            <div class="dash-row"><span class="label">Active</span><span class="value" id="sys-model">--</span></div>
            <p class="text-muted">Model switching will be available when real inference is integrated.</p>
          </div>
          <div class="system-card">
            <h3>Scheduled Tasks</h3>
            <div id="schedule-list">
              <p class="text-muted">No scheduled tasks. Use chat to create schedules: "Every morning at 8am, give me a status report"</p>
            </div>
          </div>
          <div class="system-card">
            <h3>Network</h3>
            <div class="dash-row"><span class="label">IP</span><span class="value" id="sys-ip">--</span></div>
            <div class="dash-row"><span class="label">mDNS</span><span class="value">llamaste.local</span></div>
          </div>
          <div class="system-card">
            <h3>About</h3>
            <div class="dash-row"><span class="label">Version</span><span class="value">Llamaste v0.2.0</span></div>
            <div class="dash-row"><span class="label">Mode</span><span class="value" id="sys-mode">--</span></div>
          </div>
        </div>
      </div>
    </div>

    <!-- Tab Bar -->
    <nav id="tab-bar">
      <button class="tab active" data-tab="chat" onclick="switchTab('chat')">Chat</button>
      <button class="tab" data-tab="files" onclick="switchTab('files')">Files</button>
      <button class="tab" data-tab="dashboard" onclick="switchTab('dashboard')">Dashboard</button>
      <button class="tab" data-tab="system" onclick="switchTab('system')">System</button>
    </nav>

    <!-- Toast notifications container -->
    <div id="toast-container"></div>
  </div>

  <script src="/chat.js"></script>
  <script src="/dashboard.js"></script>
  <script src="/files.js"></script>
  <script src="/system.js"></script>
  <script src="/notifications.js"></script>
  <script src="/install.js"></script>
</body>
</html>
```

**Step 2: Add global tab switcher**

Add an inline script block or put this at the end of index.html before the other scripts:

```html
<script>
  window.switchTab = function(name) {
    document.querySelectorAll('.tab-panel').forEach(function(p) {
      p.classList.remove('active');
    });
    document.querySelectorAll('.tab').forEach(function(t) {
      t.classList.remove('active');
    });
    var panel = document.getElementById('panel-' + name);
    var tab = document.querySelector('.tab[data-tab="' + name + '"]');
    if (panel) panel.classList.add('active');
    if (tab) tab.classList.add('active');
    // Load files tab data on first switch
    if (name === 'files' && typeof filesRefresh === 'function') filesRefresh();
  };
</script>
```

**Step 3: Test in browser**

Open `index.html` in a browser. Verify: status bar at top, four tabs at bottom, chat panel visible by default, clicking tabs switches panels.

**Step 4: Commit**

```bash
git add src/llamaste/web/index.html
git commit -m "feat: restructure web UI to status-bar + tabs layout"
```

---

### Task 3: Rewrite style.css for new layout

**Files:**
- Modify: `src/llamaste/web/style.css`

**Step 1: Replace the CSS**

Keep all `:root` variables, scrollbar styles, and message/tool-call/input/installer styles. Replace the layout sections (sidebar, #app, #main, #chat-header, etc.) with:

- `#app`: `display: flex; flex-direction: column; height: 100%`
- `#status-bar`: 40px height, flex row, `background: var(--bg-secondary)`, border-bottom
- `#tab-content`: `flex: 1; overflow: hidden; position: relative`
- `.tab-panel`: `position: absolute; inset: 0; display: none; flex-direction: column; overflow: hidden`
- `.tab-panel.active`: `display: flex`
- `#tab-bar`: 48px height, flex row, `background: var(--bg-secondary)`, border-top
- `.tab`: button styles with active state using `var(--accent)` border-bottom
- Status bar items: monospace font, `var(--text-secondary)` color, separator pipes
- `.wordmark`: `var(--accent)` color, letter-spacing, bold
- Conversation strip (`#conv-strip`): horizontal flex, overflow-x auto, 40px height
- Dashboard grid (`.dash-grid`): CSS grid, `grid-template-columns: repeat(auto-fill, minmax(300px, 1fr))`, gap 16px, padding 16px
- `.dash-card`: background `var(--bg-secondary)`, border, border-radius 8px, padding 16px
- Files layout: toolbar (flex row, 40px), files-list (flex 1, overflow-y auto), files-preview (200px, border-top)
- System layout: `.system-sections` flex column, gap 16px, padding 16px
- `.system-card`: same as `.dash-card`
- Toast notifications: `#toast-container` fixed top-right, z-index 500
- `.toast`: background `var(--bg-secondary)`, border-left 3px solid `var(--accent)`, padding, margin-bottom 8px, animation slide-in
- Notification badge: `.notif-badge` 16px circle, background `var(--danger)`, white text, font-size 10px
- `.notif-badge.hidden`: `display: none`
- Responsive: at `max-width: 768px`, status-bar center section hidden, tabs use icons instead of text

**Step 2: Remove old sidebar CSS**

Delete all `#sidebar`, `#sidebar-header`, `#sidebar-toggle`, `#sidebar-overlay`, `#conversations-section`, `#chat-header` rules (they no longer exist in the HTML).

**Step 3: Test**

Open in browser. Verify dark theme, status bar fixed at top, tabs at bottom, content fills between them, chat panel with input at bottom. Test responsive at 768px.

**Step 4: Commit**

```bash
git add src/llamaste/web/style.css
git commit -m "feat: rewrite CSS for status-bar + tabs desktop layout"
```

---

### Task 4: Update chat.js to work in tab panel

**Files:**
- Modify: `src/llamaste/web/chat.js`

**Step 1: Update DOM references**

The chat no longer has a sidebar or chat-header. Remove:
- `toggleSidebar` function and its `window` export
- References to `document.getElementById('chat-title')`
- All sidebar-related code (toggling, overlay)

Update `connStatus` reference — the connection status dot is now in the status bar:
```js
const connStatus = document.getElementById('connection-status');
```
This still works since the element ID hasn't changed.

**Step 2: Update conversation list rendering**

Conversation list is now in `#conv-strip` (horizontal strip). Update `renderConversationList()` to render horizontally:
```js
function renderConversationList(conversations) {
    var listEl = document.getElementById('conv-list');
    listEl.innerHTML = '';
    for (var i = 0; i < conversations.length; i++) {
        var conv = conversations[i];
        var el = document.createElement('button');
        el.className = 'conv-chip';
        if (conv.id === conversationId) el.className += ' active';
        el.textContent = conv.title || conv.id.substring(0, 8);
        el.setAttribute('data-conv-id', conv.id);
        el.addEventListener('click', (function (id, title) {
            return function () { loadConversation(id, title); };
        })(conv.id, conv.title));
        listEl.appendChild(el);
    }
}
```

**Step 3: Remove window.toggleSidebar export**

Remove `window.toggleSidebar = toggleSidebar;` and the `toggleSidebar` function.

**Step 4: Test**

Open in browser. Send a message. Verify SSE streaming works, tool cards render, conversations strip is horizontal.

**Step 5: Commit**

```bash
git add src/llamaste/web/chat.js
git commit -m "feat: update chat.js for tab panel layout"
```

---

### Task 5: Rewrite dashboard.js for status bar + dashboard tab

**Files:**
- Modify: `src/llamaste/web/dashboard.js`

**Step 1: Split into two updater functions**

The dashboard now has two jobs:
1. Update the **status bar** (always visible) — clock, model, speed, RAM, IP
2. Update the **dashboard tab** (only when visible) — full stats grid

```js
(function () {
  'use strict';

  var POLL_INTERVAL = 5000;
  var pollTimer = null;
  var lastData = null;

  // --- Status bar elements ---
  var statusEls = {
    clock: document.getElementById('status-clock'),
    model: document.getElementById('status-model'),
    speed: document.getElementById('status-speed'),
    ram: document.getElementById('status-ram'),
    ip: document.getElementById('status-ip')
  };

  // --- Dashboard tab elements ---
  var dashEls = {
    cpu: document.getElementById('dash-cpu'),
    cpuBar: document.getElementById('dash-cpu-bar'),
    temp: document.getElementById('dash-temp'),
    ram: document.getElementById('dash-ram'),
    ramBar: document.getElementById('dash-ram-bar'),
    disk: document.getElementById('dash-disk'),
    diskBar: document.getElementById('dash-disk-bar'),
    cpuModel: document.getElementById('dash-cpu-model'),
    cpuCores: document.getElementById('dash-cpu-cores'),
    gpu: document.getElementById('dash-gpu'),
    avx2: document.getElementById('dash-avx2'),
    ip: document.getElementById('dash-ip'),
    mode: document.getElementById('dash-mode'),
    uptime: document.getElementById('dash-uptime'),
    model: document.getElementById('dash-model'),
    speed: document.getElementById('dash-speed')
  };

  // --- System tab elements ---
  var sysEls = {
    model: document.getElementById('sys-model'),
    ip: document.getElementById('sys-ip'),
    mode: document.getElementById('sys-mode')
  };

  // Clock updater (every second)
  setInterval(updateClock, 1000);
  updateClock();

  // Data polling
  fetchDashboard();
  pollTimer = setInterval(fetchDashboard, POLL_INTERVAL);

  document.addEventListener('visibilitychange', function () {
    if (document.hidden) {
      clearInterval(pollTimer);
      pollTimer = null;
    } else {
      fetchDashboard();
      pollTimer = setInterval(fetchDashboard, POLL_INTERVAL);
    }
  });

  function updateClock() {
    var now = new Date();
    var h = now.getHours();
    var m = now.getMinutes();
    var ampm = h >= 12 ? 'PM' : 'AM';
    h = h % 12 || 12;
    var months = ['Jan','Feb','Mar','Apr','May','Jun','Jul','Aug','Sep','Oct','Nov','Dec'];
    statusEls.clock.textContent = h + ':' + (m < 10 ? '0' : '') + m + ' ' + ampm +
      '  ' + months[now.getMonth()] + ' ' + now.getDate();
  }

  function fetchDashboard() {
    fetch('/llamaste/system')
      .then(function (r) { if (!r.ok) throw new Error(); return r.json(); })
      .then(function (data) {
        lastData = data;
        updateStatusBar(data);
        updateDashboardTab(data);
        updateSystemTab(data);
      })
      .catch(function () {
        statusEls.model.textContent = '--';
        statusEls.speed.textContent = '-- t/s';
        statusEls.ram.textContent = '-- / -- GB';
        statusEls.ip.textContent = '--';
      });
  }

  function updateStatusBar(d) {
    // Model (truncate to 20 chars)
    var modelName = d.model || '--';
    if (modelName.length > 20) modelName = modelName.substring(0, 17) + '...';
    statusEls.model.textContent = modelName;

    // Speed
    var tps = typeof d.tokens_per_sec === 'number' ? d.tokens_per_sec : 0;
    statusEls.speed.textContent = tps > 0 ? tps.toFixed(1) + ' t/s' : '-- t/s';

    // RAM in GB
    var ramUsed = (d.ram_used_mb || 0) / 1024;
    var ramTotal = (d.ram_total_mb || 0) / 1024;
    statusEls.ram.textContent = ramUsed.toFixed(1) + ' / ' + ramTotal.toFixed(1) + ' GB';

    // IP
    statusEls.ip.textContent = d.ip || '--';
  }

  function updateDashboardTab(d) {
    // CPU
    var cpuPct = typeof d.cpu_percent === 'number' ? d.cpu_percent : 0;
    dashEls.cpu.textContent = cpuPct.toFixed(0) + '%';
    dashEls.cpuBar.style.width = cpuPct + '%';
    setBarColor(dashEls.cpuBar, cpuPct);

    // Temp
    if (typeof d.temperature === 'number' && d.temperature > 0) {
      dashEls.temp.textContent = d.temperature.toFixed(0) + '\u00B0C';
      dashEls.temp.className = 'value ' + (d.temperature < 60 ? 'temp-green' : d.temperature < 80 ? 'temp-yellow' : 'temp-red');
    } else {
      dashEls.temp.textContent = '--';
    }

    // RAM
    var ramUsed = d.ram_used_mb || 0;
    var ramTotal = d.ram_total_mb || 1;
    var ramPct = (ramUsed / ramTotal) * 100;
    dashEls.ram.textContent = ramUsed.toFixed(0) + ' / ' + ramTotal.toFixed(0) + ' MB';
    dashEls.ramBar.style.width = ramPct.toFixed(1) + '%';
    setBarColor(dashEls.ramBar, ramPct);

    // Disk
    var diskUsed = d.disk_used_gb || 0;
    var diskTotal = d.disk_total_gb || 1;
    var diskPct = (diskUsed / diskTotal) * 100;
    dashEls.disk.textContent = diskUsed.toFixed(1) + ' / ' + diskTotal.toFixed(1) + ' GB';
    dashEls.diskBar.style.width = diskPct.toFixed(1) + '%';
    setBarColor(dashEls.diskBar, diskPct);

    // Hardware
    dashEls.cpuModel.textContent = d.cpu_model || '--';
    dashEls.cpuCores.textContent = d.cpu_cores || '--';
    dashEls.gpu.textContent = d.gpu_detected ? (d.gpu_name || 'Detected') : 'None';
    dashEls.avx2.textContent = d.has_avx2 ? 'Yes' : 'No';

    // Network
    dashEls.ip.textContent = d.ip || '--';
    dashEls.mode.textContent = d.mode || '--';
    dashEls.uptime.textContent = d.uptime ? formatUptime(d.uptime) : '--';

    // Model
    dashEls.model.textContent = d.model || '--';
    var tps = typeof d.tokens_per_sec === 'number' ? d.tokens_per_sec : 0;
    dashEls.speed.textContent = tps > 0 ? tps.toFixed(1) + ' t/s' : '-- t/s';
  }

  function updateSystemTab(d) {
    sysEls.model.textContent = d.model || '--';
    sysEls.ip.textContent = d.ip || '--';
    sysEls.mode.textContent = d.mode || '--';
  }

  function setBarColor(barEl, pct) {
    barEl.classList.remove('warn', 'danger');
    if (pct >= 90) barEl.classList.add('danger');
    else if (pct >= 70) barEl.classList.add('warn');
  }

  function formatUptime(seconds) {
    var d = Math.floor(seconds / 86400);
    var h = Math.floor((seconds % 86400) / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    if (d > 0) return d + 'd ' + h + 'h';
    if (h > 0) return h + 'h ' + m + 'm';
    return m + 'm';
  }
})();
```

**Step 2: Test**

Open in browser. Verify: status bar shows clock ticking, model name, RAM in GB. Dashboard tab shows all six cards with live data.

**Step 3: Commit**

```bash
git add src/llamaste/web/dashboard.js
git commit -m "feat: rewrite dashboard.js for status bar + dashboard tab"
```

---

### Task 6: Create files.js — file browser tab

**Files:**
- Create: `src/llamaste/web/files.js`

**Step 1: Write files.js**

The file browser calls `/llamaste/chat` (non-streaming) with tool-dispatch prompts, or directly dispatches `fs.list_directory` via a new thin endpoint. For Phase 2a, use the existing tools dispatch via a helper endpoint.

Actually — the simpler approach: add a thin `/llamaste/files` API endpoint to `child_main.cpp` that directly dispatches `fs.list_directory` and `fs.read_file` tools. This avoids going through the agent loop for file browsing.

Create `files.js` as an IIFE that:
- Maintains current path state (starts at `/data/`)
- `filesRefresh()` calls `GET /llamaste/files?path=/data/...` to list directory
- Renders file list with name, size, date
- Click on directory: navigate into it
- Click on file: show preview in bottom panel via `GET /llamaste/files?path=/data/...&action=read`
- Parent directory (`..`) entry for navigation
- Export `filesRefresh`, `filesUpload`, `filesNewFolder` to window

**Step 2: Add `/llamaste/files` endpoint to child_main.cpp**

After the existing API routes section (~line 708), add:

```cpp
svr.Get("/llamaste/files", [](const httplib::Request& req, httplib::Response& res) {
    std::string path = req.get_param_value("path");
    std::string action = req.get_param_value("action");
    if (path.empty()) path = "/data";

    json args;
    args["path"] = path;

    std::string result;
    if (action == "read") {
        result = g_tools.dispatch("fs.read_file", args.dump());
    } else {
        result = g_tools.dispatch("fs.list_directory", args.dump());
    }
    res.set_content(result, "application/json");
});
```

**Step 3: Add files.js to build system**

- Add `"files.js:WEB_FILES_JS"` to `embed_web.cmake` WEB_FILES list
- Add `${WEB_DIR}/files.js` to DEPENDS in `CMakeLists.txt:70`
- Add `svr.Get("/files.js", ...)` route in `child_main.cpp` with embed ifdef block

**Step 4: Test**

Open browser, switch to Files tab. Verify directory listing loads, navigation works, file preview shows content.

**Step 5: Commit**

```bash
git add src/llamaste/web/files.js src/llamaste/child_main.cpp src/llamaste/CMakeLists.txt src/llamaste/embed_web.cmake
git commit -m "feat: add file browser tab with /llamaste/files endpoint"
```

---

### Task 7: Create system.js — system settings tab

**Files:**
- Create: `src/llamaste/web/system.js`

**Step 1: Write system.js**

Minimal IIFE that populates the System tab from `/llamaste/system` data (already polled by dashboard.js — reuse `lastData` or re-fetch). For Phase 2a, this is mostly static info display. The scheduled tasks section will be populated in Phase 2b.

**Step 2: Add to build system**

Same pattern as files.js:
- Add `"system.js:WEB_SYSTEM_JS"` to `embed_web.cmake`
- Add `${WEB_DIR}/system.js` to DEPENDS in `CMakeLists.txt`
- Add route in `child_main.cpp`

**Step 3: Commit**

```bash
git add src/llamaste/web/system.js src/llamaste/child_main.cpp src/llamaste/CMakeLists.txt src/llamaste/embed_web.cmake
git commit -m "feat: add system settings tab"
```

---

### Task 8: Create notifications.js — toast system (placeholder)

**Files:**
- Create: `src/llamaste/web/notifications.js`

**Step 1: Write notifications.js**

Create the toast notification infrastructure. For Phase 2a, this is just the display layer — the SSE connection to `/llamaste/notifications` will be wired in Phase 2b.

Functions needed:
- `showToast(title, body, type)` — creates a toast element, appends to `#toast-container`, auto-removes after 8s
- `updateBadge(count)` — shows/hides the notification badge on the status bar
- Export `showToast` to window so other modules can trigger toasts

**Step 2: Add to build system**

Same pattern.

**Step 3: Test**

In browser console: `showToast('Test', 'This is a test notification', 'alert')`. Verify toast appears top-right and auto-dismisses.

**Step 4: Commit**

```bash
git add src/llamaste/web/notifications.js src/llamaste/child_main.cpp src/llamaste/CMakeLists.txt src/llamaste/embed_web.cmake
git commit -m "feat: add toast notification system (display layer)"
```

---

### Task 9: Integration test — full web UI in browser

**Step 1: Run the server locally**

Build and run `llamaste` on the dev machine (or WSL). Open `http://localhost:8080` in a browser.

**Step 2: Verify all tabs**

- Chat: Send messages, verify SSE streaming, tool cards
- Files: Navigate `/data/`, click files for preview
- Dashboard: All six cards show data, bars animate
- System: Model info, network info displayed

**Step 3: Verify status bar**

- Clock ticks every second
- Model name shows
- RAM shows in GB format
- IP shows
- Connection status dot is green

**Step 4: Verify responsive**

Resize to 768px width. Verify layout doesn't break.

**Step 5: Commit any fixes**

```bash
git add -A
git commit -m "fix: web UI integration fixes"
```

---

## Phase 2b: Heartbeat Scheduler

---

### Task 10: Create scheduler.h — data structures and interface

**Files:**
- Create: `src/llamaste/scheduler.h`

**Step 1: Write the header**

```cpp
#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <ctime>
#include "json.hpp"

struct ScheduledTask {
    std::string id;
    bool enabled = true;
    std::string prompt;            // what the agent should do
    std::string cron;              // cron expression (optional)
    time_t at = 0;                 // one-shot timestamp (optional)
    int every_seconds = 0;         // interval in seconds (optional)
    bool once = false;             // auto-delete after firing
    time_t created = 0;
    time_t last_run = 0;
    time_t next_run = 0;           // computed
};

struct AlertConfig {
    int ram_threshold_percent = 85;
    int disk_threshold_percent = 90;
    int temp_threshold_c = 80;
    int cooldown_seconds = 300;
};

struct Notification {
    std::string id;
    std::string type;              // "alert", "scheduled", "reminder"
    std::string title;
    std::string body;
    time_t time = 0;
};

// Callback type: called when a scheduled task fires
// The scheduler calls this with the task prompt, expects the agent response back
using SchedulerInferenceFn = std::function<std::string(const std::string& prompt)>;

// Callback type: called when a notification should be pushed to clients
using SchedulerNotifyFn = std::function<void(const Notification& notif)>;

class Scheduler {
public:
    void set_data_dir(const std::string& dir);
    void set_inference_fn(SchedulerInferenceFn fn);
    void set_notify_fn(SchedulerNotifyFn fn);

    // Task CRUD
    std::string create_task(const nlohmann::json& params);
    std::string list_tasks();
    std::string delete_task(const std::string& id);
    std::string update_task(const std::string& id, const nlohmann::json& params);

    // Alert config
    void set_alert_config(const AlertConfig& config);

    // Main loop (runs in its own thread)
    void start();
    void stop();

    // Get pending notifications (for SSE endpoint)
    std::vector<Notification> drain_notifications();

    // Current stats for /llamaste/system
    int active_task_count() const;
    time_t next_scheduled_time() const;

private:
    void run_loop();
    void check_tasks();
    void check_alerts();
    void fire_task(ScheduledTask& task);
    void load_tasks();
    void save_tasks();
    time_t compute_next_cron(const std::string& expr, time_t after) const;

    std::string data_dir_;
    SchedulerInferenceFn inference_fn_;
    SchedulerNotifyFn notify_fn_;

    std::vector<ScheduledTask> tasks_;
    AlertConfig alerts_;
    std::mutex mutex_;
    std::atomic<bool> running_{false};

    // Notification queue
    std::vector<Notification> pending_notifications_;
    std::mutex notif_mutex_;

    // Alert cooldown tracking
    time_t last_ram_alert_ = 0;
    time_t last_disk_alert_ = 0;
    time_t last_temp_alert_ = 0;
};
```

**Step 2: Commit**

```bash
git add src/llamaste/scheduler.h
git commit -m "feat: add scheduler header with task/alert/notification types"
```

---

### Task 11: Implement scheduler.cpp — core loop, cron parser, persistence

**Files:**
- Create: `src/llamaste/scheduler.cpp`

**Step 1: Write the implementation**

Key sections:
- `load_tasks()` / `save_tasks()`: Read/write `/data/llamaste/schedules.json` using nlohmann/json
- `compute_next_cron()`: Simple cron parser supporting `minute hour day month weekday` fields with `*` and numbers. Only needs basic support — not full cron (no ranges, no lists, no step values for Phase 2).
- `run_loop()`: While running, every 10 seconds: `check_tasks()`, `check_alerts()`, sleep
- `check_tasks()`: Iterate tasks, find any where `next_run <= now`, call `fire_task()`
- `fire_task()`: Call `inference_fn_(task.prompt)`, create Notification with result, push to queue
- `check_alerts()`: Read system stats (same as gather_system_info), compare against thresholds, push alerts with cooldown
- `start()`: Launch `run_loop()` in detached thread
- `stop()`: Set `running_ = false`
- Task CRUD: Validates input, generates ID, calls `save_tasks()`

**Step 2: Add to CMakeLists.txt**

Add `scheduler.cpp` to `LLAMASTE_SOURCES` list (after `net_mdns.cpp`).

**Step 3: Test**

Write a host test that creates a Scheduler, adds a task, manually calls `check_tasks()`, verifies notification is generated.

**Step 4: Commit**

```bash
git add src/llamaste/scheduler.cpp src/llamaste/CMakeLists.txt
git commit -m "feat: implement scheduler with cron, intervals, alerts, persistence"
```

---

### Task 12: Create tools_schedule.cpp — schedule.* tools

**Files:**
- Create: `src/llamaste/tools_schedule.cpp`
- Modify: `src/llamaste/tools.h:47-58` (add declaration)
- Modify: `src/llamaste/tools.cpp:98-105` (register)

**Step 1: Write tools_schedule.cpp**

Four tools following existing pattern (designated initializers):
- `schedule.create` — params: `prompt` (required), `cron`/`at`/`every_seconds` (one required), `id` (optional)
- `schedule.list` — no params, returns all tasks with next_run times
- `schedule.delete` — params: `id` (required)
- `schedule.update` — params: `id` (required), `enabled`/`prompt`/`cron`/`at`/`every_seconds` (optional)

Each handler calls the global Scheduler instance.

**Step 2: Add `register_schedule_tools` declaration to tools.h**

Add after `register_install_tools`:
```cpp
void register_schedule_tools(ToolRegistry& reg);
```

**Step 3: Register in tools.cpp**

In `register_all_tools()`, add:
```cpp
register_schedule_tools(reg);
```

**Step 4: Add schedule tools to system prompt**

In `prompt_builder.cpp`, the tools are auto-discovered from the registry, so no changes needed — they'll appear automatically in the "schedule" category.

**Step 5: Commit**

```bash
git add src/llamaste/tools_schedule.cpp src/llamaste/tools.h src/llamaste/tools.cpp src/llamaste/CMakeLists.txt
git commit -m "feat: add schedule.create/list/delete/update tools"
```

---

### Task 13: Wire scheduler into child_main.cpp + notification SSE

**Files:**
- Modify: `src/llamaste/child_main.cpp`

**Step 1: Add scheduler global and startup**

After the existing globals section (~line 76), add:
```cpp
#include "scheduler.h"
static Scheduler g_scheduler;
```

In `child_main()`, after mDNS start (~line 638), add:
```cpp
// Start heartbeat scheduler
g_scheduler.set_data_dir("/data/llamaste");
g_scheduler.set_inference_fn([](const std::string& prompt) -> std::string {
    // Create a temporary conversation for the scheduled task
    ConversationState conv;
    conv.system_prompt = g_system_prompt;
    conv.add_user_message(prompt);
    return agent_turn(conv, g_tools, stub_inference);
});
g_scheduler.set_notify_fn([](const Notification& /*notif*/) {
    // Notifications are drained by the SSE endpoint; no-op here
});
g_scheduler.start();
```

Before server shutdown (~line 820), add:
```cpp
g_scheduler.stop();
```

**Step 2: Add `/llamaste/notifications` SSE endpoint**

```cpp
svr.Get("/llamaste/notifications", [](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_header("Content-Type", "text/event-stream");
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    res.set_chunked_content_provider(
        "text/event-stream",
        [](size_t /*offset*/, httplib::DataSink& sink) -> bool {
            while (g_running) {
                auto notifs = g_scheduler.drain_notifications();
                for (const auto& n : notifs) {
                    json data;
                    data["type"] = n.type;
                    data["title"] = n.title;
                    data["body"] = n.body;
                    data["time"] = static_cast<long>(n.time);
                    data["id"] = n.id;
                    std::string line = "data: " + data.dump() + "\n\n";
                    sink.write(line.c_str(), line.size());
                }
                // Heartbeat every 15s to keep connection alive
                std::string heartbeat = ": heartbeat\n\n";
                sink.write(heartbeat.c_str(), heartbeat.size());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
            sink.done();
            return true;
        }
    );
});
```

**Step 3: Add schedule REST endpoints**

```cpp
svr.Get("/llamaste/schedules", [](const httplib::Request& /*req*/, httplib::Response& res) {
    res.set_content(g_scheduler.list_tasks(), "application/json");
});

svr.Post("/llamaste/schedules", [](const httplib::Request& req, httplib::Response& res) {
    auto body = json::parse(req.body, nullptr, false);
    if (body.is_discarded()) { res.status = 400; return; }
    res.set_content(g_scheduler.create_task(body), "application/json");
});

svr.Delete(R"(/llamaste/schedules/(.+))", [](const httplib::Request& req, httplib::Response& res) {
    std::string id = req.matches[1];
    res.set_content(g_scheduler.delete_task(id), "application/json");
});
```

**Step 4: Update gather_system_info to use real scheduler stats**

Replace the placeholder fields from Task 1:
```cpp
info["scheduled_tasks_count"] = g_scheduler.active_task_count();
auto next = g_scheduler.next_scheduled_time();
info["next_scheduled_time"] = next > 0 ? static_cast<long>(next) : 0;
```

**Step 5: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: wire scheduler, notification SSE, and schedule REST endpoints"
```

---

### Task 14: Wire notifications.js to SSE endpoint

**Files:**
- Modify: `src/llamaste/web/notifications.js`

**Step 1: Add SSE connection**

Add to the existing notifications.js:
```js
// Connect to notification stream
var notifSource = new EventSource('/llamaste/notifications');
var unreadCount = 0;

notifSource.onmessage = function(event) {
    try {
        var data = JSON.parse(event.data);
        showToast(data.title, data.body, data.type);
        unreadCount++;
        updateBadge(unreadCount);
    } catch (e) {
        // ignore parse errors (heartbeat comments, etc.)
    }
};

notifSource.onerror = function() {
    // Auto-reconnect is built into EventSource
};
```

**Step 2: Clear badge when Chat tab is active**

```js
// Reset badge when user views chat
document.querySelector('.tab[data-tab="chat"]').addEventListener('click', function() {
    unreadCount = 0;
    updateBadge(0);
});
```

**Step 3: Test**

Create a schedule via REST (`POST /llamaste/schedules` with `every_seconds: 30`). Wait 30s. Verify toast appears and badge increments.

**Step 4: Commit**

```bash
git add src/llamaste/web/notifications.js
git commit -m "feat: wire notifications.js to SSE endpoint"
```

---

### Task 15: Update System tab to show scheduled tasks

**Files:**
- Modify: `src/llamaste/web/system.js`

**Step 1: Fetch and display schedules**

Add to system.js: on tab switch, fetch `GET /llamaste/schedules` and render the task list in `#schedule-list`. Show each task with: enabled toggle, ID, schedule expression, prompt (truncated), next run time.

**Step 2: Commit**

```bash
git add src/llamaste/web/system.js
git commit -m "feat: display scheduled tasks in System tab"
```

---

## Phase 2c: Desktop Compositor (Buildroot Changes)

These tasks require WSL2 / Linux build environment.

---

### Task 16: Update kernel config for graphics and input

**Files:**
- Modify: `br2-external/board/llamaste/linux.config`

**Step 1: Add DRM, input, and Wayland prerequisites**

Append to linux.config:
```
CONFIG_DRM=y
CONFIG_DRM_KMS_HELPER=y
CONFIG_DRM_FBDEV_EMULATION=y
CONFIG_DRM_VIRTIO_GPU=y
CONFIG_DRM_VMWGFX=y
CONFIG_DRM_I915=y
CONFIG_INPUT_EVDEV=y
CONFIG_INPUT_MOUSEDEV=y
CONFIG_FUTEX=y
CONFIG_MULTIUSER=y
CONFIG_SYSVIPC=y
```

**Step 2: Commit**

```bash
git add br2-external/board/llamaste/linux.config
git commit -m "feat: enable DRM, evdev, GPU drivers in kernel config for desktop mode"
```

---

### Task 17: Add Buildroot packages for Cage + Cog

**Files:**
- Modify: `br2-external/configs/llamaste_x86_64_defconfig`

**Step 1: Add Wayland, compositor, and browser packages**

Append to defconfig:
```
BR2_PACKAGE_CAGE=y
BR2_PACKAGE_WAYLAND=y
BR2_PACKAGE_WAYLAND_PROTOCOLS=y
BR2_PACKAGE_MESA3D=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_SWRAST=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_VIRGL=y
BR2_PACKAGE_MESA3D_OPENGL_EGL=y
BR2_PACKAGE_MESA3D_OPENGL_ES=y
BR2_PACKAGE_LIBDRM=y
BR2_PACKAGE_LIBINPUT=y
BR2_PACKAGE_EUDEV=y
BR2_PACKAGE_LIBXKBCOMMON=y
BR2_PACKAGE_PIXMAN=y
```

**Note:** Cog/WPEWebKit availability in Buildroot 2024.02.9 must be verified. If not available, use `weston --kiosk` + `weston-simple-egl` or a custom package. Alternatively, build a minimal GTK3/WebKitGTK browser. This step may need iteration.

**Step 2: Update genimage.cfg for larger rootfs**

In `br2-external/board/llamaste/genimage.cfg`, increase sys-a partition:
```
size = 200M
```

**Step 3: Test Buildroot build**

```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "cd /root/llamaste-build/output && make llamaste_x86_64_defconfig && make"
```

**Step 4: Commit**

```bash
git add br2-external/configs/llamaste_x86_64_defconfig br2-external/board/llamaste/genimage.cfg
git commit -m "feat: add Cage, Wayland, Mesa packages to Buildroot for desktop mode"
```

---

### Task 18: Add compositor launch code to child_main.cpp

**Files:**
- Modify: `src/llamaste/child_main.cpp`

**Step 1: Add desktop mode compositor fork**

In `child_main()`, after the HTTP server `svr.listen()` call starts, add a thread that launches the compositor. Actually — since `svr.listen()` blocks, launch the compositor in a thread before `svr.listen()`:

After the scheduler start (~Task 13 location), before `svr.listen()`:

```cpp
#ifndef _WIN32
    // Desktop mode: launch Wayland kiosk compositor
    pid_t g_cage_pid = 0;
    if (g_boot_mode == "desktop") {
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);
        setenv("WLR_LIBINPUT_NO_DEVICES", "1", 1);

        // Delay briefly to ensure HTTP server is ready
        std::thread([&]() {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            pid_t pid = fork();
            if (pid == 0) {
                execlp("cage", "cage", "-s", "--",
                       "cog", "http://localhost", nullptr);
                // If exec fails, try weston fallback
                execlp("weston", "weston", "--shell=kiosk",
                       "--continue-without-input", nullptr);
                fprintf(stderr, "[child] Failed to exec compositor\n");
                _exit(1);
            } else if (pid > 0) {
                g_cage_pid = pid;
                fprintf(stderr, "[child] Desktop compositor launched (pid %d)\n", pid);
            } else {
                fprintf(stderr, "[child] Failed to fork compositor: %s\n", strerror(errno));
            }
        }).detach();
    }
#endif
```

**Step 2: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: launch Cage/Cog compositor in desktop mode"
```

---

### Task 19: Test desktop mode in QEMU

**Step 1: Build with desktop packages**

Full Buildroot rebuild in WSL2.

**Step 2: Test with QEMU + virtio-gpu**

```bash
qemu-system-x86_64 \
  -m 4096 \
  -smp 2 \
  -drive file=output/images/llamaste.img,format=raw \
  -device virtio-gpu-pci \
  -display gtk \
  -append "llamaste.mode=desktop" \
  -kernel output/images/bzImage
```

Verify: Cage starts, Cog opens fullscreen, web UI is visible.

**Step 3: Test server mode still works**

```bash
scripts/qemu-boot-test.sh output/images/llamaste.img
```

Verify all existing E2E tests still pass (no compositor launched in server mode).

**Step 4: Commit any fixes**

```bash
git commit -am "fix: desktop mode QEMU boot fixes"
```

---

## Phase 2d: Real Inference Integration

---

### Task 20: Integrate llama.cpp inference (replace stub)

This is a large task that requires building llama.cpp into the binary. Detailed sub-steps:

1. Add llama.cpp as a Buildroot dependency or vendor it
2. Add `#include "llama.h"` and link against libllama
3. Create `inference.cpp` wrapping llama.cpp's server-style inference
4. Replace `stub_inference` callback with real inference
5. Download test model to `/data/models/`
6. Add `tokens_per_sec` tracking (measure time between first and last token)
7. Test full agent loop: user message -> LLM -> tool call -> tool result -> LLM -> final response

This task is large enough to warrant its own design doc and implementation plan. Create `docs/plans/2026-XX-XX-inference-integration-design.md` when ready.

**Step 1: Placeholder commit**

```bash
echo "# Inference integration — to be planned" > docs/plans/inference-integration-todo.md
git add docs/plans/inference-integration-todo.md
git commit -m "docs: placeholder for inference integration plan"
```

---

## Summary

| Phase | Tasks | Key Deliverables |
|-------|-------|-----------------|
| 2a | 1-9 | New web UI layout, status bar, 4 tabs, file browser |
| 2b | 10-15 | Scheduler thread, schedule.* tools, notification SSE, toasts |
| 2c | 16-19 | Kernel graphics, Buildroot packages, Cage compositor launch |
| 2d | 20 | llama.cpp integration (separate plan) |

Total: ~20 tasks, estimated 3-5 implementation sessions.
