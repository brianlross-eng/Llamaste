# Llamaste Project Context

## What This Project Is
Llamaste is a bootable Linux image where the LLM IS the operating system. A single C++ binary (`llamaste`) combines llama-server + agent loop + system tools + web UI and runs as PID 1. The Linux kernel handles hardware; the LLM handles everything else (shell, file management, system config, networking, help).

## Current Status
- **Phase**: Phase 3a COMPLETE — Voice I/O (STT+always-listening+TTS+Web UI), MCP server (44 tools), mDNS DNS-SD, proactive SSE notifications all done. 138 tests/10 suites.
- **Session status file**: `D:\Llamaste\SESSION-STATUS.md` (detailed progress)
- **Implementation plan**: `D:\Llamaste\LLAMASTE-IMPLEMENTATION-PLAN.md` (v2, current)
- **Phase 1 build plan**: `D:\Llamaste\docs\plans\2026-02-26-phase1-implementation-plan.md` (12 tasks, done)
- **Phase 1 design doc**: `D:\Llamaste\docs\plans\2026-02-26-phase1-implementation-design.md`
- **Console + Auth design**: `D:\Llamaste\docs\plans\2026-03-05-console-auth-design.md`
- **Console + Auth plan**: `D:\Llamaste\docs\plans\2026-03-05-console-auth-implementation-plan.md`

## Key Architecture Decisions
- Single static C++ binary extending llama-server (not separate Go/Rust daemons)
- Runs as PID 1 (no init system, no BusyBox, no shell)
- Dual-boot GRUB menu: Server (headless) / Desktop (GUI)
- musl libc, Buildroot build system, squashfs + ext4 partitions
- Qwen2.5-Instruct models (Apache 2.0), auto-selected by RAM tier
- Tools are compiled into the binary (function calling, not arbitrary code exec)
- Web UI is vanilla JS + SSE, embedded into the binary at compile time
- Desktop mode uses Cage/Labwc Wayland compositor

## Speed Optimization Strategy (Priority Order)
1. KV cache reuse for system prompt (free, built into llama.cpp)
2. Grammar-constrained tool output (100x faster JSON)
3. Semantic cache for common queries (<10ms)
4. N-gram speculative decoding (1.8-2.5x speedup, no draft model)
5. Dual-model strategy — tiny model for tool dispatch, big model for reasoning (Phase 2)
6. Thread tuning — optimize latency not throughput
7. Hardware-specific SIMD dispatch (build flag)

