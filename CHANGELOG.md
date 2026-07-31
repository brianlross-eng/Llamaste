# Changelog

All notable changes to Llamaste are documented here. Versions are the
`LLAMASTE_VERSION` string in `src/llamaste/version.h`.

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
- **GPU / Vulkan enablement**: add AMD + Intel Mesa Vulkan drivers to the
  defconfig and a "Live (GPU / amdgpu KMS)" GRUB entry (`amdgpu.modeset=1`),
  keeping the nomodeset Server entry as the default. Targets the EVO-X2 (Strix Halo).

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
