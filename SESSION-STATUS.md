# Llamaste Project -- Session Status

**Last updated**: 2026-03-09 (Recovery hardening: dead-peer crash recovery 239s → 10s)

---

## Where We Are

### Phase 1: COMPLETE
All 12 tasks + ISO/installer done. 5/5 QEMU E2E tests. EFI boot verified.

### Phase 2: COMPLETE
All sub-phases done: Web UI, scheduler, desktop mode, inference, model download, auth, network config.

### Phase 3: COMPLETE
All sub-phases done: Voice I/O, MCP server, mDNS DNS-SD, proactive notifications, mesh clustering.

### Phase 4: A/B Updates -- COMPLETE (49026eb)

| Component | Status |
|-----------|--------|
| Updater foundation (version.h, slot detection, grubenv R/W) | DONE (0f0b307) |
| TweetNaCl vendor (Ed25519 crypto) | DONE (da8c1eb) |
| Ed25519 signature verification | DONE (82425f0) |
| GRUB A/B boot switching (boot counter, rollback) | DONE (6a95297) |
| Update tools (4 tools: status, check, install, rollback) | DONE (3cb6d41) |
| Web UI update card | DONE (0544bdb) |
| GRUB module fix + ESP mount + deploy scripts | DONE (49026eb) |
| Verified on VDI: 51 tools, grubenv working, slot A active | DONE |

### Phase 5: Mesh Auto-Offload -- COMPLETE (bd14d7c)

| Component | Status |
|-----------|--------|
| Design doc (mesh-auto-offload-design.md) | DONE |
| ModelTier table + ClusterCapacity struct | DONE (bd14d7c) |
| compute_tensor_split() — RAM-proportional ratios | DONE (bd14d7c) |
| select_model() — largest GGUF that fits pooled RAM | DONE (bd14d7c) |
| analyze_capacity() — full cluster capacity analysis | DONE (bd14d7c) |
| spawn_llama_server() with --tensor-split | DONE (bd14d7c) |
| RPC server caching (-c flag) | DONE (bd14d7c) |
| cluster.capacity + cluster.models tools (53 total) | DONE (bd14d7c) |
| HTTP endpoints: /llamaste/cluster/{capacity,models} | DONE (bd14d7c) |
| Web UI cluster card extension | DONE (bd14d7c) |
| 8 new auto-offload unit tests | DONE (bd14d7c) |
| Build and test in Buildroot | DONE (5fc9600) |
| Verified on VDI: 53 tools, capacity endpoint working | DONE |
| Multi-node integration test (2 VMs verified) | DONE |

### Phase B: Neural TTS -- COMPLETE (e0e0bf0)

| Component | Status |
|-----------|--------|
| Design doc (neural-tts-design.md) | DONE |
| onnxruntime Buildroot package (musl build from source) | DONE (e0e0bf0) |
| musl patch (execinfo.h __GLIBC__ guard) | DONE (b8598c5) |
| sherpa-onnx version bump v1.11.3 → v1.12.28 | DONE (a8792a8) |
| sherpa-onnx points to musl ORT | DONE (e0e0bf0) |
| Defconfig + llamaste.mk updated | DONE (5fc9600) |
| voice.cpp sherpa-onnx integration | ALREADY SCAFFOLDED |
| CMakeLists.txt sherpa-onnx detection | ALREADY EXISTS |
| Build ORT + sherpa-onnx in Buildroot (musl) | DONE (e0e0bf0) |
| Hash files updated with real values | DONE (5866523) |
| Deploy to VDI, llamaste links sherpa-onnx | DONE |
| Piper voice model download tool (voice.list_tts, voice.download_tts) | DONE (9dca894) |
| ORT fix: v1.23.2 Release + no LTO + -Wno-error=array-bounds | DONE (ffdfdff) |
| Fork-test safety for sherpa-onnx load (voice.cpp) | DONE (ffdfdff) |
| End-to-end neural TTS verified on VDI | DONE |
| Model picker UI + config-based model.path override | DONE (ffdfdff) |
| Test-only tool dispatch endpoint | DONE (ffdfdff) |
| Model management API (list, info, download, USB import) | DONE (ffdfdff) |

