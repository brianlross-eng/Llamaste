# Changelog

All notable changes to Llamaste are documented here. Versions are the
`LLAMASTE_VERSION` string in `src/llamaste/version.h`.

## 0.5.9-beta

Restore Desktop mode's compositor (task #18). Desktop mode had silently degraded to
"boots like server mode" -- the on-screen GUI never came up.

- **Root cause:** the defconfig enabled `BR2_PACKAGE_LABWC=y`, but **labwc isn't a package
  in Buildroot 2025.02** (it was in the old 2024.02 tree). `olddefconfig` dropped the unknown
  symbol silently, and with it the entire wlroots stack labwc would have pulled in. So in
  desktop mode every compositor `exec`-failed and the box just sat at a console with the web
  server running -- indistinguishable from server mode (and the wifi console step is skipped
  in desktop mode, so it even booted a touch faster).
- **Fix:** enable `BR2_PACKAGE_CAGE=y` -- cage is the compositor `child_main` actually
  launches first (wlroots-based kiosk; `zwp_text_input_v3` so typing in the web UI works),
  and it `select`s wlroots (libinput/seatd/libdrm/pixman/hwdata/libdisplay-info). Verified
  cage 0.2.0 + wlroots 0.18 build clean on the 2025.02 base. cog (the browser) was already
  building fine; it just had no compositor to run inside.

Desktop mode now boots with a real amdgpu KMS display (`nomodeset` is gone as of 0.5.3), so
this is the first build where the compositor should actually render on-screen.

## 0.5.8-beta

