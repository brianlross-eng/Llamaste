# Llamaste Project -- Session Status

**Last updated**: 2026-03-09 (Model download tools DONE, 128 tests across 9 suites)

---

## Where We Are

### Phase 1: COMPLETE
All 12 tasks + ISO/installer done. 5/5 QEMU E2E tests. EFI boot verified.

### Phase 2: IN PROGRESS — Desktop Mode + Security

| Sub-phase | Tasks | Status |
|-----------|-------|--------|
| 2a: Web UI Redesign | 1-9 | DONE — status bar, tabs, file browser, dashboard, system settings |
| 2b: Heartbeat Scheduler | 10-15 | DONE — scheduler thread, schedule.* tools, SSE notifications, toasts |
| 2c: Desktop Compositor | 16-18 | DONE — kernel DRM, Buildroot packages, compositor launch |
| 2c: QEMU Testing | 19 | DONE — 5/5 server E2E, desktop mode boots, compositor launches |
| 2c: VirtualBox Desktop | — | DONE — Cage+Cog renders web UI, setup flow works, auth works |
| 2d: Real Inference | 20 | DONE — llama-server package, HTTP proxy, lifecycle mgmt |
| 2e: Model Download | — | DONE — 5 tools (recommended, search, files, download, usb_import), dashboard button, libcurl linking |
| Auth + Console | bcrypt, AuthManager, server display | DONE — 128 host tests, 9/9 suites |

**Phase 2 implementation plan**: `docs/plans/2026-03-03-phase2-implementation-plan.md`
**Console + Auth design**: `docs/plans/2026-03-05-console-auth-design.md`
**Console + Auth plan**: `docs/plans/2026-03-05-console-auth-implementation-plan.md`

---

## Auth & Server Console (2026-03-05)

### What was built
1. **bcrypt password hashing** (`bcrypt.h/cpp` ~500 LOC): Full Blowfish/Eksblowfish implementation, bcrypt-specific base64, /dev/urandom salt, constant-time comparison
2. **AuthManager** (`auth.h/cpp` ~350 LOC): Device password (set/verify), session cookies (create/validate/expire), API key (Bearer token), brute force protection (5 failures = 30s cooldown), config persistence to /data/config/device.json
3. **Server console display** (`supervisor.cpp`): VT100 box-drawing status display on /dev/console every 5s — CPU%, RAM, disk, temperature, IP, hostname, model, child PID status
4. **Auth routes + middleware** (`child_main.cpp`): require_auth lambda wrapper, /llamaste/auth/setup, /login, /logout, /status endpoints, session expiry background thread
5. **Login + Setup web pages** (`web/login.html`, `web/setup.html`): Dark-theme first-boot setup wizard + login page
6. **Auth tools** (`tools_auth.cpp`): auth.change_password, auth.get_api_key, auth.set_session_timeout
7. **Host tests** (`tests/test_auth.cpp`): 15 tests covering bcrypt, AuthManager, and console format helpers

### Auth flow
- First boot: no password → serves setup.html → user creates password → API key generated → session cookie set → redirect to main UI
- Subsequent visits: serves login.html → enter password → session cookie → access granted
- API access: `Authorization: Bearer llm-XXXXXXXXXXXXXXXXXXXX`
- Protected routes: all API endpoints. Unprotected: /health, /login.html, /setup.html, static JS/CSS

### Test results
**128 host tests across 9 suites** — ALL PASSING:
1. Hardware Detection (3 tests)
2. Tools System (10 tests)
3. Agent Loop (19 tests)
4. Tools Integration (27 tests — 30 tools registered)
5. HTTP Server (15 tests)
6. Network/mDNS (17+ tests)
7. Auth & Console (15 tests)
8. Inference Integration (10 tests)
9. Model Download (20 tests — URL builders, validators, recommender)

### Bug found and fixed
bcrypt base64 decode table was wrong — built for standard base64 alphabet order but bcrypt uses `./A-Za-z0-9`. Fixed decode table + salt streaming in Eksblowfish key expansion.

