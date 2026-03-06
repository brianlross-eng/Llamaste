# Llamaste Project -- Session Status

**Last updated**: 2026-03-08 (Neural TTS end-to-end verified, model picker, test API)

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
| Multi-node integration test | PENDING |

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

---

## Latest Session (2026-03-08) -- Neural TTS End-to-End + Model Picker

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
1. **End-to-end neural TTS test** -- Download amy-low model on VM, verify sherpa-onnx activates and speaks
2. **Multi-node integration test** -- Test auto-offload with 2+ VMs on same network
3. **More TTS voices** -- Add additional Piper voices to the voice table (e.g. British, other quality levels)

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