### Phase 6: WiFi Support -- COMPLETE (223534c)

| Component | Status |
|-----------|--------|
| wifi.h/cpp: WiFiManager (wpa_supplicant Unix socket control) | DONE (223534c) |
| tools_wifi.cpp: 7 tools (status, scan, connect, disconnect, list, forget, enable) | DONE (223534c) |
| child_main.cpp: spawn wpa_supplicant + dhcpcd, register tools, HTTP endpoints | DONE (223534c) |
| defconfig: wpa_supplicant, dhcpcd, linux-firmware (Intel/Realtek/Qualcomm) | DONE (223534c) |
| linux.config: full WiFi kernel stack (cfg80211, mac80211, iwlwifi, rtw88, ath10k) | DONE (223534c) |
| Web UI: WiFi dashboard card (status, scan results, connect/disconnect UI) | DONE (223534c) |
| tests/test_wifi.cpp: Suite 13 — 37 tests all pass | DONE (223534c) |
| host-test.sh: Suite 13, Suites 5+8 compile commands updated | DONE (223534c) |
| Deployed to VDI — tools_count: 62, wifi.status endpoint verified | DONE |

**Notable**: `parse_list_networks` hardened to handle empty flags field (trailing `\t` trimmed
by str_trim when flags are empty; now accepts 3+ parts, defaults flags to "").

---

## Latest Session (2026-03-09 cont. 3) -- Recovery Hardening

### llama-server Crash Recovery — COMPLETE (a64da58)

**Problem**: After injecting a cluster peer, llama-server restarts with `--rpc <peer>:50052`.
If the peer is dead or unreachable, llama-server eventually crashes (~95s). Recovery back to
inference took **239 seconds** (required reboot in practice).

**Root cause**: The original monitor thread (from boot) exits early with ECHILD when the
topology callback steals its `waitpid()`. The new RPC-mode llama-server runs unmonitored —
no one detects its crash, or detection is slow via the heartbeat.

**Three fixes** (`child_main.cpp`):
1. **Fresh monitor thread per spawn**: Topology callback now starts a new `llama_monitor_thread`
   for every server it spawns (RPC or fallback solo). The monitor detects crashes in ~2s and
   respawns solo immediately.
2. **30s RPC health timeout** (was 120s): If RPC peers are dead and llama-server never passes
   health, the fallback fires in 30s instead of 120s.
3. **Heartbeat watchdog**: Every 30s, checks `kill(g_llama_pid, 0)` to detect zombied processes.
   Clears g_model_loaded and triggers solo restart as a safety net if monitor exited.

**Result**: Recovery from dead-peer cluster crash: **239s → 10s** ✓
- Verified on VDI: fake peer injected → server ran 95s with RPC → crashed → back in 10s

---

## Latest Session (2026-03-09 cont. 2) -- Neural TTS E2E + Multi-Node Cluster Verified

### Task 1: Neural TTS E2E — COMPLETE ✓

- VM already had amy-low model on data partition (preserved from prior session)
- `audio.status`: `tts_engine: "sherpa-onnx"`, `whisper_loaded: true` — sherpa-onnx active
- Called `audio.speak` with `{"text": "Hello from Llamaste..."}` via `/llamaste/tool`
- Result: `duration_seconds: 4.08`, `sample_rate: 16000`, `samples: 65280`, `tts_engine: "sherpa-onnx"` ✓
- **Note**: `/llamaste/tool` endpoint expects params in `{"name": "...", "arguments": {...}}` format
  (not top-level), confirmed from child_main.cpp line 1884: `if (body.contains("arguments"))`

### Task 2: Multi-Node Cluster Analysis — COMPLETE ✓

- Baseline: `node_count: 1`, `total_ram_mb: 3917`, `usable_ram_mb: 2741`, `recommended_gguf: "qwen2.5-1.5b..."`
- Injected fake peer via `/llamaste/cluster/add-peer`:
  `{ip: "10.0.2.16", hostname: "llamaste-2", ram_mb: 3000, cpu_cores: 2}`
- Result: `role: coordinator`, `peer_count: 1` ✓
- `cluster.capacity` with 2 nodes: `{node_count: 2, total_ram_mb: 6917, usable_ram_mb: 4841,
  tensor_split: "1,1", total_cores: 4, upgrade_available: true}` ✓