Cluster join/leave is now a UI choice, and mDNS is fully opt-in (task #16, "option B").

- **mDNS is silent by default.** A standalone box no longer opens the multicast socket,
  advertises `llamaste.local` / `_mcp._tcp`, or listens for peers -- it behaves like a plain
  llama server reached by IP. The mDNS responder starts *only* when a cluster group name is
  set (`/data/llamaste/cluster-enabled`). Decided at boot and gated on `mdns_started`, so a
  standalone box does zero network discovery and never touches an un-started responder.
- **Set the group in the UI, like choosing a model** (not at install). System -> Cluster has
  a **Group** field + Save. Same name on 2+ boxes links them into one cluster; blank =
  standalone. Backed by `GET/POST /llamaste/cluster/config`, which writes/deletes
  `/data/llamaste/cluster-enabled` (validated `[A-Za-z0-9_-]`, <=63 chars). Applies on the
  next reboot -- clustering is brought up at boot, so we never tear a live cluster down.

## 0.5.7-beta

Stop leaking raw `<tool_call>` tags into chat (task #15). A truncated or malformed tool
call -- e.g. the model emits `<tool_call>\n{"name": ...` but the JSON is cut off by
max_tokens or is otherwise unparseable -- was skipped by the tool-call parser and then
fell through to the plain-text path, so the raw tag text showed up in the user's chat.

- New `strip_tool_call_residue()` removes complete `<tool_call>...</tool_call>` blocks that
  weren't consumed as structured calls, drops a dangling unclosed `<tool_call>` (the
  truncated-call case), and clears stray closing tags. Applied at the source (the
  llama_inference plain-text branch, so `/v1/chat/completions` and the semantic cache are
  clean) and defensively inside `agent_turn` (covering `/llamaste/chat` SSE, which streams
  the agent's final text). If the model emitted *only* a bad tool call, the stripped
  content is empty and the caller shows the usual no-response note instead of raw tags.
- Multiple and malformed calls in one turn were already handled (the parser loops every
  `<tool_call>` block; a malformed block is skipped, and an all-malformed turn triggers the
  existing GBNF grammar-constrained retry) -- this closes the remaining visible leak.

## 0.5.6-beta

Detect and recover from a **wedged** llama-server (task #14). The crash monitor only
reacted to the engine process *exiting* (`waitpid`); a wedged engine -- alive but
503ing / hung / deadlocked -- never exits, so `/health` kept reporting
`inference_ready: true` while every chat request failed.

- New **health-poll thread** probes llama-server `/health` every 5 s while a model is
  loaded. On 3 consecutive bad probes (~15 s wedged) it flags the engine not-ready and
  SIGKILLs it, which makes the existing crash monitor's `waitpid()` return and respawn a
  fresh engine. Poller detects+kills; monitor is the sole respawner (no double-respawn).
  It idles when no model is loaded, including the respawn window, so it never kills a
  server that is still starting up.
- `/health` now reports real readiness: `inference_ready` = `model_loaded && engine
  responsive`, plus a new `engine_responsive` field. It no longer lies during a wedge.
- "no slot available" (busy mid-generation) counts as healthy, so long generations don't
  trip a false respawn.

## 0.5.5-beta

Boot-menu cleanup on both the live ISO and installed system. Now that GPU vs no-GPU
is no longer a menu choice (KMS is on by default and vendor-neutral, GPU decided at
runtime), the menus are trimmed to 3 entries each:

- **Llamaste Server** (default) — KMS on, vendor-neutral
- **Llamaste Desktop** — KMS on
- **Llamaste Server (safe mode - no GPU)** — the one `nomodeset` recovery fallback

- Live ISO menu (`grub-live.cfg`): the old default was `nomodeset` (booted the USB to
  CPU), plus a separate "GPU / amdgpu KMS" entry and AMD-specific `amdgpu.modeset=0/1`
  flags. Removed — the default now boots to the GPU with no vendor flags, so there's no
  "pick the GPU entry" dance. Dropped the redundant Diagnostic and per-mode safe entries.
- Installed menu (`grub.cfg`): dropped the redundant "Desktop (safe mode)" entry.

## 0.5.4-beta

Hotfix: the 0.5.3 installed `grub.cfg` contained a non-ASCII character (an em-dash in
a comment). GRUB's config parser rejected the file and dropped to the `grub>` rescue
prompt, so a fresh install of 0.5.3 wouldn't boot (no menu). grub.cfg is now pure ASCII.
(Recover a stuck 0.5.3 install from the `grub>` prompt with:
`search --file --set=root /bzImage` then `linux /bzImage init=/opt/llamaste/llamaste
rootfstype=squashfs ro amdgpu.modeset=1 llamaste.mode=server llamaste.slot=A`,
`initrd /initramfs.cpio.gz`, `boot`.)

## 0.5.3-beta

**The installed system now boots to the GPU by default** (the last thing blocking real
GPU inference after a disk install), and the boot config is vendor-neutral.

- **Root cause of "installs run on CPU":** the installed `grub.cfg` forced `nomodeset`,
  which disables kernel mode-setting for *every* GPU vendor. With no KMS, RADV (and i915 /
  nouveau) enumerate no device, so llama.cpp silently falls back to CPU — it still prints
  `offloaded N/N layers to GPU` but the buffers are `CPU_Mapped`. (The live ISO worked only
  because it has a separate `amdgpu.modeset=1` GPU entry.)
- **Fix:** the default `menuentry`s ("Llamaste Server" / "Llamaste Desktop") no longer pass
  `nomodeset`, so KMS is on and `init` brings up whatever GPU is present (amdgpu / i915 /
  nouveau, loaded adaptively by hardware detection). **No vendor flag is hardcoded in GRUB**
  — the GPU-vendor decision stays at runtime, where it belongs. A `nomodeset` "safe mode"
  entry is kept as a fallback for hardware where KMS hangs.
- Verified on the EVO-X2 (Strix Halo, gfx1151): 32B Q4 fully offloaded (65/65, 18.5 GB on
  `Vulkan0`) at 10.3 tok/s vs 4.4 on CPU; 14B at 21, 7B at 40, 3B at 78. See
  `docs/gpu-benchmarks.md`.

## 0.5.2-beta

Makes GPU offload actually engage on Strix Halo (0.5.0/0.5.1 silently ran real-size
models on CPU), plus boot-status and GPU-visibility improvements.

### GPU offload now engages for real-size models (the important fix)
- **Root cause: hwdetect runs before amdgpu binds.** `amdgpu` loads as a module after
  the squashfs pivot, but `detect_hardware()` runs earlier — so the DRM scan finds no
  card and falls to the PCI fallback, which sets only `detected`+`name` (no VRAM/GTT,
  `unified=false`). The `-ngl` decision then hit the CPU branch — **no `-ngl` passed,
  every model on CPU** (`offloaded 0/N`). (0.5.0's USB boot only worked by a timing
  fluke.) Fix: **`refresh_gpu_memory()` re-reads the amdgpu sysfs at llama-server spawn
  time** (driver up by then), reading `mem_info_vram_total` + `mem_info_gtt_total` and
  classifying unified unless there's ≥2 GiB of real dedicated VRAM.
- **`-ngl` decision hardened**: for any detected GPU with a Vulkan ICD present, offload
  `-ngl 99` (all layers) unless it's a confirmed discrete card with ≥2 GiB VRAM (which is
  sized to fit). On Strix Halo the iGPU shares system RAM via GTT — where RADV allocates —
  so a 0/tiny dedicated-VRAM reading can never gate offload again.

### GPU visibility
- `/debug/sysinfo` now includes a live `gpu` object: detected, name, driver, `vram_mb`,
  `gtt_mb`, `unified`, `discrete` (refreshed from sysfs on request).

### Boot status
- The console dashboard shows **BOOTING** (not RUNNING) until the web UI actually
  answers on 127.0.0.1, instead of flipping to RUNNING the instant the child forks.

## 0.5.0-beta

**GPU inference on the AMD Strix Halo iGPU (Radeon 8060S, gfx1151) via Vulkan/RADV.**
Until now llama-server ran CPU-only (`offloaded 0/N layers`) because the image
shipped no Vulkan compute driver. This build ships Mesa RADV so llama.cpp's Vulkan
backend can offload onto the iGPU instead of the CPU.

### What ships now
- **Mesa RADV Vulkan driver** for gfx1151: `libvulkan_radeon.so` +
  `/usr/share/vulkan/icd.d/radeon_icd..json` (ICD → `/usr/lib/libvulkan_radeon.so`),
  the Vulkan loader (`libvulkan.so.1`), and `libdrm-amdgpu`. `llama-server` links
  `libvulkan.so.1` and its Vulkan shaders were compiled with `glslc`
  (coopmat/coopmat2/dot/bfloat16 support — the cooperative-matrix path gfx1151 wants).
- **llama-server GPU flags corrected**: `GGML_VULKAN=ON` (our AMD path), `GGML_HIP=OFF`
  (HIP/ROCm needs a cross-compiler Buildroot doesn't have and hard-fails the build;
  AMD GPUs use the Vulkan backend), `GGML_BLAS=OFF` (no OpenBLAS in the image; the
  BLAS backend symbol was undefined at link — CPU accel still comes from `GGML_NATIVE`).

### Build base moved to Buildroot 2025.02.16 + Mesa 24.2.8
gfx1151 RADV needs Mesa ≥ 24.1, so the GPU image is built from a newer Buildroot tree
(kept separate from the stable 0.4.12 tree). This surfaced a long tail of base-version
build fixes, now captured in `br2-external` (and the build host's Buildroot tree):
- **defconfig**: enable `MESA3D_VULKAN_DRIVER_AMD`, `LIBDRM_AMDGPU`, `MESA3D_LLVM`;
  disable the Intel `iris`/`crocus` gallium drivers (they force `intel-clc`, which
  hard-requires an unpackaged `libclc`; iris never worked here anyway — pixman fallback).
- **onnxruntime.mk**: its CMake FetchContent can't download (Buildroot host-cmake has no
  TLS) — a post-extract hook rewrites `deps.txt` to a local dep cache; protos are
  generated with a matching protobuf-3.21.12 `protoc` (Buildroot's 29.3 emits a
  `runtime_version.h` include absent in the v21.12 runtime); GCC-13 `-Werror`s downgraded.
- **sherpa-onnx.mk**: post-extract hook pre-downloads its FetchContent deps (nested ones
  auto-resolved via `/tmp`); `-include cstdint` for GCC-13's dropped transitive include.
- Build-host Buildroot tree: Mesa bumped 24.0.9→24.2.8 with rebased/pruned patches,
  `libwpe` forced shared-only (static loader failed `--no-undefined`), WebKit
  `SPEECH_SYNTHESIS=OFF` (a VIDEO-off build bug), and a kernel-headers `AT_LEAST`
  mapping so the custom 6.18 kernel passes the (loose) headers check.

Note: **0.4.12-beta remains the stable line.** This is an experimental GPU build; verify
boot + actual GPU offload on the EVO-X2 before promoting.

## 0.4.12-beta

Fixes the real cause of the intermittent stub/503 — the box was auto-clustering
with random LAN machines. Clustering is now opt-in and grouped.

### Cluster safety (this is the root cause of the instability)
- **mDNS discovery was grabbing foreign services.** `parse_service_responses()`
  collected every SRV record arriving on the shared 224.0.0.251 multicast socket
  without checking the owner name matched the queried `_llama-rpc._tcp`. So a
  Windows desktop's unrelated announcements (`_oculusal_sp._tcp`, `_dosvc._tcp`)
  were mis-added as bogus "peers" (garbage port, empty TXT → `ram_mb:0`). The box
  then spawned llama-server with `--rpc` to them, which timed out and wedged/
  respawned the engine — the intermittent stub, the "llama server timing out"
  console line, and the flapping `node_count`. Now filters SRV records to the
  queried service type. (Found live: peer `BRossAsusROG @ .101` was the user's
  desktop's Oculus streaming service, nothing to do with Llamaste.)
- **Reject junk peers** — discovery drops any node without a valid `ram=` TXT.
- **Clustering is now OPT-IN and GROUPED.** A box auto-recruiting other machines'
  LLMs off the LAN unprompted is hostile (reads like a worm). It only advertises/
  discovers the cluster service when `/data/llamaste/cluster-enabled` exists; the
  file's contents are a **cluster ID**, and a node only peers with others sharing
  the same ID — so multiple independent Llamaste clusters can coexist on one LAN.
  **Default: standalone.**

### Inference (cosmetic)
- Gate `-ngl` GPU offload on an actual Vulkan ICD, not just a detected *display*
  GPU. This build has no ICD, so it stays on CPU exactly as before (llama.cpp was
  already offloading 0/29 layers) — this just stops passing a no-op flag.

## 0.4.11-beta

Fixes the phantom-peer 503, the false "no model" alert, and the copy button.

### Cluster / inference stability (the 503 root cause)
- `child_main.cpp`: mDNS peer discovery filtered self by a single `self.ip`, so a
  **multi-homed box** (eth + WiFi) discovered its OWN announcement via the other
  interface's IP and added **itself as a phantom peer**. That fake 2-node cluster
  made it spawn llama-server with an RPC/tensor-split to a "peer" that was really
  itself → llama-server wedged → persistent **HTTP 503** on every chat request.
  Added `is_local_ip()` (checks all interfaces via `getifaddrs`) and reject any
  discovered peer whose IP is local. Observed live on the EVO-X2 (`node_count:2`,
  `tensor_split:"21923,1"`).

### Notifications
- `scheduler.cpp`: the "No AI Model Loaded" alert fired whenever the periodic
  check caught the brief model-swap window (e.g. 0.5B→7B) and then never cleared.
  Debounced: only alerts after the model has been **continuously** unloaded for
  ≥120 s, so a swap/reload can't trip it.

### Web UI
- `system.js`: the API-key **Copy button** did nothing on `http://` LAN origins —
  `navigator.clipboard` is `undefined` there, so `.writeText` threw synchronously
  before the `.catch` fallback. Guard for it and fall back to `execCommand`.
- `dashboard.js`: the "current model" card now shows the logical model name
  (strips the `-00001-of-00002` shard suffix) to match the picker (#12).

### Known (not code)
- On the EVO-X2, Linux sees only ~31 GB of 128 GB — a Strix Halo **BIOS UMA/GPU
  memory carveout**, not a detection bug. Reduce the iGPU memory allocation in
  BIOS to reclaim RAM for CPU models. Remaining hardening (health should reflect
  real llama-server readiness; respawn a wedged llama-server) tracked in #14.

## 0.4.10-beta

Console WiFi-setup no longer blocks an unattended boot; model picker groups shards.

### Console WiFi setup (supervisor.cpp)
- The network-selection keypress and manual-SSID entry did blocking `read()`s on
  tty1, so a headless box sat at the WiFi prompt until someone pressed a key
  (the "hit Enter twice every boot" annoyance). Added `poll()`-based timeouts: a
  25s countdown on the network-selection keypress that auto-skips setup and
  continues boot if nobody responds, and a 25s first-keystroke timeout on manual
  SSID entry. Once you start typing, input reads normally. wpa.conf escaping +
  chmod 0600 preserved.

### Model picker (tools_model.cpp `model.list`, web/dashboard.js)
- The download/select list showed every raw `.gguf` file, including each shard of
  a sharded model (`…-00001-of-00002.gguf`, `…-00002-of-00002.gguf`) as separate
  picks — a user had to know to choose shard 1, and choosing shard 2 silently
  failed. `model.list` now groups shards into one logical model (keyed on the base
  name, using shard 1 as the loadable file), sums their size, and reports
  `sharded`/`shard_count`/`shards_present`/`complete`. The UI shows the logical
  name and disables any model that's missing shards.

## 0.4.9-beta

Fix chat breaking on tool-calling models (`[Inference error: … type must be string, but is null]`).

### Inference
- `build_chatml_prompt()` used `msg.value("content", "")`, but nlohmann's
  `.value(key, default)` **throws `type_error.302` when the key exists and is
  `null`** — it only substitutes the default for *missing* keys. When a model
  emits a tool call, the agent stores the assistant turn with `content: null`
  (`msg_obj["content"] = nullptr`), so the *next* round's prompt build threw and
  the whole turn failed with `[Inference error: …]`. With 67 tools in the prompt,
  a capable model (e.g. Qwen2.5-7B) tool-calls constantly, so it looked like chat
  failed on everything.
- Added a null-safe `jstr()` helper (missing/null → default, string → value,
  object → serialized JSON) and used it for role/content/tool-call name+arguments
  in `build_chatml_prompt`. The semantic-cache readers were already safe (they
  only read system/user content, never the null assistant content).

## 0.4.8-beta

Out-of-box chat: bundle a default model and actually load it.

### Model loading (fixes "no built-in model / no chat" on real hardware)
- The runtime only searched `/data/models/`, which is a fresh **tmpfs** (live) or
  the **DATA partition** (installed) — both mask whatever the squashfs carries
  under `/data`. So with no external model drive, `/data/models` was empty and the
  box booted to **stub mode**. (Earlier QEMU passes only "worked" because a model
  disk was attached, which `scan_usb_for_model()` linked in — a real EVO-X2 with
  just a flash drive had nothing.)
- `main.cpp`: `seed_bundled_models()` symlinks `/opt/llamaste/models/*.gguf` into
  `/data/models/` at boot (both modes). Symlink, not copy — the gguf stays mmap'd
  from the read-only squashfs instead of eating the 512M live tmpfs.
- `post_build.sh`: bundle the gguf(s) from `/root/llamaste-build/llm-models/` into
  the squashfs at `/opt/llamaste/models/` (reproducible; was a fragile manual file).
- Net: `select_model()`/`available_models()` now find the bundled Qwen2.5-0.5B and
  load it — chat works with no network and no model drive.

### Note
- The model *download* UI still flags 32B as "recommended" on big-RAM boxes
  (`recommend_model` by RAM). That's a "download this for more quality" hint, not
  the boot default — the boot loads the smallest on-disk model that fits.

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
