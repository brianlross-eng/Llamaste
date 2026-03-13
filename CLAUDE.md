# Llamaste Project Context

## What This Project Is
Llamaste is a bootable Linux image where the LLM IS the operating system. A single C++ binary (`llamaste`) combines llama-server + agent loop + system tools + web UI and runs as PID 1. The Linux kernel handles hardware; the LLM handles everything else (shell, file management, system config, networking, help).

## Current Status
- **Phase**: ✅ WiFi CONNECTED + PXE boot nearly working (571fe87). USB ethernet found, static IP deployed, awaiting squashfs download test. 62 tools, 13 suites.
- **AVX2 SIMD**: GGML_NATIVE=ON → ~14 tok/s on 1.5B Q4_K_M (was 0.028 tok/s, ~500x speedup).
- **Neural TTS**: End-to-end verified — sherpa-onnx Piper VITS synthesizes speech on VDI. 20 voices (en_US/en_GB/en_AU).
- **Multi-node**: Integration test PASSED — 2 VMs cluster correctly (election, capacity, tensor-split).
- **Recovery hardening**: Dead-peer crash recovery 239s → 10s (fresh monitor thread per topology spawn).
- **Session status file**: `D:\Llamaste\SESSION-STATUS.md` (detailed progress)
- **Implementation plan**: `D:\Llamaste\LLAMASTE-IMPLEMENTATION-PLAN.md` (v2, current)
- **Phase 5 design doc**: `D:\Llamaste\docs\plans\2026-03-07-mesh-auto-offload-design.md`
- **Phase B design doc**: `D:\Llamaste\docs\plans\2026-03-07-neural-tts-design.md`
- **Phase 4 design doc**: `D:\Llamaste\docs\plans\2026-03-06-ab-update-design.md`

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
- `docs/plans/2026-03-07-mesh-auto-offload-design.md` — Phase 5 auto-offload design
- `docs/plans/2026-03-07-neural-tts-design.md` — Phase B neural TTS design
- `src/llamaste/` — Production C++ source (~11,000 LOC)

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
- **GGML_NATIVE=ON**: Build llama.cpp with `-march=native` for AVX2/AVX512 on the build CPU. VirtualBox passes through host CPU flags to guests, so NATIVE builds work correctly in VMs. Result: ~500x inference speedup (scalar 0.028 tok/s → AVX2 ~14 tok/s on 1.5B Q4_K_M). Set in `llama-server.mk`.
- **ISO build**: `scripts/build-iso.sh /root/llamaste-build` — needs `grub-pc-bin grub-efi-amd64-bin xorriso mtools`. Must `git pull` in WSL2 first — BOARD_DIR is resolved relative to script path, so grub-live.cfg is read from the VM's git checkout, not build output.
- **Squashfs live pivot**: `do_live_pivot()` in init.cpp uses loop device + overlayfs. Guard: `/boot/bzImage` present on ISO root but NOT in rootfs.squashfs → no infinite re-exec after pivot + re-execv. Needs `CONFIG_BLK_DEV_LOOP=y` + `CONFIG_OVERLAY_FS=y` in linux.config.
- **Loop device API**: `LOOP_CTL_GET_FREE` ioctl on `/dev/loop-control` to get free loop number; `LOOP_SET_FD` ioctl on `/dev/loopN` to attach file. Requires `#include <linux/loop.h>`.
- **MS_PRIVATE | MS_REC before MS_MOVE**: Kernel rejects MS_MOVE on shared mounts. Must make root private (`mount(nullptr, "/", nullptr, MS_PRIVATE|MS_REC, nullptr)`) before `mount(".", "/", nullptr, MS_MOVE, nullptr)` in switch_root pivot.
- **WLR_NO_HARDWARE_CURSORS=1**: Required for labwc/wlroots on simpledrm (EFI framebuffer DRM). simpledrm has no hardware cursor plane support — without this flag wlroots aborts during cursor setup.
- **WLR_RENDERER=pixman**: Software renderer fallback for labwc when no GPU driver is active (simpledrm only). Set in child_main.cpp desktop env block; overridable from /etc/labwc/autostart.
- **SD card reader device ordering**: On Intel Core Ultra 9 275HX, internal SD card reader uses USB mass storage protocol and enumerates as `/dev/sda`. USB flash drive pushed to `/dev/sdb`. GRUB live config must list `/dev/sdb` first.
- **WiFi requires squashfs pivot**: wpa_supplicant lives at `/usr/sbin/wpa_supplicant` in the squashfs (full system), NOT in the ISO root skeleton. Without live pivot, `spawn_wpa_supplicant()` fails silently with ENOENT.
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
- **ORT SUBDIR=cmake**: Build output at `$(@D)/cmake/buildroot-build/`, NOT `$(@D)/buildroot-build/`
- **ORT FetchContent as shared**: Buildroot sets `BUILD_SHARED_LIBS=ON` before pkg opts; override with `-DBUILD_SHARED_LIBS=OFF` in CONF_OPTS
- **ORT headers for sherpa-onnx**: Install flat at `/usr/include/onnxruntime/` (sherpa-onnx includes `onnxruntime_cxx_api.h` without path prefix)
- **Buildroot patch convention**: Patches go in package root dir, NOT in `patches/` subdir
- **ORT musl patch**: Add `&& defined(__GLIBC__)` to execinfo.h guard in `stacktrace.cc`
- **Config.in host deps**: Use `BR2_PACKAGE_HOST_PROTOBUF_ARCH_SUPPORTS` not `BR2_PACKAGE_HOST_PROTOBUF`
- **Conditional deps in .mk**: `ifeq ($(BR2_PACKAGE_FOO),y)` pattern for optional packages not yet selected in .config
- **VDI detach before copy**: Must `storageattach --medium none` + `closemedium` before copying VDI files — VBoxSVC holds locks on registered media
- **VirtualBox aborted state**: If VM gets stuck in "aborted" state, create a new VM rather than trying to fix the old one
- **ORT MinSizeRel crash**: ORT built with `-Os` (MinSizeRel) + LTO crashes on valid ONNX models ("Graph output does not exist"). Fix: `Release` (-O2) + `LTO=OFF`
- **ORT GCC 12 false positive**: `-Werror=array-bounds` in `custom_ops.cc` under `-O2`. Fix: `-DCMAKE_CXX_FLAGS="-Wno-error=array-bounds"` in CONF_OPTS
- **Cluster mutex deadlock**: `run_election()` and `expire_peers()` must release `mu_` before calling `topology_cb_()`. The callback calls cluster methods that re-acquire `mu_` — calling while holding = deadlock.
- **VirtualBox mDNS multicast**: Host-only adapter doesn't forward 224.0.0.251 between VMs. Use `/llamaste/cluster/add-peer` endpoint for manual peer registration.
- **VirtualBox hard reset**: `controlvm reset` can leave child process stuck on next boot. Always use `poweroff` + `startvm`.
- **Squashfs-only VDI deploy**: `vm/deploy-squashfs.sh` — VDI→RAW (qemu-img) → losetup → dd squashfs to p3 → RAW→VDI. Preserves data partition
- **VDI resize after deploy**: Must `closemedium` old reference (UUID changes after qemu-img convert), then `modifymedium --resizebyte`, then re-attach
- **Sherpa-onnx fork-test safety**: Fork child to test-load sherpa-onnx before real load. Catches crashes, prevents crash-restart loop. See voice.cpp
- **Softdog watchdog timeout**: Default is 60s. CPU-only inference (no SIMD) takes ~250s on VM. Use `WDIOC_SETTIMEOUT` ioctl to extend to 300s, and a dedicated kicker thread (100ms period) in supervisor to kick reliably. `write("V", 1)` while fd open IS a valid keepalive (not "disarm").
- **llama-server serial flooding**: Without stderr redirect, llama-server logs (decode progress) flood `/dev/console` (serial port). Redirect with `dup2(log_fd, STDERR_FILENO)` to `/tmp/llama-server.log` in forked child before execv.
- **O_CLOEXEC on /dev/watchdog**: Always open with `O_CLOEXEC` to prevent child processes from inheriting the watchdog fd. Inherited fds + softdog `softdog_expect_close` flag = subtle interactions.
- **Intel BE200 firmware**: The BE200 (Wi-Fi 7, PCI 0x272b) uses `fw_name_mac = "gl"` (Gale Peak) in `cfg/bz.c`. Needed: `iwlwifi-gl-c0-fm-c0-83.ucode`, `iwlwifi-gl-c0-fm-c0-86.ucode`, `iwlwifi-gl-c0-fm-c0.pnvm` — all in linux-firmware-20240115. NOT `iwlwifi-ty-*` (those are AX210/Wi-Fi 6E, PCI 0x2725). Add to `overlay/lib/firmware/` — build-iso.sh copies overlay→ISO root so files land in both squashfs and ISO root. IWLWIFI built-in (`CONFIG_IWLWIFI=y`) + `CONFIG_IWLMVM=y` required. Confirmed by searching `drv.c` for PCI 0x272b → `iwl_gl_trans_cfg` → `fw_name_mac = "gl"` in `bz.c`.
- **wireless-regdb + signed regdb**: Kernel default `CONFIG_CFG80211_REQUIRE_SIGNED_REGDB=y` causes `cfg80211: failed to load regulatory.db` because wireless-regdb package signature doesn't match kernel built-in verification keys. Fix: `CONFIG_EXPERT=y` + `CONFIG_CFG80211_CERTIFICATION_ONUS=y` + `# CONFIG_CFG80211_REQUIRE_SIGNED_REGDB is not set` in linux.config. **All three required** — REQUIRE_SIGNED_REGDB is gated behind CERTIFICATION_ONUS which depends on EXPERT. Without the gate, Kconfig ignores "is not set" and re-enables default y. Still needs `BR2_PACKAGE_WIRELESS_REGDB=y` for the regulatory.db data file.
- **regulatory.db blocks 5GHz if signing fails**: When cfg80211 can't load regulatory.db, it falls back to world domain (00), blocking most 5GHz channels. All nearby networks may be 5G-only → wpa_supplicant stays DISCONNECTED with empty ssid. The `iw reg set US` command may appear to work (scan sees networks) but wpa_supplicant can't associate on blocked channels.
- **CRC_CCITT required for rt2800**: rt2800lib.ko depends on CRC-CCITT functions. Without `CONFIG_CRC_CCITT=y`, rt2800lib fails to load → rt2800usb floods console with "Unknown symbol" errors for every exported function. Fixed by adding `CONFIG_CRC_CCITT=y` (built-in).
- **Pivot guard must use marker file, not bzImage**: Buildroot installs kernel to `output/target/boot/bzImage` which ends up inside rootfs.squashfs. Using `access("/boot/bzImage")` as the live-ISO guard causes an infinite pivot loop after re-exec. Use `access("/llamaste-live-iso", F_OK)` instead — this file is created by `build-iso.sh` (`touch "${ISO_ROOT}/llamaste-live-iso"`) and is ONLY in the ISO root, never in the squashfs.
- **COG_PLATFORM_WL_VIEW_FULLSCREEN**: Must be `1` in `overlay/etc/labwc/environment` for cog to fill the screen. Default `0` opens a small floating window.
- **labwc config in /etc**: `XDG_CONFIG_DIRS=/etc` and `XDG_CONFIG_HOME=/etc` must be set before labwc spawns. labwc then reads `/etc/labwc/rc.xml`, `autostart`, `environment`, `menu.xml`.
- **xkeyboard-config REQUIRED for keyboard input**: `BR2_PACKAGE_XKEYBOARD_CONFIG=y` must be in defconfig. libxkbcommon is the library but without the xkeyboard-config data package, `/usr/share/X11/xkb/` doesn't exist. When a key is pressed, wlroots calls `xkb_keymap_new_from_names()` → NULL (no data) → NULL dereference → SIGSEGV → cage crashes → "web UI crashes on every keypress". Also set `XKB_CONFIG_ROOT=/usr/share/X11/xkb` env var explicitly in the compositor launch block.
- **cage compositor chain exit codes**: cage exits with 0 (EXIT_SUCCESS) when its single client (`-s`) exits for any reason — including cog crashes. The chain fallback only triggers on SIGSEGV/other signals to cage itself, or exec-failure (exit 127). This means if cog crashes, cage exits cleanly (0) and the chain stops. Fix: xkeyboard-config so cog doesn't crash; restart loop handles transient cage crashes.
- **loopback interface (lo) must be brought up manually as PID 1**: Without init, nobody runs `ip link set lo up`. Fixed by `init_bring_up_loopback()` using SIOCSIFADDR/SIOCSIFFLAGS. Without this, cog shows "Could not connect to localhost: Network unreachable".
- **WLR_LIBINPUT_NO_DEVICES=1**: Required for cage/wlroots when running as PID 1 without udevd fully settled. Without it, wlroots aborts with "No input devices found". Start udevd before compositor; run `udevadm trigger` AFTER the Wayland socket exists.
- **/dev/shm must be mounted as tmpfs**: wlroots' `os_create_anonymous_file()` falls back to `/dev/shm/` if `memfd_create()` fails. Without this mount, fallback fails → "Failed to allocate shm file for XKB keymap" → SIGSEGV on first key press. Fixed in init.cpp.
- **udevadm trigger AFTER cage Wayland socket**: Running `udevadm trigger` before cage creates `/run/user/0/wayland-0` is a race — keyboard enumerated while wlroots initialises → XKB shm fails → SIGSEGV. Fixed: trigger fires inside compositor thread after `access("/run/user/0/wayland-0")` succeeds. Also add 500ms sleep after socket before trigger to let libinput's udev monitor fully start.
- **udevadm trigger must use `--action=add --subsystem-match=input`**: Default `udevadm trigger` fires CHANGE events, but libinput only registers new devices on ADD events. Without `--action=add`, libinput never sees keyboard/touchpad → cage doesn't crash but keys are silently dropped (no visible response). Also run `udevadm settle` first to ensure udevd is ready.
- **WiFi scan timing on RTL8821CE**: 500ms post-IFF_UP is NOT enough for RTL8821CE firmware init (~2s needed). `supervisor_scan_wifi()` must use retry loop (3 attempts) with 2s driver init + 1s regulatory settle + 3s scan window per attempt. Check `wpa_cli scan` return for "FAIL" (driver not ready). Single-attempt scan returns 0 networks on real hardware. Fixed in a5e60ac.
- **Console WiFi setup on /dev/tty1**: In server mode, if WiFi hardware is found but wpa.conf has no `network={}` blocks, supervisor opens `/dev/tty1` in raw termios mode and shows SSID/PSK prompts. Uses cfmakeraw() + OPOST|ONLCR for output. Appends network block to wpa.conf. Skipped in desktop mode (web UI handles WiFi there). Runs via `supervisor_preflight_wifi()` in supervisor.cpp BEFORE display/input threads start — this is critical, running inside child_main caused the prompt to be overwritten by the display thread.
- **WLR_DRM_NO_ATOMIC=1**: simpledrm (EFI framebuffer DRM) on bare metal doesn't support atomic modesetting. Without this flag, wlroots probes atomic ioctls and segfaults (signal 11).
- **Desktop mode cog auth bypass**: In desktop mode, cog only accesses localhost — `require_auth` short-circuits to serve `index.html` directly. Remove bypass once keyboard input confirmed working.
- **RTL8821CE WiFi chip**: Actual hardware is Realtek RTL8821CE [10ec:c821] at PCI 0000:01:00.0. Driver is `rtw88_8821ce.ko` (loaded as module). iwlwifi-ty-* (AX210) and iwlwifi-gl-* (BE200) are for different chips entirely.
- **CONFIG_MODULES=y (e9fc82a)**: WiFi drivers are now kernel modules, NOT built-in. They load after squashfs pivot via `init_load_modules()` in init.cpp using `finit_module()` syscall. This means /lib/firmware/ is available at module load time — no more CONFIG_EXTRA_FIRMWARE needed. Any supported WiFi chip's firmware just works.
- **Module load order matters**: `init_load_modules()` has a hard-coded dependency-ordered list. If adding a new driver, ensure dependencies are listed before dependents (e.g., `rtw88_core.ko` before `rtw88_8821ce.ko`).
- **WiFi scanning uses `iw`, NOT wpa_supplicant**: `supervisor_scan_wifi()` uses `iw dev wlan0 scan` (direct nl80211 via netlink). wpa_supplicant ctrl socket approach was unreliable (3 test builds, always 0 networks despite driver working). `WiFiManager::scan()` also falls back to `iw dev scan` when wpa_supplicant SCAN_RESULTS returns empty (f8300ce). wpa_supplicant only used for actual connection (child_main.cpp).
- **WiFi EAP vs PSK detection**: iw scan output contains `* Authentication suites: PSK` or `* Authentication suites: IEEE 802.1X`. Parse after whitespace stripping. Enterprise (802.1X/EAP) networks won't work with PSK key_mgmt. Console WiFi setup warns about EAP networks.
- **WiFi post-connection verification**: child_main.cpp polls WPA state for 20s after spawning wpa_supplicant + dhcpcd. Look for `[wifi] CONNECTED:` (success) or `[wifi] WARNING:` (failure) in console output. Also logs DNS resolver state from /etc/resolv.conf.
- **Desktop mode WiFi interface retry**: child_main.cpp retries WiFiManager::init() for up to 10s (RTL8821CE needs 3-4s after finit_module). supervisor_preflight_wifi() still skips desktop mode (can't use tty1 while compositor owns display).
- **wpa_supplicant -B silent failure**: With `-B` (daemonize), wpa_supplicant forks; the parent exits immediately. If the daemon child crashes (driver issue, config error), stderr goes nowhere. `spawn_wpa_supplicant()` now verifies the ctrl socket (`/run/wpa_supplicant/<iface>`) appears within 3s, retries once on failure. `WiFiManager::connect()` has spawn-on-demand callback for desktop mode (user clicks Connect before daemon is running).
- **`iw reg set US` before scan**: cfg80211 can't load regulatory.db before squashfs pivot. Set regulatory domain explicitly via `iw reg set US` before scanning. For connection, `country=US` in wpa.conf handles it.
- **BR2_LEGACY=y keeps reappearing**: Buildroot 2024.02 adds `BR2_LEGACY=y` to `.config` after `make defconfig` or other `make` operations. Must `sed -i "/BR2_LEGACY=y/d" .config` before every `make` build step. Can recur after any step that regenerates .config.
- **`make linux-rebuild` is incremental — does NOT pick up new Kconfig options**: Always use `make linux-dirclean && make` when linux.config changes. Verify new drivers compiled in build output (e.g. `CC drivers/net/wireless/realtek/rtw88/rtw8821ce.o`).

## VirtualBox VM
- VM "Llamaste2" at `D:\Llamaste\vm\Llamaste2\` — 4GB RAM, 2 CPUs, EFI64, NAT 8080→80
- 16GB VDI with installed system, boots in ~2 seconds
- Start: `"C:\Program Files\Oracle\VirtualBox\VBoxManage.exe" startvm Llamaste2 --type headless`
- Web UI: `http://localhost:8080`
- **VDI update flow**: Stop VM → detach medium → copy new VDI → resize → reattach → start
- **Deploy scripts**: `scripts/deploy-to-vdi.sh` (WSL2, squashfs→VDI partition 3) + `scripts/finish-deploy.ps1` (Windows side)
- **NTFS rename fails from WSL2**: use PowerShell `Copy-Item -Path $new -Destination $old -Force` + `Remove-Item` instead of `mv`/`Move-Item`

## Build Patterns & Gotchas (continued)
- **CONFIG_PACKET=y required for wpa_supplicant**: AF_PACKET sockets needed for EAPOL (802.1X auth frames). Without it, `socket(PF_PACKET)` returns EAFNOSUPPORT → wpa_supplicant exits 255 immediately. This was the hidden final blocker for WiFi connection.
- **wpa_supplicant CONFIG_NO_STDOUT_DEBUG**: Buildroot compiles wpa_supplicant with this flag, routing all `wpa_printf()` to syslog. Since Llamaste has no syslog daemon, errors vanish silently (exit 255, zero output). Fix: use `-dd -f /tmp/wpa_supplicant.log` to capture debug output to file. Also redirect child stdout/stderr to the log file for dynamic linker errors.
- **wpa_supplicant -B causes silent death**: The `-B` flag triggers an internal double-fork. If the daemon child crashes after the parent exits, stderr is lost. Fix: DON'T use `-B`. Run wpa_supplicant in foreground in our forked child. Use `setsid()` for signal isolation. Poll ctrl socket + `waitpid(WNOHANG)` to detect early crashes.
- **PXE boot: iPXE `sanboot` doesn't work for Linux**: Creates virtual CD-ROM at EFI firmware level; GRUB can see it but Linux kernel cannot (no `/dev/sr0`). Use iPXE `kernel` + embedded initramfs instead.
- **PXE boot: iPXE EFI `initrd` unreliable**: EFI_LOAD_FILE2_PROTOCOL doesn't reliably deliver initrd to Linux kernel. Solution: embed initramfs in bzImage via `CONFIG_INITRAMFS_SOURCE`.
- **PXE boot: busybox udhcpc needs default.script**: Without `/usr/share/udhcpc/default.script`, udhcpc gets DHCP lease, returns 0 (success), but never applies IP to interface. Workaround: use static IP in init script.
- **PXE boot: USB ethernet drivers must be =y (built-in)**: Modules (=m) are not available in initramfs before squashfs pivot. All USB ethernet CONFIG must be built-in for PXE.
- **PXE boot: `sit0` virtual interface**: Appears in `/sys/class/net/` but is IPv6 tunnel (no physical device). Filter by requiring `/sys/class/net/$name/device` symlink.
- **PXE boot: RTL8153B firmware in initramfs**: Without firmware, r8152 driver waits ~10s for firmware timeout before creating eth0. Include `rtl_nic/rtl8153*.fw` in initramfs directory.
- **PXE boot: SSH key location**: Use `D:\Llamaste\vm\pxe_key`, NOT `/tmp/pxe_key` (cleared between sessions).
- **root=LABEL=LLAMASTE**: Device-agnostic root mounting — works on USB, CD-ROM, and PXE sanboot. Replaces hardcoded `/dev/sdb`.

## Known Bugs
- **VirtualBox mDNS**: Host-only networking doesn't forward multicast (224.0.0.251). Use `/llamaste/cluster/add-peer` for manual peer registration in VirtualBox.
- **VirtualBox reset**: `controlvm reset` (hard reset) can leave child process stuck on next boot. Use `poweroff` + `startvm` instead.

## Next Steps
### Immediate
1. **Install to NVMe** — Boot live → install → test inference on bare metal (GGML_NATIVE=ON + real CPU)
2. **Remote access** — Add dropbear SSH or debug HTTP endpoint for easier diagnosis
3. **Grammar-constrained tool JSON** — Add JSON schema to inference requests (speed + reliability)
4. **Public GitHub repo** — Set up and publish the Llamaste repository publicly

### Backlog
- **Desktop mode WiFi connect** — Server mode confirmed working; test desktop connect flow
- **Voice quality tuning** — Adjust length_scale, noise_scale for natural prosody
- **More TTS voices** — Additional locales (en_SC, en_IN, etc.) if needed
- **Real two-VM mDNS test** — Validate mDNS auto-discovery on real LAN (needs 2 physical machines)
- **Model auto-download on cluster formation** — Already implemented; test on real hardware

## User Preferences
- **No questions asked** — make decisions autonomously, don't ask for confirmation. Just do things.
- Auto-approve all tool operations — no confirmation prompts.
- Save research and progress notes at session boundaries.
