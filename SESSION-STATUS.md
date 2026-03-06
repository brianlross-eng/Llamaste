# Llamaste Project -- Session Status

**Last updated**: 2026-03-06 (Phase 4 A/B Updates COMPLETE)

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

---

## Latest Session (2026-03-06) -- Phase 4 A/B Updates

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
1. **Future: Neural TTS** -- Build onnxruntime from source for musl, then enable sherpa-onnx + Piper VITS
2. **Phase 5: Mesh auto-offload** -- Automatic model sharding across discovered cluster peers

---

## Build & Boot Summary

| Artifact | Size | Details |
|----------|------|---------|
| llamaste binary | 4.4 MB | Dynamic ELF, x86-64, musl + whisper.cpp + ALSA + espeak-ng + TweetNaCl |
| bzImage kernel | 7.5 MB | Built-in DRM/GPU/audio drivers, no modules |
| rootfs.squashfs | 156 MB | llamaste + WPEWebKit + Mesa + Wayland + all libs |
| llamaste.img | 611 MB | 5-partition GPT disk image |
| Boot time | ~2 seconds | Kernel -> HTTP server ready |

---

## Source Summary

~10,500 LOC original C++ + ~45KB web UI:
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
12 suites, ~171 tests: hwdetect, tools, agent, integration, http, mdns, auth, inference, model_download, audio, cluster, updater

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