## Research
26 research documents in `D:\Llamaste\research\` covering:
Buildroot, llama.cpp internals, bootable images, CPU optimization, mesh clustering, appliance patterns, model selection, GPU+RAM hybrid, CPU speed projects, API development, MCP servers, minimal OS alternatives, syscall surface analysis, LLM-as-OS paradigm, LLM speed optimization, licensing analysis, failure modes & recovery, multi-user auth, privacy & data security, update mechanisms, power management, competitor UX analysis, LoRA & fine-tuning, LLM speed benchmarks, ISO image construction & boot architecture, custom distro build patterns.

## Important Files
- `LLMOS-Brainstorm.docx` — Original concept (binary, use Python to extract text)
- `LLAMASTE-IMPLEMENTATION-PLAN.md` — v2 master plan with all four phases
- `SESSION-STATUS.md` — Detailed progress tracker with next steps
- `INSTALL.md` — User installation guide
- `DEVELOPER.md` — Technical reference (architecture, API, tools, build, boot)
- `llamaste.iso` — 964 MB live ISO with installer (built from scripts/build-iso.sh)
- `docs/plans/2026-02-26-phase1-implementation-plan.md` — Phase 1 build plan (complete)
- `docs/plans/2026-02-26-phase1-implementation-design.md` — Phase 1 design doc
- `docs/plans/2026-03-05-console-auth-design.md` — Auth + console design doc
- `docs/plans/2026-03-05-console-auth-implementation-plan.md` — Auth + console build plan
- `research/llamaste-architecture.html` — v2 three-layer architecture SVG diagram
- `research/` — All research documents (01 through 26)
- `docs/plans/2026-03-08-model-download-design.md` — Model download design doc
- `docs/plans/2026-03-08-model-download-plan.md` — Model download implementation plan
- `src/llamaste/` — Production C++ source (~9,000 LOC)

## Build Patterns & Gotchas
- **Buildroot invocation**: `cd /root/llamaste-build/output && make` (NOT from buildroot/ source dir)
- **Package rebuild**: `make llamaste-dirclean && make llamaste` then `make` for full image
- **Raw string literals**: Use `R"json(...)json"` delimiter, NOT `R"(...)"` — bare `)"` inside JSON breaks the parser
- **Tool registration**: Use designated initializers (`.name = ..., .handler = ...`) — matches existing tools_*.cpp pattern
- **Declaration order in child_main.cpp**: globals must appear before functions that use them
- **Linux includes**: `mount()`/`umount()` need `#include <sys/mount.h>`, guard with `#ifndef _WIN32`
- **ISO build**: `scripts/build-iso.sh /root/llamaste-build` — needs `grub-pc-bin grub-efi-amd64-bin xorriso mtools`
- **QEMU E2E tests**: `scripts/qemu-boot-test.sh /root/llamaste-build/output/images/llamaste.img`
- **EFI boot test**: `scripts/test-efi-boot.sh` (QEMU + OVMF)
- **Install flow test**: `scripts/qemu-install-test.sh` (ISO boot → install → verify → reboot)
- **ESP filesystem**: Do NOT force FAT32 on <512MB volumes — use auto-select (FAT16 for 32MB per UEFI spec)
- **Partition alignment**: Always `align = 1M` in genimage.cfg for EFI compatibility
- **PMBR updates**: After GPT resize, must update Protective MBR size (offset 458) and CHS end (offsets 451-453)
- **Installer is pure C++**: No shell available (`BR2_SYSTEM_BIN_SH_NONE=y`), all disk ops via open/read/write/pread/pwrite, fork/execv
- **Buildroot ICU/C++ linking**: ICU (C++) is in sysroot but libstdc++.so isn't. Fix: symlink libstdc++ into sysroot via `ICU_POST_INSTALL_STAGING_HOOKS` in external.mk. Also `LIBXML2_CONF_OPTS += --without-icu`. `LIBS="-lstdc++"` did NOT work.
- **Dynamic linking for libcurl**: Full-static (`-static`) doesn't work with libcurl due to transitive deps (nghttp2→libpsl→ICU→libstdc++). Solution: `LLAMASTE_STATIC=OFF` + `-static-libgcc -static-libstdc++` (static C++ runtime, dynamic libcurl). Post-build installs libstdc++.so/libgcc_s.so for ICU.
- **libcurl CMake detection**: Use `find_library(CURL_LIB NAMES curl)` for shared, or `pkg-config --libs --static libcurl` for full static chain. `CURL_STATICLIB` define needed for static.
- **bcrypt alphabet**: bcrypt base64 is `./A-Za-z0-9` NOT standard `A-Za-z0-9+/`. Decode table must match.
- **Eksblowfish salt streaming**: Salt index must be continuous across P-array and S-box expansion (not reset to 0 for S-boxes)
- **BLKPG vs BLKRRPART**: `BLKRRPART` ioctl is unreliable for partitions already visible to kernel. Use `BLKPG_RESIZE_PARTITION` (`<linux/blkpg.h>`) to directly update a specific partition in the kernel's in-memory table.
- **DATA partition auto-resize**: init.cpp grows GPT partition 5 at boot (pure C++ GPT manipulation), then ext4 online resize via `EXT4_IOC_RESIZE_FS` ioctl on mounted filesystem. Idempotent — skips on subsequent boots.
- **Dashboard auth bug**: Web UI fetch() calls to protected API endpoints need `credentials: 'include'` or the auth middleware returns 401. Currently affects model download button.

## VirtualBox VM
- VM "Llamaste" at `D:\Llamaste\vm\Llamaste\` — 4GB RAM, 2 CPUs, EFI64, NAT 8080→80
- 16GB VDI with installed system, boots in ~1 second
- Start: `"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm Llamaste --type gui`
- Web UI: `http://localhost:8080`
- **VDI update scripts**: `scripts/deploy-to-vdi.sh` (WSL2, converts VDI→raw→dd partition→new VDI) + `scripts/finish-deploy.sh` (PowerShell copy step)
- **VBoxHeadless locks VDI**: must stop VM first — check: `powershell.exe -NoProfile -Command "tasklist | Select-String VBox"`, stop: `VBoxManage controlvm Llamaste poweroff`
- **UUID fix after VDI replace**: `VBoxManage internalcommands sethduuid "D:\Llamaste\vm\Llamaste\llamaste-disk.vdi" "144eeb0b-4df1-4213-ab6e-ac0c3ed35bf0"` — VDI registered UUID must match header
- **NTFS rename fails from WSL2**: use PowerShell `Copy-Item -Path $new -Destination $old -Force` + `Remove-Item` instead of `mv`/`Move-Item`

## Known Bugs
(None currently known)

## Next Steps
### Immediate
1. **Phase 3: Mesh clustering** — UDP multicast discovery, llama-rpc-server, coordinator election
2. **Phase 4: A/B updates** — SYS-B partition reserved, GRUB `llamaste_slot` in design (see research/20)
3. **Upgrade Flite TTS** — replace `cmu_us_kal` 8kHz voice with sherpa-onnx or Piper for quality

## User Preferences
- **No questions asked** — make decisions autonomously, don't ask for confirmation. Just do things.
- Auto-approve all tool operations — no confirmation prompts.
- Save research and progress notes at session boundaries.
