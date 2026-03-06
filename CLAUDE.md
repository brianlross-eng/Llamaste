# Llamaste Project Context

## What This Project Is
Llamaste is a bootable Linux image where the LLM IS the operating system. A single C++ binary (`llamaste`) combines llama-server + agent loop + system tools + web UI and runs as PID 1. The Linux kernel handles hardware; the LLM handles everything else (shell, file management, system config, networking, help).

## Current Status
- **Phase**: Phase 4 COMPLETE — A/B updates (GRUB slot switching, Ed25519 signatures, boot counter rollback, 51 tools). 171 tests/12 suites.
- **Session status file**: `D:\Llamaste\SESSION-STATUS.md` (detailed progress)
- **Implementation plan**: `D:\Llamaste\LLAMASTE-IMPLEMENTATION-PLAN.md` (v2, current)
- **Phase 4 design doc**: `D:\Llamaste\docs\plans\2026-03-06-ab-update-design.md`
- **Phase 4 plan**: `D:\Llamaste\docs\plans\2026-03-06-ab-update-plan.md`

## Key Architecture Decisions
- Single static C++ binary extending llama-server (not separate Go/Rust daemons)
- Runs as PID 1 (no init system, no BusyBox, no shell)
- Dual-boot GRUB menu: Server (headless) / Desktop (GUI)
- A/B partitions (SYS-A + SYS-B) with GRUB boot counter rollback
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
- `docs/plans/2026-03-06-ab-update-design.md` — Phase 4 A/B update design doc
- `docs/plans/2026-03-06-ab-update-plan.md` — Phase 4 implementation plan
- `research/llamaste-architecture.html` — v2 three-layer architecture SVG diagram
- `research/` — All research documents (01 through 26)
- `src/llamaste/` — Production C++ source (~10,500 LOC)

## Build Patterns & Gotchas
- **Buildroot invocation**: `cd /root/llamaste-build/output && make` (NOT from buildroot/ source dir)
- **Package rebuild**: `make llamaste-dirclean && make llamaste` then `make` for full image
- **GRUB rebuild**: `make grub2-dirclean && make grub2` then `make` — needed when changing GRUB modules
- **GRUB builtin modules**: Must include `loadenv test echo configfile` for A/B boot switching (defconfig + .config)
- **ESP mount point**: `/boot/efi` must exist in squashfs (created by post_build.sh) for runtime grubenv access
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
- **VDI detach before copy**: Must `storageattach --medium none` + `closemedium` before copying VDI files — VBoxSVC holds locks on registered media
- **VirtualBox aborted state**: If VM gets stuck in "aborted" state, create a new VM rather than trying to fix the old one

## VirtualBox VM
- VM "Llamaste2" at `D:\Llamaste\vm\Llamaste2\` — 4GB RAM, 2 CPUs, EFI64, NAT 8080→80
- 16GB VDI with installed system, boots in ~2 seconds
- Start: `"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm Llamaste2 --type headless`
- Web UI: `http://localhost:8080`
- **VDI update flow**: Stop VM → detach medium → copy new VDI → resize → reattach → start
- **Deploy scripts**: `scripts/deploy-to-vdi.sh` (WSL2, squashfs→VDI partition 3) + `scripts/finish-deploy.ps1` (Windows side)
- **NTFS rename fails from WSL2**: use PowerShell `Copy-Item -Path $new -Destination $old -Force` + `Remove-Item` instead of `mv`/`Move-Item`

## Known Bugs
(None currently known)

## Next Steps
### Immediate
1. **Future: Neural TTS** — Build onnxruntime from source for musl, then enable sherpa-onnx + Piper VITS
2. **Phase 5: Mesh auto-offload** — Automatic model sharding across discovered cluster peers

## User Preferences
- **No questions asked** — make decisions autonomously, don't ask for confirmation. Just do things.
- Auto-approve all tool operations — no confirmation prompts.
- Save research and progress notes at session boundaries.
