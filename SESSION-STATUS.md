# Llamaste Project -- Session Status

**Last updated**: 2026-03-26 (v0.2.2 -- Kernel 6.12.78 LTS upgrade with PREEMPT_RT)

---

## Where We Are

### 2026-03-26: v0.2.2 — Kernel 6.12.78 LTS Upgrade — ALL TESTS PASS
- Upgraded from 6.6.70 to 6.12.78 LTS
- Enabled PREEMPT_RT for deterministic inference latency
- Enabled DRM panic (text mode — QR needs Rust)
- Enabled Intel Xe DRM driver
- Resolved 16 Kconfig changes (removed/renamed/type-changed options)
- PREEMPT_RT trade-offs: i915 disabled (simpledrm+pixman fallback), THP disabled
- CONFIG_SYSFB_SIMPLEFB=y was the key fix for desktop mode on 6.12+
- **VivoBook hardware test results**: USB boot ✅, server-live ✅, WiFi ✅, install ✅, model load ✅, server mode ✅, desktop-live ✅
- **Desktop mode works on real hardware for the FIRST time**
- EVO-X2: server mode ✅, desktop mode still console-only (separate issue — likely AMD GPU/DRM)
- Wizard emoji replaced with llama emoji on login/setup pages
- Known cosmetic: desktop resolution uses EFI framebuffer res (simpledrm), not native panel
- 9 commits on `kernel-6.12-upgrade` branch

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

### Phase 6: WiFi Support -- COMPLETE (3080038) ✅ CONNECTED ON REAL HARDWARE

| Component | Status |
|-----------|--------|
| wifi.h/cpp: WiFiManager (wpa_supplicant Unix socket control) | DONE (223534c) |
| tools_wifi.cpp: 7 tools (status, scan, connect, disconnect, list, forget, enable) | DONE (223534c) |
| child_main.cpp: spawn wpa_supplicant + dhcpcd, register tools, HTTP endpoints | DONE (223534c) |
| defconfig: wpa_supplicant, dhcpcd, linux-firmware (Intel/Realtek/Qualcomm) | DONE (223534c) |
| linux.config: full WiFi kernel stack (cfg80211, mac80211, iwlwifi, rtw88, ath10k) | DONE (223534c) |
| Web UI: WiFi dashboard card (status, scan results, connect/disconnect UI) | DONE (223534c) |
| tests/test_wifi.cpp: Suite 13 — 37 tests all pass | DONE (223534c) |
| Console WiFi setup (server mode) | DONE (a0c645e) |
| iw-based scanning (replaces wpa_supplicant ctrl socket) | DONE (341113a) |
| iw scan fallback in desktop WiFiManager | DONE (f8300ce) |
| Post-connection verification (20s poll) | DONE (f8300ce) |
| WPA-EAP detection | DONE (f8300ce) |
| Signed regdb disabled (5GHz unblocked) | DONE (b57978a) |
| CRC_CCITT for rt2800 (eliminates symbol spam) | DONE (b57978a) |
| wpa_supplicant spawn-on-demand (desktop) | DONE (b57978a) |
| Remove -B flag, foreground spawn with diagnostics | DONE (89c3e70) |
| -dd debug log capture on failure | DONE (3e9ade3) |
| **CONFIG_PACKET=y** (AF_PACKET for EAPOL frames) | DONE (3080038) |
| **WiFi CONNECTED on bare metal** | ✅ CONFIRMED |

---

## Latest Session (2026-03-25) -- v0.2.0C: Desktop Mode Fixed + Bare Metal Testing

### Bare Metal Testing (VivoBook)
- **Boot fix**: Initramfs embedded in bzImage broke normal boot (CONFIG_INITRAMFS_SOURCE). Fixed with ASCII-only initramfs-init.sh loaded via GRUB initrd.
- **Console WiFi IP**: Never showed on console — hardcoded interface list missed WiFi. Fixed with getifaddrs() dynamic scan showing all IPs.
- **Desktop mode crash**: cage compositor failed — Mesa iris GL driver missing. Fixed by forcing WLR_RENDERER=pixman (software renderer, universal compat).
- **Version display**: About card showed hardcoded "v0.1". Fixed to pull from /llamaste/system/info endpoint.
- **Phase B hardware verified on real hardware**: eudev auto-detection, intel-lpss I2C, touchpad via I2C HID, Bluetooth stack, RTL8821CE WiFi all working.

