# Llamaste Project -- Session Status

**Last updated**: 2026-03-12 (WiFi CONNECTED on real hardware — CONFIG_PACKET=y fix — 3080038)

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

## Latest Session (2026-03-12) -- WiFi Connection Finally Working

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
- ⬜ Install to NVMe — next priority
- ⬜ Desktop mode WiFi connect — untested (server mode confirmed)

---

## Next Steps

### Immediate
1. **Install to NVMe** — Boot live → Install tab → stop wearing out flash drives
2. **Remote access** — dropbear SSH or debug HTTP endpoint for easier diagnosis
3. **Grammar-constrained tool JSON** — Add JSON schema to inference requests (speed + reliability)
4. **Public GitHub repo** — Clean up, write README, publish

### Backlog
- Desktop mode WiFi connect test
- Voice quality tuning
- Real two-VM mDNS test on LAN
- Model auto-download on cluster formation (test on real hardware)

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
