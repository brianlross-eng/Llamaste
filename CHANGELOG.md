# Changelog

All notable changes to Llamaste are documented here. Versions are the
`LLAMASTE_VERSION` string in `src/llamaste/version.h`.

## 0.4.7-beta

Strix Halo (gfx1151) GPU firmware — fixes the amdgpu KMS freeze.

### GPU firmware
- Added the Strix Halo amdgpu firmware to `board/llamaste/overlay/lib/firmware/amdgpu/`
  (installed into the squashfs via `BR2_ROOTFS_OVERLAY`, since amdgpu is a module
  loaded post-pivot). The bundled `linux-firmware` (20240115) predates gfx1151, so
  amdgpu discovered the GPU, fetched the VBIOS, then froze in PSP/GC/SMU bring-up.
- Fetched from linux-firmware git and verified against the kernel 6.18 source
  (`IP_VERSION(11, 5, 2)` → prefix `gc_11_5_2`). Added the full family superset to
  avoid a wrong-minor reflash: `gc_11_5_2_*` (imu/me/mec/mes1/mes_2/pfp/rlc),
  `psp_14_0_{0..5}_*`, `vpe_6_1_{0,1,3}`, `dcn_3_6_dmcub`, plus `smu_14_0_{2,3}`
  (already present). VCN/JPEG (`vcn_4_0_5`, JPEG bundled) and `sdma_6_0_3` were
  already present.
- The live "GPU / amdgpu KMS" entry (`amdgpu.modeset=1`) is the test vehicle;
  installed entries stay `nomodeset` until KMS is confirmed on the EVO-X2.

## 0.4.6-beta

EVO-X2 (Strix Halo) bring-up fixes from real-hardware testing.

### Installed boot — fixes silent post-install "hang"
- `board/llamaste/grub.cfg` (Server + Desktop entries): the installed entries
  used `quiet` and listed `console=tty0 console=ttyS0,115200`. The **last**
  `console=` becomes `/dev/console`, so all initramfs/init output went to the
  **serial port** while `quiet` hid kernel progress — on a machine with only a
  monitor the screen looked dead after the first `pr_emerg` line (the benign
  `RDSEED32 is broken` AMD erratum note). Reordered to
  `console=ttyS0,115200 console=tty0` (screen is primary) and dropped `quiet`.
- Switched the installed entries from `amdgpu.modeset=0` to **`nomodeset`**,
  matching the known-good live entry — DRM refuses to bind, which is what the
  EVO-X2 needs until Strix Halo GPU firmware is bundled (below).

### Known limitation (follow-up)
- The live "GPU / amdgpu KMS" entry still freezes on Strix Halo: the bundled
  `linux-firmware` (20240115) predates gfx1151 firmware — missing `gc_11_5_2_*`,
  `smu_14_0_x`, `vpe_6_1*`, `dcn_3_6`. Kernel 6.18 supports the hardware; the fix
  is a `linux-firmware` bump. Use the `nomodeset` entries meanwhile.

## 0.4.5-beta

Cherry-pick batch from `CHERRY-PICK-TO-MASTER.md` — usability, GPU, and a full
security-hardening pass applied on top of canonical master.

### Boot & install (Tier 0)
- `spawn_dhcpcd`: guard `strdup(NULL)` on the argv terminator (crash on DHCP bring-up).
- Web installer `/install/start`: dispatch `install.to_disk` with `confirmed=true`
  so the install is not rejected with "confirmation required".

### Usability (Tier 1)
- **Default to the smallest model tier** in `do_auto_upgrade_check()` so a bare
  live boot with no bundled model still lands on a runnable 0.5B model instead of
  recommending something the box can't load.
- **GPU display**: a "Live (GPU / amdgpu KMS)" GRUB entry (`amdgpu.modeset=1`),
  keeping the nomodeset Server entry as the default. Targets EVO-X2 (Strix Halo)
  display bring-up.
- **Hardware Vulkan: not available on this stack (documented limitation).**
  Buildroot 2024.02.13 / Mesa 24.0.9 packages no AMD (RADV) Vulkan option; the
  Intel (ANV) driver requires glibc and we build musl; only software Vulkan
  (lavapipe) is buildable and is intentionally off (slower than CPU). The Vulkan
  loader ships but exposes 0 devices; llama.cpp's Vulkan backend falls back to
  CPU. Deferred until a Mesa bump exposes RADV (or a glibc toolchain for ANV).

### Security hardening (Tier 2)
- **init.cpp**: never reformat the DATA partition on a failed ext4 resize/mount in
  the auto-grow path; only the explicit phase-1 candidate path may format
  (`allow_format`), phase-2 discovery never does. Prevents silent data loss.
- **tools_fs.cpp**: `validate_data_path` now resolves the longest existing
  ancestor via `realpath` and requires it to stay under `/data` (blocks `..` /
  symlink escape); `search_recursive` gained a depth cap (20) and uses `lstat`
  so it won't follow symlinks into an infinite loop.
- **net_mdns.cpp**: re-anchor the parse offset after an SRV record (`offset =
  rdata_start + rdlength`) like the TXT/A branches — malformed SRV rdata no
  longer desyncs the parser.
- **tools_install.cpp**: bounds-check the GPT partition-entry count/size before
  computing the entry-array size (reject 0, >256 entries, entry size <128 or >4096).
- **tools_network.cpp**: validate DNS servers with `inet_pton` (v4/v6) in `set_ip`.
- **supervisor.cpp** (`console_wifi_setup`): escape `"`/`\` and drop control chars
  in SSID/PSK before writing `wpa.conf`, and `chmod 0600` the file (it holds the PSK).
- **wifi.cpp** (`connect`): send the SSID to wpa_supplicant as a hex string
  (no quoting to break out of) and escape the PSK — both are attacker-supplied
  off the air.
- **web/system.js**: add `escapeHtml` and apply it to every SSID/security sink in
  the WiFi status and scan-result rendering (stored/reflected XSS from a hostile AP name).
- **updater.cpp**: close the verify→write TOCTOU — read the payload into memory
  once, hash that buffer, and write the *same* buffer to the inactive partition
  (was re-reading the file after the hash check). Refuse to install when the build
  shipped the placeholder all-zero update public key (fail closed). 1 GiB payload cap.

### TLS (Tier 3)
- **tools_model_download.cpp** (3 curl sites): never disable TLS verification.
  `SSL_VERIFYPEER=1`/`VERIFYHOST=2` unconditionally; use a CA bundle when present,
  otherwise rely on curl's default store. Removes the silent-MITM fallback.