### Commits
| Commit | Description |
|--------|-------------|
| c775666 | fix: ASCII-only initramfs init script for busybox ash |
| d41f923 | docs: initramfs boot gotchas |
| fbedbbf | feat: dynamic IP display on console + version 0.2.0B |
| b89c5ec | fix: force pixman renderer for desktop mode compositor |
| 1cbd929 | feat: version 0.2.0C — dynamic version display in web UI |

### Backup
- `D:\Llamaste\backups\v0.2.0C\` — ISO (2.1GB), IMG (867MB), source zip (9.6MB)

---

## Previous Session (2026-03-24) -- Phase B HW Compat IMPLEMENTED + Bugfix

### Phase B Hardware Compatibility — IMPLEMENTED & BUILT

All 9 plan steps executed in a single session. Research → design → plan → implement → build → test → deploy.

| Step | Description | Status |
|------|-------------|--------|
| 1 | Kernel config — platform (I2C, HID, IOMMU, ACPI, pinctrl, cpufreq) | DONE (435eb57) |
| 2 | Kernel config — ethernet + WiFi expansion | DONE (435eb57) |
| 3 | Kernel config — GPU (amdgpu=m, nouveau=m, Mesa radeonsi/nouveau) | DONE (435eb57) |
| 4 | Bluetooth (BlueZ, dbus, kernel BT, pairing persistence) | DONE (59cd66b) |
| 5 | eudev auto-detection (replaces hardcoded 80-module list) | DONE (34c9b99) |
| 6 | Firmware expansion + sound (Buildroot linux-firmware selections) | DONE (dfb54e6) |
| 7 | Hardware detection enhancement (BT, touchpad, battery, NICs) | DONE (0cdc530) |
| 8 | Build + deploy + VM verification | DONE |
| 9 | Documentation (CLAUDE.md updated with all gotchas) | DONE (fc10c19) |

### Bugfix: Null Content Inference Crash

- **Bug**: 3B model returns `"content": null` from `/completion` → `.get<std::string>()` throws `json::type_error::302`
- **User saw**: `[Inference error: [json.exception.type_error.302] type must be string, but is null]`
- **Root cause**: Our code, not llama-server. Missing `.is_null()` check in child_main.cpp line 622
- **Fix**: Changed to `.is_string()` check + diagnostic logging + hardened TTS endpoint
- **Commit**: 13709c0

### Build Results
- **rootfs.squashfs**: 259MB (was 170MB, under 350MB budget ✅)
- **llamaste.img**: 867MB
- **bzImage**: 12MB (new built-in configs)
- **ISO**: 2.1GB → `D:\Llamaste\llamaste.iso`
- **VM**: Running at http://localhost:8080, 64 tools, inference ready

### Key Architecture Decisions
- **eudev + kmod** replaces hardcoded 80-module `init_load_modules()` — auto-detection via modalias
- **GPU modules explicitly loaded** before udev trigger (amdgpu, nouveau need firmware from squashfs)
- **udevadm trigger --subsystem-nomatch=input** early, input trigger delayed until Wayland socket exists
- **BlueZ + dbus** for Bluetooth HID (~4.5MB), with supervisor restart monitoring
- **3-tier firmware**: Tier 1 bundled (~50-70MB), Tier 2 downloadable, Tier 3 on-demand
- **IOMMU_DEFAULT_DMA_LAZY** (NOT PASSTHROUGH — avoids silent memory corruption)

### All Commits This Session
| Commit | Description |
|--------|-------------|
| a7a4fc5 | docs: hardware compatibility research + design spec + implementation plan |
| 0d02327 | fix: address spec review findings — 5 critical + 7 important fixes |
| 435eb57 | feat: kernel config — Bluetooth HID support (BT core + vendor HCI drivers) |
| dfb54e6 | feat: Buildroot defconfig — kmod, BlueZ, dbus, GPU firmware, Mesa expansion |
| 34c9b99 | feat: eudev auto-detection replaces hardcoded module loading |
| 0cdc530 | feat: enhanced hardware detection — Bluetooth, touchpad, battery, network |
| 59cd66b | feat: BlueZ config + supervisor restart monitoring for dbus/bluetoothd |
| fc10c19 | docs: update CLAUDE.md with Phase B hardware compatibility gotchas |
| bc4c46d | fix: add pid_t include to init.h (build fix) |
| 41ae3f4 | fix: increase sys-a/sys-b partition size 256M → 384M |
| 24a3aa0 | fix: correct GPU dependency module paths for kernel 6.6.70 |
| 13709c0 | fix: null content crash in inference response parsing |

---

## Previous Session (2026-03-23) -- EVO-X2 Bugfixes + Boot Debugging

### Bugfixes Applied (from BUGFIX-*.md files)
| Bugfix | Status |
|--------|--------|
| DATA partition resize crash on 2TB+ NVMe (init.cpp) | ✅ Applied — safer GPT write with error checks, PMBR clamp to uint32 |
| Desktop mode AMD GPU (linux.config, hwdetect.cpp) | ⚠️ Applied then REVERTED — CONFIG_DRM_AMDGPU=y caused black screen (steals display before firmware available). Needs =m module approach with firmware in overlay |
| DHCP first boot (grub.cfg, child_main.cpp, dhcpcd.conf) | ✅ Applied — ip=dhcp in grub.cfg, dhcpcd.conf timeout 30, fork/exec retry (system() broken w/o /bin/sh) |
| main.cpp tmpfs fallback | ✅ Applied — init_mount_data() failure → tmpfs /data |
| hwdetect.cpp PCI scan | ✅ Applied — fallback GPU detection via PCI bus |

### EVO-X2 Kernel Panic — INVESTIGATING
- **Symptom**: `Kernel Panic - not syncing: Attempting to kill init! exitcode=0x00000`
- **When**: Booting from USB (live ISO) on GMKtec EVO-X2
- **Binary never reaches main()**: Ultra-early `write()` diagnostic (`[INIT] BINARY STARTED`) never appears
- **Exit code 0x00000**: Binary exits cleanly before main() — not a crash/signal
- **All shared libs present in ISO**: liblzma, libcurl, libasound, libflite*, libespeak-ng, libsherpa-onnx-c-api, libc — all verified
- **No RPATH/RUNPATH**: Binary relies on musl default paths (/lib, /usr/lib) — all correct
- **Works in QEMU**: Same ISO boots fine in QEMU emulation
- **Prime suspect**: ONNX Runtime global constructor (libonnxruntime.so.1 loaded via libsherpa-onnx-c-api.so) — ORT does CPU feature detection at load time
- **Diagnostic ISO built**: 3rd GRUB entry "Diagnostic - Kernel Test (static init)" — tiny 18KB static binary, no shared libs
- **Next step**: User boots diagnostic entry. If static init works → problem is in shared libs (likely ORT). If static init also panics → kernel config issue

### Files Modified
- `br2-external/board/llamaste/grub.cfg` — ip=dhcp added to both entries
- `br2-external/board/llamaste/grub-live.cfg` — loglevel=7 panic=30 + diagnostic entry
- `br2-external/board/llamaste/linux.config` — AMDGPU removed (was causing black screen)
- `br2-external/board/llamaste/overlay/etc/dhcpcd.conf` — timeout 30, reboot 10
- `src/llamaste/child_main.cpp` — DHCP retry fork/exec (replaced broken system() call)
- `src/llamaste/init.cpp` — GPT resize error handling, PMBR uint32 clamp
- `src/llamaste/main.cpp` — init_mount_data() bool check + tmpfs fallback + early diagnostics
- `src/llamaste/hwdetect.cpp` — PCI bus GPU scan fallback

---

## Previous Session (2026-03-14) -- Version 0.2.0a, 14B Model Testing, Roadmap Expansion

### Version 0.2.0a
- Version bumped to 0.2.0a (version.h authoritative)
- 14B model tested on bare metal (ASUS VivoBook, 36GB RAM)
- tok/s wired up from llama-server timings
- ALSA auto-unmute added (alsa-utils + init_setup_audio)
- Download button always visible
- Roadmap expanded: self-learning skills, skill marketplace, peer skill sharing, multi-user auth

### Bare Metal Hardware Testing (ASUS VivoBook, i5-1035G1, 36GB RAM, Toshiba 1TB SATA)

| Test | Status |
|------|--------|
| USB boot (server-live) | ✅ PASS |
| Install to SATA Toshiba 1TB | ✅ PASS |
| Boot from SATA (server mode) | ✅ PASS |
| Boot from SATA (desktop mode) | ✅ PASS |
| WiFi connect (RTL8821CE) | ✅ PASS |
| DATA partition auto-resize | ✅ PASS — 14.5/901.8 GB |
| DHCP on installed system | ✅ PASS |
| 3B model download + inference | ✅ PASS — ~14 tok/s |
| 14B model download (3 shards) | ✅ PASS — 8.2 GB loaded |
| 14B inference | ✅ PASS — 2.0-2.2 tok/s, correct reasoning |
| NVMe boot speed | ✅ PASS — noticeably faster than SATA |
| Shutdown/reboot | ✅ PASS |
| SVG status bar icons | ✅ PASS |
| Desktop mode | ✅ PASS — compositor, keyboard, LLM working |
| Microphone (desktop) | ❌ FAIL — cog/WPE lacks getUserMedia support |
| Speakers (desktop) | 🔧 ALSA auto-unmute added, untested |

### DATA Partition Auto-Resize Fix
- **Root cause**: Static device name candidates (`/dev/sda5`, `/dev/sdb5`, etc.) missed the actual device on this hardware
- **Fix**: Dynamic `/sys/block/` scanning to find the correct partition at runtime
- **Result**: DATA partition now correctly resized — 14.5/901.8 GB (previously falling back to 1.0 GB tmpfs)

### Web UI Improvements
- Notification history panel with review/clear functionality
- SVG icons for voice and connection status indicators
- Model picker layout cleanup
- Download button always visible (not hidden when model loaded)

### New Debug Endpoints
- `/debug/block-devices` — shows all block devices + partitions + mounts

### Bugs Found & Fixed (Previous 2026-03-14 Session)

| # | Bug | Root Cause | Fix | Commit |
|---|-----|-----------|-----|--------|
| 1 | 2-min boot timeout on installed system | `ip=dhcp` in grub.cfg | Removed kernel DHCP param | 6dfa105 |
| 2 | dhcpcd can't run hooks without /bin/sh | Shell-based dhcpcd-run-hooks | C binary replacement (dhcpcd-hook.c) | 1a7ed51 |
| 3 | /var/run read-only on squashfs | Squashfs is immutable | tmpfs mounts on /var/run, /var/db | 1a7ed51 |
| 4 | WiFi 4-way handshake failure (installed) | Stale BSSID in persisted wpa.conf | clear_all_bssids() at startup | 942ebe4 |
| 5 | Model download 404 (HuggingFace) | Sharded GGUF format change | Updated filenames + shard loop | 55016e3 |
| 6 | DATA partition shows 1G (tmpfs fallback) | Static device name candidates missed actual device | Dynamic /sys/block/ scanning | e065204 |
| 7 | Download UI stuck with no progress | Synchronous HTTP (30+ min block) | Async download + progress polling API | b60a8ab |
| 8 | Only "Download Recommended" button | No model size picker | Tier selector dropdown + download-tier API | bbf990e |
| 9 | Shard-1-only download check | Incomplete shards reported as done | Check all N shards exist | 0f2a08b |

### Commits This Session

| Commit | Description |
|--------|-------------|
| 1a7ed51 | feat: add dhcpcd-hook C binary and fix read-only squashfs mounts |
| 6dfa105 | fix: remove ip=dhcp from grub.cfg to prevent 2-min boot timeout |
| 55016e3 | feat: support HuggingFace sharded GGUF model downloads |
| e065204 | fix: improve DATA partition mount diagnostics and add sdb candidates |
| 942ebe4 | fix: clear stale BSSID hints to prevent WiFi 4-way handshake failures |
| b60a8ab | feat: async model download with progress polling |
| bbf990e | feat: add model tier selector and download-tier endpoint |
| 0f2a08b | fix: check all shards exist before reporting model as downloaded |

---

## Previous Session (2026-03-13) -- QEMU Integration Test Suite

### Test Suite Implementation (a02e022 - 6d2b10b)

Built automated QEMU-based integration test suite — 7 bash scripts + 1 C++ debug endpoint.
All 44 assertions pass in `run-all-tests.sh --quick` mode.

| Test | Assertions | Status |
|------|-----------|--------|
| install | 10 | ✅ PASS — ISO boot → disk detect → install → reboot → health |
| cluster | 25 | ✅ PASS — 2-node mesh, roles, add-peer, capacity, models, peers, reload |
| update | 8 | ✅ PASS — status, version, check, rollback, post-rollback, error handling |
| recovery | 1 (3 skip) | ✅ PASS — debug API available, child kill skipped (no model) |
| model | -- | SKIP (--quick mode) |

Key design decisions:
- Per-test port ranges (20-port windows) to avoid WSL2 TIME_WAIT collisions
- Shared test library with `pass()`/`fail()`/`skip()` assertion framework
- `tools_debug.cpp` adds `debug.kill_child` endpoint for recovery testing
- Sequential execution only — `pkill -f qemu-system-x86_64` cleanup kills ALL QEMUs

### Commits This Session (Test Suite)

| Commit | Description |
|--------|-------------|
| a02e022 | feat: add shared QEMU test library (qemu-test-lib.sh) |
| ac8b0ce | feat: add install flow integration test (v2 with assertions) |
| 6a4302d | feat: add A/B update status integration test |
| 8aedf5b | feat: add multi-node cluster integration test |
| e332e64 | feat: add model download integration test |
| d16242e | feat: add watchdog recovery integration test |
| b28c2d1 | feat: add master test runner (run-all-tests.sh) |
| 64f308a | fix: ISO boot networking and sequential test port collisions |
| c3dd16e | chore: remove temporary verify-wsl-env.sh diagnostic script |
| 0e4d230 | fix: use bash array for drive_args in boot_qemu_iso |
| 6d2b10b | fix: increase cluster test health timeout to 180s |

---

## Previous Session (2026-03-13) -- USB Ethernet, DNS, Dashboard, PXE Boot Server

### USB Ethernet Dongle Support (47255f2 - 1b269df)

Added wired ethernet auto-DHCP for USB dongles. Scans /sys/class/net, filters by
ARPHRD_ETHER type (skips SIT tunnels), brings interface UP via ioctl, waits for
carrier, spawns dhcpcd. Fixed 4 bugs during hardware testing:

| Issue | Root Cause | Fix | Commit |
|-------|-----------|-----|--------|
| sit0 tunnel false positive | Type 776 picked up as ethernet | Check `/sys/class/net/<iface>/type` == 1 | 57b0bcf |
| RTL8153B firmware missing | Not in overlay | Copied from linux-firmware-20240115 | 57b0bcf |
| phylink module failed | CONFIG_PHYLINK not set | Added CONFIG_PHYLINK=m + module load | 57b0bcf |
| cdc_mbim module failed | CONFIG_USB_WDM not set | Added CONFIG_USB_WDM=m + module load | 57b0bcf |

### DNS Resolution Fix (f626579)

resolv.conf always empty because dhcpcd hooks (shell scripts) require /bin/sh
which doesn't exist (BR2_SYSTEM_BIN_SH_NONE=y). Fixed by writing DNS directly
from C++: parses /proc/net/route for gateway IP, writes gateway + 8.8.8.8 + 1.1.1.1.

### Dashboard Network Card (1b269df)

Status endpoint now returns `networks` JSON array with all UP interfaces (name, type,
IP, state). Dashboard renders them dynamically with WiFi/wired icons.

### PXE Boot Server (4ef4fb9) -- NO MORE USB FLASHING

Set up Alpine Linux VM as PXE server on isolated 10.0.50.0/24 ethernet segment:

| Component | Details |
|-----------|---------|
| VM | DHCP-Server (Alpine 3.21, 2GB RAM, bridged + NAT) |
| DHCP | dnsmasq on eth0 (10.0.50.1), range .100-.200 |
| TFTP | Custom-built iPXE EFI binary (ipxe.efi) + autoexec.ipxe |
| HTTP | lighttpd serving llamaste.iso on port 80 |
| SSH | Port 2222 NAT, key auth (/tmp/pxe_key) |
| Boot flow | UEFI PXE -> DHCP -> ipxe.efi (TFTP) -> autoexec.ipxe -> sanboot ISO (HTTP) |

Key gotchas discovered:
- VirtualBox bridged adapter needs **promiscuous mode = allow-all** for DHCP broadcasts
- Test laptop PXE is **UEFI (Arch:00007)**, not BIOS — needs ipxe.efi not undionly.kpxe
- iPXE embedded scripts don't work reliably — use `autoexec.ipxe` via TFTP instead
- Custom iPXE must be built on Alpine (`apk add make gcc musl-dev perl xz-dev`)

Deploy new ISO: `scp -i /tmp/pxe_key -P 2222 llamaste.iso root@127.0.0.1:/data/http/`

### Commits This Session

| Commit | Description |
|--------|-------------|
| 47255f2 | feat: USB ethernet dongle support + wired auto-DHCP |
| 57b0bcf | fix: USB ethernet refinements — sit0 filter, RTL8153B firmware, phylink |
| f626579 | fix: write DNS resolv.conf directly (dhcpcd hooks need /bin/sh) |
| 1b269df | feat: dashboard network card shows all interfaces with IPs |
| 4ef4fb9 | feat: add PXE boot server and deploy script |

---

## Confirmed Working on Real Hardware

- ✅ USB Ethernet dongle (RTL8153B r8152 driver) — auto-DHCP, gigabit
- ✅ Dual-homed: WiFi (172.30.2.x) + Ethernet (10.0.50.x) simultaneously
- ✅ PXE boot over USB ethernet — UEFI iPXE sanboot of ISO over HTTP
- ✅ DNS resolution (gateway + Google + Cloudflare fallback)
- ✅ Dashboard shows all network interfaces with IPs
- ✅ Debug endpoints accessible via Bearer token over ethernet

---

## Previous Session (2026-03-12) -- WiFi Connection Finally Working

### The 8-Issue WiFi Debugging Journey

WiFi connection on real hardware required fixing 8 separate issues across kernel config,
wpa_supplicant spawn, regulatory domain, and driver support:

| # | Issue | Root Cause | Fix | Commit |
|---|-------|-----------|-----|--------|
| 1 | wpa_cli missing | defconfig vs .config mismatch | Rebuild with correct config | aed4327 |
| 2 | wireless-regdb missing | Same defconfig issue | Same fix | aed4327 |
| 3 | No regulatory domain | cfg80211 can't load regdb before pivot | `iw reg set US` + `country=US` in wpa.conf | aed4327 |
| 4 | Scan timing too fast | RTL8821CE needs 2-4s firmware init | Retry loop, extended waits | a5e60ac |
| 5 | wpa_supplicant scan unreliable | ctrl socket approach flaky | Replaced with `iw dev scan` | 341113a |
| 6 | Post-scan connectivity gaps | No verification, no desktop fallback | Poll WPA state 20s, iw fallback | f8300ce |
| 7 | Signed regdb blocks 5GHz | `CONFIG_CFG80211_REQUIRE_SIGNED_REGDB=y` default | Disabled signed regdb, added CRC_CCITT | b57978a |
| 8 | **AF_PACKET missing** | `CONFIG_PACKET` not in linux.config | Added `CONFIG_PACKET=y` | 3080038 |

### Key Diagnostic Breakthrough

Issue #8 was invisible for weeks. wpa_supplicant exited with code 255 (main returns -1)
but printed NO error output. Three diagnostic iterations to find it:

1. **89c3e70**: Removed `-B` flag (internal double-fork hid errors). Saw exit code 255 but no output.
2. **3e9ade3**: Added `-dd -f /tmp/wpa_supplicant.log` + log dump. Saw the actual error:
   `l2_packet_init: socket(PF_PACKET): Address family not supported by protocol`
3. **3080038**: Added `CONFIG_PACKET=y` to linux.config. **WiFi connected.**

The "no error output" was because Buildroot's wpa_supplicant is built with
`CONFIG_NO_STDOUT_DEBUG` which routes all wpa_printf to syslog. Since Llamaste has no
syslog daemon (PID 1 is the LLM), all messages vanished. The `-f` flag bypasses this.

### Commits This Session

| Commit | Description |
|--------|-------------|
| 89c3e70 | Remove -B from wpa_supplicant spawn, add diagnostics |
| 3e9ade3 | Capture wpa_supplicant debug log on failure (exit code 255) |
| 3080038 | Enable CONFIG_PACKET=y for wpa_supplicant EAPOL frames |

---

## Confirmed Working on Real Hardware (Intel Core Ultra 9 275HX)

- ✅ Squashfs live pivot (`/llamaste-live-iso` marker file guard)
- ✅ Keyboard input in desktop mode (udevadm `--action=add --subsystem-match=input`)
- ✅ LLM answers questions (desktop mode)
- ✅ WiFi hardware detected: RTL8821CE binds, firmware loads
- ✅ Console WiFi setup prompt works (server mode)
- ✅ WiFi scan via `iw dev wlan0 scan` — finds networks
- ✅ Desktop mode WiFi scan via iw fallback
- ✅ **WiFi CONNECTED** — wpa_supplicant + EAPOL + WPA handshake working (3080038)
- ✅ Desktop mode auto-launches cog fullscreen (labwc + C++ fork/exec)
- ✅ GRUB simplified to 2 entries (server + desktop)
- ✅ WiFi scan finds networks in both server and desktop mode
- ✅ Install to SATA — working on Toshiba 1TB
- ✅ DATA partition auto-resize — 14.5/901.8 GB (dynamic /sys/block/ scanning)
- ✅ 3B model download + inference — working on bare metal
- ✅ Shutdown/reboot — working
- ✅ SVG status bar icons — working
- ⬜ Desktop mode — testing in progress
- ⬜ 7B/14B model on 36GB RAM — next test
- ⬜ A/B update on real hardware — untested

---

## Latest Session (2026-03-12) — Four Immediate Items

### Completed (4e49c1d)

| Item | Approach | Details |
|------|----------|---------|
| NVMe install docs | INSTALL.md update | Added NVMe device naming, BIOS tips (AHCI, RST, Secure Boot) |
| Remote access | HTTP debug endpoints | 6 auth-protected endpoints: `/debug/{logs,wpa,dmesg,modules,network,sysinfo}` |
| Grammar-constrained JSON | `response_format` in agent.cpp | `json_object` type added when tools present — llama-server converts to GBNF |
| Public GitHub repo | README + LICENSE + push | README.md, Apache 2.0 LICENSE, pushed to brianlross-eng/Llamaste |

Design doc: `docs/plans/2026-03-12-four-items-design.md`

---

## Latest Session (2026-03-12) — Hardware Testing + Install Button Fix

### Real Hardware Testing (ASUS VivoBook X712JA)
Iterative build-test cycles on real hardware uncovered and fixed several issues:

| Issue | Root Cause | Fix | Status |
|-------|-----------|-----|--------|
| Too many GRUB options | 4+ entries confusing | Simplified to 2 (server + desktop) | ✅ |
| Desktop blank screen | labwc autostart needs /bin/sh (none on system) | C++ fork+exec cog via post_ready_fn | ✅ |
| Desktop not fullscreen | COG_PLATFORM_WL_VIEW_FULLSCREEN not inherited | setenv() in fork child before exec | ✅ |
| WiFi 0 networks | `iw` binary missing (defconfig/.config mismatch) | `make llamaste_x86_64_defconfig` | ✅ |
| cfg80211 regdb failure | Built-in cfg80211 loads before squashfs pivot | `iw reg reload` + `iw reg set US` | ✅ |
| Install button missing | `/run/llamaste-live` destroyed by fresh tmpfs | Changed to `/cdrom/llamaste-live-iso` | 🔧 Needs test |

### Commits This Session

| Commit | Description |
|--------|-------------|
| f8ff342 | Fix json_object response_format, fix README static claim |
| 39d5546 | Update session status with four-items completion |
| 4e49c1d | Debug endpoints, grammar JSON, README, LICENSE, NVMe docs |
| 95c1979 | Install button detection, desktop auto-launch, GRUB simplification |

---

## Next Steps

### Immediate — Phase B Hardware Compatibility (IN PROGRESS)
1. **Step 1**: Kernel config — platform support (I2C, HID, IOMMU, ACPI, pinctrl, cpufreq)
2. **Step 2**: Kernel config — ethernet + WiFi expansion
3. **Step 3**: Kernel config — GPU drivers (amdgpu=m, nouveau=m, xe=m) + Mesa (radeonsi, nouveau, llvmpipe)
4. **Step 4**: Bluetooth (BlueZ, dbus, kernel BT modules, pairing persistence)
5. **Step 5**: eudev auto-detection (replace hardcoded init_load_modules)
6. **Steps 6-9**: Firmware, hwdetect, testing, docs

### After Phase B
- **Phase C**: Kernel 6.12 LTS upgrade (WiFi 7, Intel Xe, Thunderbolt, webcam)
- **Audio fix (mic/speaker)** — Native C++ audio bypass for desktop mode
- **EVO-X2 boot diagnostic** — Static init test to isolate kernel vs binary/libs

### Roadmap (Future Phases)
- **Phase C (Skills)**: Self-learning skills — `/data/skills/` loader, LLM writes own skill files
- **Phase D: Skill marketplace** — Remote skill repo, `skill.search`, `skill.install`
- **Phase E: Peer skill sharing** — Cluster nodes sync skills on join
- **Phase F: Multi-user auth** — User accounts, roles (admin/user/guest), per-user history (v1.1/v2.0)
- **Peer-to-peer model transfer** — Nodes serve GGUF shards to new cluster members

---

## Known Issues

### WSL2 localhost access
- VirtualBox port forwarding doesn't work from WSL2 `localhost`
- Use `172.18.208.1:8080` instead

### VirtualBox VM renamed
- VM is now "Llamaste2" (original "Llamaste" got stuck in aborted state)
- `scripts/ensure-vm.bat` tries both names

### wpa_supplicant silent errors
- Buildroot wpa_supplicant uses CONFIG_NO_STDOUT_DEBUG → output goes to syslog
- No syslog daemon in Llamaste → errors vanish
- Workaround: `-dd -f /tmp/wpa_supplicant.log` captures debug output
- Consider adding klogd or simple syslog receiver

---

## Build & Boot Summary

| Artifact | Size | Details |
|----------|------|---------|
| llamaste binary | 4.3 MB | Dynamic ELF, x86-64, musl + whisper.cpp + ALSA + espeak-ng + sherpa-onnx + TweetNaCl |
| libonnxruntime.so | 17 MB | ORT v1.23.2, CPU-only, all deps statically linked |
| libsherpa-onnx-c-api.so | 3.5 MB | sherpa-onnx v1.12.28, Piper VITS TTS |
| bzImage kernel | ~10 MB | CONFIG_MODULES=y, ~60 WiFi modules, CONFIG_PACKET=y |
| rootfs.squashfs | ~170 MB | llamaste + ORT + sherpa-onnx + WPEWebKit + Mesa + Wayland + all libs |
| llamaste.iso | 1485 MB | Live ISO with GRUB, squashfs pivot, installer (v0.2.0) |
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
13 suites, ~182 tests: hwdetect, tools, agent, integration, http, mdns, auth, inference, model_download, audio (13 tests), cluster (16 tests), updater, wifi (37 tests)

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