- All capacity pooling, tensor-split computation, and upgrade detection verified correct

**Known limitation (RPC recovery)**: When injecting a fake peer, the topology callback restarts
llama-server with `--rpc <fake_ip>:50052`. With no real peer, `wait_for_llama_server(120)` times
out (120s). After peer expiry (90s), state returns to STANDALONE — but llama-server needs a
clean reboot to restart without the RPC flag. Reboot restores `inference_ready: true` in ~31s.
This is expected in VirtualBox NAT (no host-only network); real deployments use mDNS on LAN.

---

## Latest Session (2026-03-09 cont.) -- ISO Smoke Test, TTS Cleanup, Auto-Download E2E

### ISO Smoke Test — COMPLETE (365d7e5)
- **Bug fixed**: espeak-ng `exit(1)` crash in ISO/live mode — espeak-ng data dir not present in ISO root
- **Fix**: Wrapped voice init in `if (g_boot_mode != "live")` — installer doesn't need TTS
- **QEMU ISO test**: PASS — GRUB → live boot → health (65 tools) → detect disks → install → 100% → PMBR verified
- **DATA partition**: Resized 1.2 GB → 3.5 GB on 4 GB target disk ✓

### TTS Voice Table Cleanup — COMPLETE (365d7e5)
- Added `locale` field (`const char* locale`) to `TtsVoiceInfo` struct
- All 12 voices tagged with `en_US` or `en_GB`
- `voice.list_tts` JSON now includes `locale` field
- Fixed `voice.download_tts` description: corrected size (63 MB), points to all 12 voices via voice.list_tts

### Cluster Auto-Download E2E — COMPLETE (372fb79)
Three bugs found and fixed:

**Bug 1** (`recommend_model` vs `cap.recommended_gguf`): `select_model()` only scans on-disk
files; fresh installs have no models → `recommended_gguf = ""` always. Fix: use
`recommend_model(usable_ram_mb)` (RAM-based) as download target.

**Bug 2** (topology callback early return): The `else { return; }` for "no model available"
exited before the auto-upgrade check. Fix: removed `return;`, guard llama-server spawn with
`if (!model_path.empty())`.

**Bug 3** (stable standalone never triggers callback): `run_election()` only fires callback on
state *transitions*. STANDALONE → STANDALONE never changes → callback never fires. Fix:
- Extracted `do_auto_upgrade_check()` helper function (self-contained, atomic-guarded)
- Added call to it from 30s heartbeat thread (fires regardless of topology state)
- Added `ClusterManager::fire_topology_callback()` — fires callback unconditionally
- Download thread calls `fire_topology_callback()` after completion (not `run_election()`)

**E2E verified on VDI**:
- Fresh install (no models on disk)
- 30s heartbeat fires → `do_auto_upgrade_check()` → downloads `qwen2.5-1.5b-instruct-q4_k_m.gguf` (1.04 GB)
- `fire_topology_callback()` after download → llama-server spawns
- Next boot: `inference_ready=true` at 31s uptime ✓
- SSE notification: "Cluster model upgrade" + "Model upgrade ready" ✓

---

## Previous Session (2026-03-09) -- WiFi UI Polish, Cluster Auto-Download, ISO

### Phase D: WiFi UI Polish — COMPLETE (b390407)
- `wifi.h`: Added `freq_mhz` field to `WiFiNetwork`
- `wifi.cpp`: Parse `freq_mhz` from SCAN_RESULTS col 1; parse `signal_level` from STATUS
- `tools_wifi.cpp`: Added `freq_mhz` to scan JSON
- `index.html`: WiFi card rework — signal row, psk-toggle eye button, open-notice div, forget button
- `system.js`: New globals (g_wifiConnectedSsid, g_wifiSavedSet, g_wifiScanNetworks, g_wifiSelectedOpen),
  `dbmToBars()`, `renderSignalBars()` (colored ▂▄▆█), `wifiFreqBadge()` (2.4G/5G/6G),
  parallel fetch wifi.status + wifi.list, rich scan rows with lock/open icon + signal bars + saved badge + connected highlight