---

## Phase 2a-2c Details

### Phase 2a (Tasks 1-9): Web UI Redesign
- index.html: Status bar (clock, model, speed, RAM, IP, notifications), bottom tab nav (Chat/Files/Dashboard/System)
- dashboard.js: Status bar updates + Dashboard tab (CPU/temp/RAM/disk bars, hardware info, network, model)
- files.js: File browser with navigation, preview, upload, new folder
- system.js: System info display + scheduled tasks list
- notifications.js: Toast notification system + SSE connection + badge
- chat.js: Refactored for tabbed layout
- style.css: Complete dark-theme redesign
- child_main.cpp: /llamaste/files endpoint, web asset routes

### Phase 2b (Tasks 10-15): Heartbeat Scheduler
- scheduler.h/cpp: Background thread, cron parser, task CRUD, alert monitoring, persistence
- tools_schedule.cpp: schedule.create/list/delete/update tools
- child_main.cpp: Scheduler wiring, /llamaste/notifications SSE, /llamaste/schedules REST
- notifications.js: SSE EventSource connection, badge management
- system.js: Schedule display with dot indicators, separate fetch

### Phase 2c (Tasks 16-18): Desktop Compositor (source changes done, untested)
- linux.config: DRM (virtio, vbox, bochs, simpledrm, vmwgfx, i915), evdev, mousedev, futex, sysvipc
- defconfig: Cage, Wayland, Mesa (swrast+virgl), Cog+WPEWebKit, libdrm, libinput, eudev, libxkbcommon, pixman
- genimage.cfg: sys-a partition bumped to 256M
- child_main.cpp: TCP poll for server readiness, access() probe for browser, fork/exec cage+cog

---

## Next Steps

### Immediate
1. **Model download feature complete** (2026-03-09) — 5 new tools + dashboard button + REST endpoints:
   - `model.recommended`: RAM-based model recommendation (6 Qwen2.5 tiers)
   - `model.search`: Search Hugging Face for GGUF models
   - `model.files`: List files in a HF repository
   - `model.download`: Download GGUF models from Hugging Face via libcurl
   - `model.usb_import`: Import GGUF models from USB drives (NTFS3 kernel support)
   - Dashboard "Download Recommended Model" button with progress states
   - Buildroot: libcurl+openssl+NTFS3, dynamic linking with static C++ runtime
   - Design doc: `docs/plans/2026-03-08-model-download-design.md`
   - Plan: `docs/plans/2026-03-08-model-download-plan.md`

2. **Desktop mode bugs fixed** (2026-03-08) — 3 issues found and resolved:
   - `execlp` → `execl` with full paths (PID 1 has no PATH)
   - `BR2_ROOTFS_DEVICE_CREATION_DYNAMIC_EUDEV=y` (cage/wlroots need HAS_UDEV)
   - `CONFIG_HYPERVISOR_GUEST=y` in kernel (vmwgfx needs it for VirtualBox VMSVGA)
   - Also added `BR2_PACKAGE_SEATD=y` (wlroots dependency)
   - Squashfs: 73 MB, disk image: 611 MB
   - Desktop mode: Cage+Cog renders full web UI, first-boot setup works

2. ~~**Task 19**: WSL2 Buildroot build + QEMU desktop mode test~~ **DONE**
   - Build fixes: libstdc++ symlinks in sysroot (ICU/C++ linking), --without-icu for libxml2
   - Squashfs: 65 MB (from 5.9 MB — includes WPEWebKit, Mesa, Wayland, Cage, etc.)
   - Server mode: 5/5 E2E tests pass, boots in 2s
   - Desktop mode: boots, HTTP server works, compositor launches (exits gracefully without real display)

