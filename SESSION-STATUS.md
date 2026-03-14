# Llamaste Project -- Session Status

**Last updated**: 2026-03-14 (Real hardware testing — WiFi, install, model download)

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

## Latest Session (2026-03-14) -- Real Hardware Testing & Fixes

### Hardware Test Results (ASUS VivoBook, i5-1035G1, 36GB RAM)

| Test | Status |
|------|--------|
| USB boot (server-live) | ✅ PASS |
| Install to SATA Toshiba 1TB | ✅ PASS |
| Boot from SATA (server mode) | ✅ PASS (after ip=dhcp fix) |
| Boot from SATA (desktop mode) | ✅ PASS |
| WiFi connect (RTL8821CE) | ✅ PASS (after BSSID fix) |
| DATA partition mount + auto-resize | ✅ PASS — 953 GB ext4 |
| DHCP on installed system | ✅ PASS (dhcpcd-hook binary) |
| Model download (32B sharded) | IN PROGRESS — shard 1 done, shard 2 downloading |

### Bugs Found & Fixed

| # | Bug | Root Cause | Fix | Commit |
|---|-----|-----------|-----|--------|
| 1 | 2-min boot timeout on installed system | `ip=dhcp` in grub.cfg | Removed kernel DHCP param | 6dfa105 |
| 2 | dhcpcd can't run hooks without /bin/sh | Shell-based dhcpcd-run-hooks | C binary replacement (dhcpcd-hook.c) | 1a7ed51 |
| 3 | /var/run read-only on squashfs | Squashfs is immutable | tmpfs mounts on /var/run, /var/db | 1a7ed51 |
| 4 | WiFi 4-way handshake failure (installed) | Stale BSSID in persisted wpa.conf | clear_all_bssids() at startup | 942ebe4 |
| 5 | Model download 404 (HuggingFace) | Sharded GGUF format change | Updated filenames + shard loop | 55016e3 |
| 6 | DATA partition shows 1G (tmpfs fallback) | Mount failure logged only to stderr | rlog diagnostics + /dev/sdb candidates | e065204 |
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
- ⬜ Install button — detection fixed (/cdrom/llamaste-live-iso), needs hardware test
- ⬜ Install to NVMe — next priority after install button confirmed
- ⬜ Desktop mode WiFi connect — untested (server mode confirmed)

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

### Immediate
1. **Test install button** — Flash ISO (95c1979), boot desktop, verify Install tab appears
2. **Install to NVMe** — Once install button confirmed, test actual install flow
3. **DNS fix** — `/etc/resolv.conf` empty after WiFi connect (dhcpcd issue)

### Backlog
- Desktop mode WiFi connect test
- Voice quality tuning
- Real two-VM mDNS test on LAN
- Model auto-download on cluster formation (test on real hardware)
- Dropbear SSH (requires adding a shell binary — deferred)

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
| llamaste.iso | 1.5 GB | Live ISO with GRUB, squashfs pivot, installer |
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