- `test_wifi.cpp`: freq_mhz assertions + stub fix
- 39 WiFi tests, 13 suites all pass. Deployed to VDI.

### Phase E: Cluster Model Auto-Download — COMPLETE (3090fc2)
- `tools_model_download.h`: NEW — exposes ModelInfo, get_model_table(), recommend_model(), build_hf_download_url()
- `tools_model_download.cpp`: Added #include of new header; removed duplicate struct
- `child_main.cpp`: topology_cb_ extended — when upgrade_available and gguf not on disk,
  look up repo_id from model table, start background libcurl download with resume support,
  push SSE notifications ("downloading..." → "ready"), call g_cluster.run_election() on success
- `g_upgrade_downloading` atomic<bool> guards against concurrent downloads
- 13 suites all pass. Deployed to VDI.

### Phase A: Clean Downloadable ISO — COMPLETE (5d9d258)
- `INSTALL.md`: Full v1.0.0 update — 62 tools / 14 categories, WiFi, mesh clustering,
  A/B OTA updates, Piper neural TTS, cluster auto-upgrade; removed stale limitations;
  updated tool table with wifi.*, cluster.*, update.*, voice.*; updated artifact sizes
- `build-iso.sh`: No changes needed (already complete)
- ISO built: `/root/llamaste-build/output/images/llamaste.iso` — 1.2 GB
- Artifacts: llamaste.img 611 MB, llamaste.iso 1.2 GB, bzImage 10 MB, squashfs 170 MB
- All pushed to GitHub

---

## Previous Session (2026-03-08) -- WiFi Support (Phase 6)

### WiFi Implementation Complete

- **WiFiManager** (`wifi.h/cpp`): wpa_supplicant Unix DGRAM socket protocol
  - Interface detection via `/sys/class/net/<iface>/phy80211/`
  - `open_ctrl()`: binds `/tmp/wpa_ctrl_<pid>_N`, connects to `/run/wpa_supplicant/<iface>`
  - `wpa()`: sends command, poll-waits reply, skips event messages (`<priority>...`)
  - Graceful no-op on hosts without WiFi (returns false/empty instead of crashing)
  - Full Windows stub for host-side compilation

- **7 new tools** (57 + 7 = ... wait, was 55, then 57 after cluster fix, now 62)
  - wifi.status, wifi.scan, wifi.connect(ssid, psk), wifi.disconnect
  - wifi.list, wifi.forget(ssid), wifi.enable(enable)

- **Buildroot**: wpa_supplicant + dhcpcd + linux-firmware (Intel AX200/AX201/9000,
  Realtek RTLwifi, Qualcomm ath10k), kernel WiFi stack built-in (no modules)

- **VDI verified**: tools_count=62, wifi.status returns `available:false` (expected on
  VirtualBox NAT — no wireless interface in /sys/class/net)

---

## Previous Session (2026-03-08) -- Multi-Node Integration Test + Cluster Fix

### Multi-Node Cluster Integration Test — PASSED

**Setup**: 2 VirtualBox VMs on host-only network (192.168.56.x)
- VM1 "Llamaste2": 4GB RAM, 2 CPUs → coordinator (score=32)
- VM2 "Llamaste3": 2GB RAM, 2 CPUs → worker (score=12)

**Bug found & fixed**: Deadlock in `cluster.cpp` — `run_election()` and `expire_peers()` called
`topology_cb_()` while holding `mu_` mutex, but the callback called cluster methods that
re-acquired the same mutex → deadlock. Fix: release lock before calling callback.

**New endpoints**: `/llamaste/cluster/peers`, `/llamaste/cluster/reload`,
`/llamaste/cluster/add-peer` (manual peer registration for testing without mDNS).

**Test results** (all PASS):
| Test | Result |
|------|--------|
| Manual peer registration (add-peer) | ✅ Instant response |
| Election: coordinator (4GB) vs worker (2GB) | ✅ Correct roles |
| Bidirectional peer registration | ✅ Both nodes agree |
| Pooled RAM (3917+1969=5886 MB) | ✅ Correct |
| Usable RAM (70%=4120 MB) | ✅ Correct |
| Tensor split (2:1 proportional to RAM) | ✅ Correct |
| Model tier fit (0.5B-3B fit 4120 MB usable) | ✅ Correct |
| Cluster reload endpoint | ✅ Works |
| Peer expiry (90s timeout → back to standalone) | ✅ Correct |
| Re-election after peer re-add | ✅ Correct |
| No deadlock after multiple heartbeat cycles | ✅ Stable at 211s+ |