2. ~~**Task 20**: Real inference integration~~ **DONE**
   - Design doc: `docs/plans/2026-03-06-inference-integration-design.md`
   - Implementation plan: `docs/plans/2026-03-07-inference-integration-plan.md`
   - Buildroot package for llama-server (llama.cpp b5460, static CPU build)
   - HTTP proxy inference function (llama_inference → localhost:8088)
   - Process lifecycle: spawn, health poll, crash recovery (3 retries)
   - g_inference_fn global swap (stub → real when model loads)
   - Health endpoint reports model_loaded, model_name, inference_ready
   - ~435 lines new code (C++ + Buildroot + tests)

### Build Commands (WSL2)
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && export FORCE_UNSAFE_CONFIGURE=1 && cd /root/llamaste-build/output && make llamaste_x86_64_defconfig && make llamaste-dirclean && make llamaste && make"
```

---

## Build & Boot Summary

| Artifact | Size | Details |
|----------|------|---------|
| llamaste binary | ~1.6 MB | Dynamic ELF, x86-64, musl (static libstdc++/libgcc, dynamic libcurl/liblzma) |
| bzImage kernel | 7.5 MB | Built-in DRM/GPU drivers, evdev, no modules |
| rootfs.squashfs | 75 MB | llamaste + WPEWebKit + Mesa + Wayland + Cage + ICU + libcurl |
| llamaste.img | 611 MB | 5-partition GPT disk image |
| llamaste.iso | ~400 MB (needs rebuild) | Hybrid BIOS+UEFI live ISO with installer |
| Boot time | ~2 seconds | Kernel → HTTP server ready |

---

## Source Summary

~7,500 LOC original C++ + ~40KB web UI:
- main.cpp, supervisor.cpp, init.cpp, hwdetect.cpp, child_main.cpp
- agent.cpp, prompt_builder.cpp
- tools.cpp + 10 tool files (fs, process, network, system, config, model, model_download, install, schedule, auth)
- bcrypt.cpp, auth.cpp
- net_mdns.cpp, scheduler.cpp
- Web UI: index.html, login.html, setup.html, chat.js, dashboard.js, files.js, system.js, notifications.js, install.js, style.css

## Phase 1 Implementation Summary

| Task | Status | Commit | Key Output |
|------|--------|--------|------------|
| 0: WSL2 dev environment | DONE | (setup) | Ubuntu 24.04, gcc 13.3, cmake 3.28, qemu 8.2 |
| 1: Buildroot external tree | DONE | 38678a7 | BR2_EXTERNAL, defconfig, stub.c, package recipe |
| 2: Minimal kernel config | DONE | aff89d6 | 157-line kernel config, no modules, built-in drivers |
| 3: Stock llama-server build | DONE | 2ac8a0f | genimage.cfg, grub.cfg, llamaste.mk, llama.cpp cloned |
| 4: PID 1 supervisor | DONE | 988b9c5 | main.cpp, supervisor.cpp, init.cpp, hwdetect.cpp, child_main.cpp |
| 5: Tools system | DONE | 9f7ed2e | 25 tools across 6 categories, 37 tests |
| 6: Agent loop | DONE | 0cd1d43 + f99e77c | agent.cpp, prompt_builder.cpp, 19 tests |
| 7: Web UI | DONE | 1aab8bf | index.html, chat.js, dashboard.js, style.css, embed_web.cmake |
| 8: HTTP server integration | DONE | 00d3219 | child_main.cpp rewritten, httplib.h, 15 tests |
| 9: Network | DONE | 613cb4b | net_mdns.cpp, kernel DHCP config, 17 tests |
| 10: GRUB config | DONE | b7675ba | Production dual-boot grub.cfg with A/B slot |
| 11: Genimage layout | DONE | 36d52fb | 5-partition GPT, post_build.sh, post_image.sh |
| 12: QEMU test scripts | DONE | 6973c15 | qemu-test.sh, qemu-run.sh, host-test.sh |
| Build fixes | DONE | cfd83a4 + f2f8936 | C++ toolchain, PCI kernel, genimage/post_image fixes |
| ISO + installer | DONE | 49766c8 + dd71dba | Live ISO, installer, INSTALL.md, DEVELOPER.md |
