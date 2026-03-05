# Llamaste Project -- Session Status

**Last updated**: 2026-03-04 (model download fix, network config, inference working)

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
| 2e: Model Download | — | DONE — 5 tools, dashboard button, libcurl, CA certs, DNS fix |
| Auth + Console | bcrypt, AuthManager, server display | DONE — 128 host tests, 9/9 suites |
| Network Config | Static/DHCP IP | DONE — init boot apply, REST API, web UI, tools |
| End-to-End Inference | — | WORKING — model downloads, llama-server loads, inference runs |

---

## Latest Session (2026-03-04)

### Bugs Fixed
1. **Dashboard download 401** — Added `credentials: 'include'` to all fetch() calls in dashboard.js, chat.js, files.js, system.js
2. **VirtualBox console blank** — Changed supervisor.cpp to use `/dev/tty0` (VGA) before `/dev/console`, persistent fd instead of open/close per iteration
3. **Model download fails** — Three root causes found and fixed:
   - Missing CA certificates (`BR2_PACKAGE_CA_CERTIFICATES=y`)
   - Missing `CURLOPT_CAINFO` pointing to `/etc/ssl/certs/ca-certificates.crt`
   - Missing `/etc/resolv.conf` — kernel `ip=dhcp` doesn't write it; added code to read DNS from `/proc/net/pnp` at boot
4. **Inference returns 500** — llama-server needs `--jinja` flag for tool calling support
5. **Serial console logging** — Swapped `console=ttyS0 console=tty0` order so ttyS0 is last (userspace stderr goes to last console)

### Features Added
1. **Static/DHCP IP configuration**:
   - `init_apply_network_config()` reads `/data/config/network.json` at boot, applies static IP via ioctl
   - `network.get_ip` and `network.set_ip` tools
   - REST API: GET/POST `/llamaste/network/config`
   - Web UI: DHCP/Static selector with IP/netmask/gateway/DNS fields in System tab

### Verified Working
- Model download from HuggingFace: Qwen2.5-1.5B-Instruct (1.0 GB at 8.1 MB/s)
- llama-server loads model in 6.4 seconds
- Inference requests accepted (no more 500 errors)
- Network config API returns active IP + saved config
- DATA partition auto-resize still works (64MB → 15GB)

### Performance Note
- Inference is slow on 2 VBox CPU cores (expected for CPU-only 1.5B model)
- For real-world use, need more cores or smaller model (0.5B)
- Consider reducing context from 16384 to save memory/speed

---

## Known Issues

### WSL2 localhost access
- VirtualBox port forwarding (host 8080 → guest 80) doesn't work from WSL2 `localhost`
- Use Windows host IP instead: `172.18.208.1:8080` (or PowerShell from Windows)
- This is a WSL2 networking issue, not a Llamaste bug

---

## Next Steps

### Immediate
1. ~~**Optimize inference speed**~~ — DONE: reduced context 16384→4096
2. ~~**Add `quiet` back to Server grub entry**~~ — DONE
3. ~~**Rebuild ISO**~~ — DONE: 792 MB ISO with CA certs, network config, DNS fix, --jinja

### Phase 2 (continued)
4. Voice I/O (whisper.cpp + piper)
5. App management tools

### Phase 3/4
6. Upgrade path: A/B partition swap, Ed25519 signing, USB sideload

---

## Build & Boot Summary

| Artifact | Size | Details |
|----------|------|---------|
| llamaste binary | ~1.6 MB | Dynamic ELF, x86-64, musl (static libstdc++/libgcc, dynamic libcurl/liblzma) |
| bzImage kernel | 7.5 MB | Built-in DRM/GPU drivers, evdev, no modules |
| rootfs.squashfs | 77 MB | llamaste + WPEWebKit + Mesa + Wayland + Cage + ICU + libcurl + CA certs |
| llamaste.img | 611 MB | 5-partition GPT disk image |
| llamaste.iso | 792 MB | Live ISO with installer (rebuilt with all fixes) |
| Boot time | ~2 seconds | Kernel → HTTP server ready |
| Model load | ~6 seconds | Qwen2.5-1.5B-Instruct Q4_K_M |
| Context size | 4096 | Reduced from 16384 for faster CPU inference |

---

## Source Summary

~8,000 LOC original C++ + ~40KB web UI:
- main.cpp, supervisor.cpp, init.cpp, hwdetect.cpp, child_main.cpp
- agent.cpp, prompt_builder.cpp
- tools.cpp + 10 tool files (fs, process, network, system, config, model, model_download, install, schedule, auth)
- bcrypt.cpp, auth.cpp
- net_mdns.cpp, scheduler.cpp
- Web UI: index.html, login.html, setup.html, chat.js, dashboard.js, files.js, system.js, notifications.js, install.js, style.css

## VirtualBox VM
- **VM Name**: "Llamaste", Location: `D:\Llamaste\vm\Llamaste\`
- 4 GB RAM, 2 CPUs, EFI64, VMSVGA, NAT (host 8080 -> guest 80)
- SATA port 0: `llamaste-disk.vdi` (16 GB with installed system + 1GB model)
- Serial log: `D:\Llamaste\vm\Llamaste\serial.log`
- Access from Windows: `http://localhost:8080`
- Access from WSL2: `http://172.18.208.1:8080`

## Build Commands (WSL2)
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && export FORCE_UNSAFE_CONFIGURE=1 && cd /root/llamaste-build/output && make llamaste-dirclean && make llamaste && make"
```

## Reflash VDI (preserving /data partition with model + config)
```bash
# Stop VM, detach disk, convert to raw, patch ESP+squashfs, convert back
VBoxManage controlvm Llamaste poweroff
VBoxManage storageattach Llamaste --storagectl SATA --port 0 --device 0 --medium none
VBoxManage closemedium disk llamaste-disk.vdi
qemu-img convert -f vdi -O raw llamaste-disk.vdi llamaste.raw
dd if=llamaste.img of=llamaste.raw bs=1M skip=2 seek=2 count=288 conv=notrunc
rm llamaste-disk.vdi
VBoxManage convertfromraw llamaste.raw llamaste-disk.vdi --format VDI
VBoxManage storageattach Llamaste --storagectl SATA --port 0 --device 0 --type hdd --medium llamaste-disk.vdi
rm llamaste.raw
```