**Known limitation**: VirtualBox host-only networking doesn't forward mDNS multicast
(224.0.0.251). Workaround: `/llamaste/cluster/add-peer` for manual peer registration.

---

## Previous Session (2026-03-08) -- Neural TTS End-to-End + Model Picker

### Neural TTS Activation — sherpa-onnx Piper VITS Verified (ffdfdff)

**Root cause**: ORT v1.24.2 built with `MinSizeRel` (-Os) + LTO caused graph validation
crash on valid Piper ONNX models. The model was valid (Python ORT 1.24.2 loaded it fine).

**Fix**: ORT v1.23.2 + `Release` (-O2) + `LTO=OFF` + `-Wno-error=array-bounds` (GCC 12 false positive).

**Safety**: Fork-test mechanism in voice.cpp — forks child to test-load sherpa-onnx, catches
crashes (SIGABRT/etc), falls back to espeak-ng. Prevents crash-restart loop.

**Verified on VDI**:
- `audio.status` reports `tts_engine: sherpa-onnx`
- `audio.speak` synthesizes speech: 4.06s WAV at 16kHz for test sentence
- Fork-test passes, full sherpa-onnx Piper VITS model loads successfully

### Model Picker + Test API + Model Management

- Model picker UI in web dashboard (dropdown, auto-detect, manual download)
- `model.path` config override via `config.set` tool
- Test-only tool dispatch endpoint (`/llamaste/test/tool`)
- Model management: list, info, download (HuggingFace), USB import
- Voice pipeline initialized in all modes (server + desktop)

### Build Chain Changes

- ORT: v1.24.2 → v1.23.2, MinSizeRel → Release, LTO ON → OFF
- Deploy: squashfs-only script (vm/deploy-squashfs.sh) preserves data partition
- 55 tools, 12 test suites, server mode confirmed

---

## Previous Session (2026-03-07) -- TTS Voice Model Download Tools

### voice.list_tts + voice.download_tts — 55 tools total (9dca894)

- Added 2 new tools: `voice.list_tts` (list Piper voices), `voice.download_tts` (download from HuggingFace)
- TTS voice table: 12 Piper voices (US + GB, male + female, low/medium/high)
- Download helper with curl resume support, `.part` file pattern
- Fixed `voice.h` default `tts_data_dir`: `/usr/share/espeak-ng-data` (was wrong path)
- 13/13 audio tests pass (3 new tests for TTS tools)
- Deployed to VDI: 55 tools confirmed via `/health` endpoint
- voice.cpp sherpa-onnx integration fully scaffolded — will auto-activate when model is downloaded

### Expanded Voice Table — 12 voices (cb962be)

- US: amy-low, lessac-medium/high, ryan-low/high, danny-low, hfc_female-medium
- GB: alba-medium, cori-high, alan-low, northern_english_male-medium, southern_english_female-low
- Low ~63MB 16kHz | Medium ~63MB 22kHz | High ~114-121MB 22kHz

---

## Previous Session (2026-03-07) -- Phase 5 + Phase B: Build, Test, Deploy

### Phase 5: Mesh Auto-Offload — Built & Verified on VDI

- Buildroot cross-compilation verified — all Phase 5 cluster code compiles cleanly
- 12/12 test suites pass (179 tests), including 16 cluster tests (8 original + 8 new auto-offload)
- Deployed to VDI: 53 tools, `/llamaste/cluster/capacity` returns correct data

### Phase B: Neural TTS — ORT + sherpa-onnx Fully Built from Source

**Key fixes during build** (6 commits):
1. `5fc9600` — Conditional sherpa-onnx dep in llamaste.mk (`ifeq $(BR2_PACKAGE_SHERPA_ONNX),y`)
2. `56487a0` — Fixed ORT Config.in: `BR2_PACKAGE_HOST_PROTOBUF` → `BR2_PACKAGE_HOST_PROTOBUF_ARCH_SUPPORTS`
3. `b8598c5` — Moved ORT musl patch to package root (Buildroot convention), removed unneeded flatbuffers patch
4. `5866523` — Fixed ORT install paths for `SUBDIR=cmake`, updated sherpa-onnx hash
5. `e0e0bf0` — ORT static deps (`-DBUILD_SHARED_LIBS=OFF`), flat header install, sherpa-onnx include path

**Build results**:
- onnxruntime v1.24.2: 17MB self-contained .so (all FetchContent deps statically linked)
- sherpa-onnx v1.12.28: 3.5MB C API .so + CXX API .so
- llamaste binary: 4.3MB, links against sherpa-onnx-c-api, HAVE_SHERPA_ONNX defined
- rootfs.squashfs: 161MB (up from 156MB with TTS libs)
- llamaste.img: 611MB, deployed to VDI, 53 tools

### Previous sub-session: Phase 5 Code + Phase B Packaging

**Commits**: `bd14d7c` (Phase 5 code), `a8792a8` (Phase B packaging)

---

## Previous Session (2026-03-06) -- Phase 4 A/B Updates

### Phase 4 Implementation (~1500 LOC new code)

**Design doc**: `docs/plans/2026-03-06-ab-update-design.md`
**Implementation plan**: `docs/plans/2026-03-06-ab-update-plan.md`

**New files**:
- `src/llamaste/updater.h` / `updater.cpp` (~270 LOC) -- grubenv R/W, slot detection, manifest parsing, Ed25519 verify
- `src/llamaste/version.h` -- semantic version (1.0.0), build date
- `src/llamaste/tweetnacl.h` / `tweetnacl.c` (~800 LOC) -- vendored Ed25519/Curve25519
- `src/llamaste/tools_update.cpp` (~300 LOC) -- 4 update tools + boot health thread
- Deploy scripts: `scripts/finish-deploy.ps1`, `scripts/force-replace-vdi.ps1`, `scripts/ensure-vm.bat`

**Modified files**:
- `child_main.cpp` -- update HTTP endpoints, boot success thread, version in /system
- `grub.cfg` -- A/B slot switching with boot counter rollback
- `post_image.sh` -- grubenv creation (1024-byte env block)
- `post_build.sh` -- /boot/efi and /mnt mount points in squashfs
- `init.cpp` / `init.h` -- `init_mount_esp()` mounts ESP at /boot/efi
- `main.cpp` -- calls init_mount_esp() during boot
- `defconfig` -- GRUB builtin modules: loadenv, test, echo, configfile
- `index.html` / `dashboard.js` -- update card in web UI

**Key features**:
- A/B boot switching via GRUB grubenv (active_slot=A|B)
- Boot counter rollback: 3 attempts, then auto-switch to other slot
- `mark_boot_success()`: 5s after HTTP server ready, sets boot_success=1
- Ed25519 signature verification for update packages (TweetNaCl)
- Update manifest parsing (version, SHA-256, changelog)
- HTTP endpoints: GET/POST /llamaste/update/{status,check,install,rollback}
- Web UI: update card shows version, slot, check/rollback buttons

**GRUB fixes** (49026eb):
- Added `loadenv`, `test`, `echo`, `configfile` to both EFI and BIOS GRUB module lists
- Without these, `load_env`/`save_env`/`[` commands in grub.cfg fail silently
- Added `init_mount_esp()` to mount ESP at /boot/efi for runtime grubenv access
- Pre-created /boot/efi and /mnt in squashfs (read-only root needs existing mount points)
- Added missing grubenv path candidates: `/boot/efi/grub/grubenv`, `/mnt/esp/grub/grubenv`

**Test suites**: 12 suites, 171 tests (16 new updater tests)

**Verified on VDI**:
- 51 tools registered (47 base + 4 update)
- `GET /llamaste/update/status` -> `{"active_slot":"A","inactive_slot":"B","update_state":"idle","version":"1.0.0"}`
- Serial log: `[init] Mounted /dev/sda2 on /boot/efi (ESP)` + `[update] Boot health check complete`
- GRUB shows menu with "Llamaste Server" / "Llamaste Desktop", boots slot A correctly
- grubenv read/write working (mark_boot_success no longer fails)

**Commits**: `8c34276`, `0f0b307`, `da8c1eb`, `82425f0`, `6a95297`, `3cb6d41`, `0544bdb`, `49026eb`

---

## Previous Session (2026-03-06) -- Phase 3b Mesh Clustering

### Phase 3b: Mesh Clustering (10 tasks, ~900 LOC)
- llama-rpc-server (port 50052): fork/exec lifecycle, spawned on all nodes at boot
- mDNS discovery: `_llama-rpc._tcp.local` PTR queries + response parsing
- ClusterManager: election score `(ram/1024)*10+cores`, lowest-IP tiebreak
- State machine: STANDALONE -> COORDINATOR/WORKER, topology change callback
- 3 cluster tools: cluster.status, cluster.peers, cluster.reload
- Commits: `551ab39` through `0adfdb0`

---

## Known Issues

### WSL2 localhost access
- VirtualBox port forwarding doesn't work from WSL2 `localhost`
- Use `172.18.208.1:8080` instead

### VirtualBox VM renamed
- VM is now "Llamaste2" (original "Llamaste" got stuck in aborted state)
- `scripts/ensure-vm.bat` tries both names

---

## Next Steps

### Immediate
1. **More TTS voices** -- Add additional Piper voices to the voice table (e.g. British, other quality levels)
2. **Real two-VM mDNS test** -- Validate mDNS auto-discovery with two physical/VM instances on same subnet (host-only adapter with promiscuous mode)
3. **Real hardware test** -- Boot and validate on a physical x86_64 machine

---

## Build & Boot Summary

| Artifact | Size | Details |
|----------|------|---------|
| llamaste binary | 4.3 MB | Dynamic ELF, x86-64, musl + whisper.cpp + ALSA + espeak-ng + sherpa-onnx + TweetNaCl |
| libonnxruntime.so | 17 MB | ORT v1.24.2, CPU-only, all deps statically linked |
| libsherpa-onnx-c-api.so | 3.5 MB | sherpa-onnx v1.12.28, Piper VITS TTS |
| bzImage kernel | 7.5 MB | Built-in DRM/GPU/audio drivers, no modules |
| rootfs.squashfs | 161 MB | llamaste + ORT + sherpa-onnx + WPEWebKit + Mesa + Wayland + all libs |
| llamaste.img | 611 MB | 5-partition GPT disk image |
| Boot time | ~2 seconds | Kernel -> HTTP server ready |

---

## Source Summary

~11,000 LOC original C++ + ~45KB web UI:
- main.cpp, supervisor.cpp, init.cpp, hwdetect.cpp, child_main.cpp
- agent.cpp, prompt_builder.cpp
- tools.cpp + 13 tool files (fs, process, network, system, config, model, model_download, install, schedule, auth, audio, cluster, update)
- voice.h, voice.cpp, cluster.h, cluster.cpp
- updater.h, updater.cpp, version.h
- tweetnacl.h, tweetnacl.c
- bcrypt.cpp, auth.cpp
- net_mdns.cpp, scheduler.cpp, mcp_server.cpp
- Web UI: index.html, login.html, setup.html, chat.js, dashboard.js, files.js, system.js, notifications.js, install.js, style.css

## Test Suites
12 suites, ~182 tests: hwdetect, tools, agent, integration, http, mdns, auth, inference, model_download, audio (13 tests), cluster (16 tests), updater

## VirtualBox VM
- **VM Name**: "Llamaste2", Location: `D:\Llamaste\vm\Llamaste2\`
- 4 GB RAM, 2 CPUs, EFI64, VMSVGA, AC97 audio, NAT (host 8080 -> guest 80)
- SATA port 0: `llamaste-disk.vdi` (16 GB)
- Access from Windows: `http://localhost:8080`
- Access from WSL2: `http://172.18.208.1:8080`

## Build Commands (WSL2)
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && export FORCE_UNSAFE_CONFIGURE=1 && cd /root/llamaste-build/output && make llamaste-dirclean && make llamaste && make"
```
