# Research 20: Update Mechanisms for Llamaste

## Context

Llamaste is a bootable Linux image where a single static C++ binary (`llamaste`) runs as PID 1. The system uses GRUB2, a squashfs root partition (read-only), and an ext4 data partition (models, config, conversations). This document researches how to deliver updates to such a system, covering both system updates (binary + kernel) and model updates (GGUF files), for devices that may or may not have internet access.

Updates are Phase 4, but the partition layout is Phase 1 -- so Phase 1 must accommodate future A/B updates from the start.

---

## 1. A/B Partition Update Frameworks

### 1.1 RAUC

**Architecture**: RAUC is a lightweight C-based update client (~512 KB binary) for embedded Linux. It uses "bundles" -- signed archives containing filesystem images and metadata. RAUC manages "slots" (partitions) that can be marked as active, inactive, or broken. On the host side, RAUC creates and signs bundles; on the target, it installs them.

**GRUB integration**: RAUC uses grubenv variables (`<slotname>_OK`, `<slotname>_TRY`, `ORDER`) to control which slot boots. An example GRUB config is provided in `contrib/grub.conf`. GRUB scripting is limited, so RAUC's example uses only one try per enabled slot.

**Bundle formats**: Three formats exist -- plain (legacy), verity (recommended, uses dm-verity for streaming verification), and crypt (adds encryption). The verity format enables HTTP streaming without intermediate storage and adaptive delta-like updates.

**Dependencies**: Minimum requires GLib (>=2.45.8) and OpenSSL (>=1.0). Optional: D-Bus (for service interface), libcurl (for network), libjson-glib (for JSON). The kernel needs CONFIG_SQUASHFS, CONFIG_BLK_DEV_LOOP, CONFIG_BLK_DEV_DM, CONFIG_DM_VERITY, CONFIG_CRYPTO_SHA256.

**Signing**: Mandatory by design -- bundles must always be cryptographically signed. Uses OpenSSL X.509 certificates, supports PKCS#11 tokens (HSMs).

**License**: LGPL-2.1 (static-linking-friendly).

**Pros**: Lightweight, mandatory signing, Buildroot support, GRUB integration, well-documented, active development.
**Cons**: Requires GLib (adds ~1-2 MB), D-Bus expected for service mode, no streaming in plain format, limited GRUB scripting.

### 1.2 SWUpdate

**Architecture**: SWUpdate is a C-based update framework (~1.3 MB binary) that acts as both an offline and OTA updater. It has an integrated web server for SOHO router-style updates and connects to Eclipse hawkBit for fleet management ("Suricatta mode"). It has an embedded Lua interpreter for extending functionality without code changes.

**Update strategies**: Supports double-copy (A/B) and single-rescue modes. In double-copy, the inactive partition is updated while the active one runs. In rescue mode, a minimal rescue image applies updates to the main partition.

**Streaming**: SWUpdate processes updates as a stream without temporary on-target storage -- a major advantage for RAM-constrained devices.

**hawkBit integration**: Eclipse hawkBit is a Java-based server for fleet OTA management. SWUpdate connects to it in "Suricatta mode" for centralized monitoring and control.

**Delta updates**: SWUpdate has explored delta update integration using binary diff tools, but notes that applying deltas is not possible for many projects due to RAM/storage constraints.

**License**: GPLv2.

**Pros**: Streaming support, integrated web server, Lua extensibility, hawkBit integration, rescue mode option, delta update support.
**Cons**: Larger binary than RAUC, GPLv2 license (not static-link-friendly), more complex configuration.

### 1.3 Mender

**Architecture**: Mender is a client-server OTA solution. The client was originally written in Go but is being rewritten in C++ to reduce footprint. The server is a microservices architecture using AWS S3 or Azure Storage. The client binary is ~6.9 MB (Go version).

**A/B partitions**: Mender mandates a dual A/B rootfs layout. Updates are atomic image-based deployments. If the new partition fails to boot, automatic rollback occurs.

**Artifact format**: Mender artifacts contain firmware, scripts, configuration, and metadata. Created with the `mender-artifact` tool from ext4 rootfs images.

**Signing**: Supports RSA (>=3072 bit) and ECDSA (P-256). The client verifies using an embedded public key.

**Standalone mode**: The client can run without a server, triggered via CLI -- useful for USB-delivered updates.

**License**: Apache 2.0 (client), proprietary server tiers.

**Pros**: Standalone mode, Apache license (client), professional server infrastructure, good documentation.
**Cons**: Large client binary (Go/C++), heavy server requirements, opinionated about partition layout, overkill for single-device use.

### 1.4 OSTree (libostree)

**Architecture**: OSTree is a content-addressed object store for filesystem trees -- essentially "Git for OS binaries." It stores complete filesystem snapshots and uses hardlinks for deduplication. Used by Fedora Silverblue, Fedora CoreOS, Fedora IoT, and Endless OS.

**Update mechanism**: Pulls a new filesystem tree from a remote repository. Only changed files are downloaded (content-addressed dedup). Upgrades are atomic -- the system reboots into the new tree. Rollback is instant by switching to the previous deployment.

**Immutable filesystem**: The root (`/`) is read-only. `/etc` and `/var` are writable. Symlinks redirect traditional paths.

**Integration**: Uses rpm-ostree as a hybrid image/package layer on Fedora. Can layer RPMs on top of base images.

**Pros**: Efficient dedup (only changed files downloaded), atomic upgrades, instant rollback, proven at scale (Fedora, RHEL CoreOS).
**Cons**: Designed for full Linux distributions (overkill for a single-binary OS), requires a filesystem tree structure, complex dependencies (libsoup, GLib, gpgme), not suited for squashfs image replacement.

### 1.5 Comparison Table

| Feature | RAUC | SWUpdate | Mender | OSTree |
|---------|------|----------|--------|--------|
| Binary size | ~512 KB | ~1.3 MB | ~6.9 MB | N/A (library) |
| Language | C | C | Go/C++ | C |
| License | LGPL-2.1 | GPLv2 | Apache 2.0 | LGPL-2.0+ |
| GRUB support | Yes (grubenv) | Yes (env) | No (U-Boot) | Yes (via grub2) |
| Streaming | Verity format | Yes | Yes | N/A |
| Mandatory signing | Yes | Configurable | Configurable | Yes (GPG) |
| Buildroot support | Yes | Yes | Partial | No |
| D-Bus required | For service | No | No | No |
| GLib required | Yes | No | No | Yes |
| Delta updates | Adaptive (verity) | Experimental | Server-side | Content-addressed |
| USB sideloading | Yes (bundle) | Yes (SWU) | Yes (standalone) | No |
| Fleet management | Via hawkBit | Via hawkBit | Built-in server | N/A |
| Complexity | Low-Medium | Medium | High | High |
| **Suitability for Llamaste** | **Best fit** | Good fit | Overkill | Wrong model |

### 1.6 Can Any Be Statically Linked Into Our Binary?

None of these frameworks are designed to be embedded as a library inside another binary. They all assume they run as separate processes/services:

- **RAUC** requires GLib, OpenSSL, and typically D-Bus -- substantial dependencies, but GLib and OpenSSL can be statically linked with musl. The D-Bus dependency can potentially be avoided if we call RAUC's install logic directly rather than through the service interface.
- **SWUpdate** has fewer hard dependencies but its GPLv2 license prevents static linking into a non-GPL binary.
- **Mender** is too large and opinionated.
- **OSTree** is architecturally incompatible (tree-based, not image-based).

**Verdict**: A custom implementation is the best path for Llamaste. We need a simple, purpose-built update mechanism that can be compiled directly into the llamaste binary. The logic is straightforward: verify signature, write squashfs to inactive partition, update grubenv, reboot. This is ~500-1000 lines of C++, far simpler than integrating any framework.

---

## 2. GRUB A/B Switching

### 2.1 How GRUB Knows Which Partition to Boot

GRUB uses a 1024-byte preallocated file called `grubenv` (typically at `/boot/grub/grubenv` or on the ESP). The `grub.cfg` script reads variables from this file using `load_env` and uses them to select the boot entry.

Key variables for A/B switching:

| Variable | Purpose |
|----------|---------|
| `saved_entry` | Default boot entry name or number |
| `next_entry` | One-time override (cleared after use) |
| `boot_success` | Set to 1 by userspace after successful boot |
| `boot_counter` | Countdown of remaining boot attempts |
| `ORDER` | RAUC-style: ordered list of slot names |
| `<slot>_OK` | RAUC-style: per-slot health flag |
| `<slot>_TRY` | RAUC-style: per-slot attempt counter |

### 2.2 Boot Counting for Automatic Rollback

The standard mechanism (used by Fedora, RAUC, Greenboot):

1. When a new system image is staged, `boot_counter` is set (e.g., to 3) and `boot_success` is set to 0.
2. At each boot, GRUB checks: if `boot_counter` exists AND `boot_success == 0`, decrement `boot_counter`.
3. If `boot_counter` reaches 0 or -1, GRUB switches to the fallback entry (the previous good system).
4. After successful boot, userspace sets `boot_success=1` (or unsets `boot_counter`).

**For Llamaste**, the boot success criteria would be:
- The llamaste binary starts and reaches its HTTP health endpoint (e.g., responds to `GET /health`).
- A watchdog timer (or systemd-equivalent logic in our PID 1) triggers after 60 seconds of no health response.
- On success: llamaste writes `boot_success=1` to grubenv.
- On failure: watchdog triggers reboot, boot_counter decrements, after N failures GRUB switches back.

**Recommended boot_counter value**: 3 (gives three tries before rollback). This handles transient failures (power glitch during first boot) without being too permissive.

### 2.3 Setting grubenv from Userspace

Two approaches:

**grub-editenv** (standard tool): `grub-editenv /boot/grub/grubenv set boot_success=1`. This is the official method but requires the grub-editenv binary (~50 KB). It does call `fsync()` but is **not fully atomic** -- power failure during write can corrupt the file.

**Direct file manipulation**: The grubenv file has a simple format -- a 1024-byte block starting with `# GRUB Environment Block` header, followed by `key=value` pairs padded with `#` characters. We can implement our own writer in ~100 lines of C++. The format is trivial to parse and write.

### 2.4 Power Failure Safety

The grubenv write is **not atomic** by default. `grub-editenv` does `fsync()` but does not use rename-over (the standard atomic write pattern). On ext4 with `data=writeback`, a power failure can produce a zero-length or corrupted grubenv.

**Mitigations for Llamaste**:
1. Store grubenv on the ESP (FAT32), which is simpler and less prone to metadata ordering issues.
2. Implement our own atomic writer: write to `grubenv.tmp`, `fsync()`, then rename over `grubenv`. On FAT32, this is reasonably safe.
3. Keep a backup copy of grubenv (`grubenv.bak`) that GRUB can fall back to if the primary is corrupted.
4. Use a simple GRUB script that defaults to slot A if grubenv is missing/corrupt.

### 2.5 Llamaste GRUB Configuration (Example)

```grub
# /boot/grub/grub.cfg for Llamaste A/B boot

set default=0
set timeout=5
load_env

# Boot counter logic
if [ "${boot_counter}" ]; then
    if [ "${boot_success}" = "0" ]; then
        if [ "${boot_counter}" = "0" ]; then
            # Exhausted attempts -- switch to other slot
            if [ "${active_slot}" = "A" ]; then
                set default=1
            else
                set default=0
            fi
            set boot_counter=-1
            save_env boot_counter
        else
            # Decrement counter
            # (GRUB math is limited; use pre-computed values)
            if [ "${boot_counter}" = "3" ]; then set boot_counter=2; fi
            if [ "${boot_counter}" = "2" ]; then set boot_counter=1; fi
            if [ "${boot_counter}" = "1" ]; then set boot_counter=0; fi
            save_env boot_counter
        fi
    fi
fi

# Override with active_slot if set
if [ "${active_slot}" = "B" ]; then
    set default=1
fi

menuentry "Llamaste A" {
    linux /vmlinuz-A root=PARTUUID=<uuid-A> ro llamaste.slot=A
    # initrd not needed (built-in initramfs or direct mount)
}

menuentry "Llamaste B" {
    linux /vmlinuz-B root=PARTUUID=<uuid-B> ro llamaste.slot=B
}
```

---

## 3. Squashfs Update Strategy

### 3.1 Full Image Replacement (Recommended for Llamaste)

The simplest and most reliable approach: download the new squashfs image, write it to the inactive partition, verify, switch boot.

**Process**:
1. Download new squashfs image (15-50 MB compressed) to DATA partition staging area.
2. Verify Ed25519 signature of the downloaded image.
3. Verify SHA-256 hash matches the manifest.
4. Write squashfs to the inactive SYSTEM partition (`dd` equivalent).
5. Verify written data by re-reading and checking hash.
6. Update grubenv to point to the new slot.
7. Set `boot_counter=3`, `boot_success=0`.
8. Reboot (or wait for user confirmation).

**Size considerations**: The llamaste squashfs (kernel + binary + web UI + configs) will be approximately 15-30 MB. At typical broadband speeds (10+ Mbps), this downloads in under 30 seconds. Even on slow connections (1 Mbps), it completes in about 4 minutes. This is small enough that full image replacement is practical.

### 3.2 Delta/Binary Diff Updates

Delta updates would reduce download size from ~20 MB to ~1-5 MB. However, the complexity is significant:

**bsdiff**: Produces the smallest patches (50-80% smaller than xdelta) but requires 17N bytes of RAM where N is the old image size. For a 30 MB image, that is ~500 MB RAM -- potentially most of the system's available memory.

**xdelta3**: Linear time/space complexity but still requires loading images into RAM. More practical than bsdiff but patches are larger.

**Deterministic squashfs**: For binary diffing to work, `mksquashfs` must produce identical output from identical input. By default it does not (timestamps, uninitialized memory). Requires patched mksquashfs with forced dates and zeroed padding.

**Recommendation**: Skip delta updates for Phase 4. The full squashfs image is small enough (15-30 MB) that delta updates add complexity without enough benefit. Revisit if the system image grows significantly larger or if bandwidth-constrained use cases emerge.

### 3.3 Integrity Verification

**Before reboot** (critical -- never boot an unverified image):
1. **Ed25519 signature** on the update manifest (contains SHA-256 hash of squashfs).
2. **SHA-256 hash** of the squashfs image, verified after download and again after writing to partition.
3. The public key is embedded in the llamaste binary itself (compiled in) and also stored on the read-only squashfs.

### 3.4 A/B vs In-Place Updates

**In-place update** (single SYSTEM partition): Would require unmounting the active root filesystem, which is impossible since it is the running OS. Even with a pivot_root trick, this is fragile and dangerous.

**A/B update** (two SYSTEM partitions): The only safe approach. Write the new image to the inactive partition while the active one continues running. Adds ~256 MB to disk usage but provides atomic updates and instant rollback.

**Verdict**: A/B squashfs partitions are mandatory for safe updates.

---

## 4. Offline Updates (USB Sideloading)

### 4.1 Workflow

1. User visits `llamaste.dev/updates` on another computer.
2. Downloads the update bundle file (e.g., `llamaste-1.2.0.update`).
3. Copies it to a USB stick (any filesystem: FAT32, exFAT, ext4).
4. Plugs USB stick into the Llamaste machine.
5. Llamaste detects the USB, validates the bundle, shows details in web UI.
6. User confirms update (or LLM auto-applies if configured).
7. Update is applied; system reboots to new version.

### 4.2 USB Detection

The llamaste binary, as PID 1, handles device hotplug via `uevent` from the kernel (netlink socket for kobject notifications). When a USB mass storage device appears:
1. Kernel sends uevent with `ACTION=add`, `SUBSYSTEM=block`.
2. Llamaste mounts the USB partition read-only to `/tmp/usb-mount`.
3. Scans for files matching `llamaste-*.update` pattern.
4. If found, validates signature and displays update info in web UI and chat.

No udev daemon is needed -- the llamaste binary handles this directly with a netlink socket listener, which is consistent with the "binary IS the OS" architecture.

### 4.3 Update Bundle Format

```
llamaste-1.2.0.update (a tar archive, not compressed -- contents are already compressed)
├── manifest.json          # Version, target arch, SHA-256 hashes, changelog
├── manifest.json.sig      # Ed25519 signature of manifest.json
├── system.squashfs        # New root filesystem image (if system update)
├── vmlinuz                # New kernel (if kernel update, optional)
└── models/                # Model files (optional, for model-only updates)
    └── qwen2.5-7b-q4_k_m.gguf
```

**manifest.json** example:
```json
{
  "version": "1.2.0",
  "build_date": "2026-03-15T10:30:00Z",
  "arch": "x86-64",
  "min_version": "1.0.0",
  "components": {
    "system": {
      "file": "system.squashfs",
      "sha256": "abc123...",
      "size": 28311552
    },
    "kernel": {
      "file": "vmlinuz",
      "sha256": "def456...",
      "size": 8388608
    }
  },
  "changelog": [
    "Improved network auto-configuration",
    "Added support for Llama 3.2 models",
    "Fixed memory leak in conversation history"
  ]
}
```

### 4.4 Security Against Malicious USB Bundles

1. **Ed25519 signature verification**: The manifest must be signed with a key whose public counterpart is compiled into the llamaste binary. No valid signature = no installation.
2. **Version checking**: `min_version` prevents downgrade attacks (cannot install an older, vulnerable version).
3. **Architecture checking**: Prevents installing an ARM image on x86 or vice versa.
4. **Read-only mount**: The USB is always mounted read-only to prevent any writes.
5. **No executable code on USB**: The bundle contains only data (images, models) -- never scripts or executables that get run.
6. **User confirmation**: By default, updates require explicit confirmation through the web UI or chat. An "auto-update from USB" mode can be enabled in settings but is off by default.

### 4.5 Trigger Methods

- **Auto-detect**: Llamaste detects USB insertion and shows notification in web UI and chat.
- **Web UI button**: "Check USB for Updates" button on the system settings page.
- **LLM chat**: User says "Check for updates on USB" or "Update from USB drive."
- **Boot-time check**: On every boot, check if a USB drive with an update bundle is present (useful for headless setups).

---

## 5. Online Updates

### 5.1 Update Server

**GitHub Releases** (recommended for Phase 4): Simple, free, CDN-backed, well-understood.
- Release URL: `https://github.com/llamaste-os/llamaste/releases/latest`
- Check URL: `https://api.github.com/repos/llamaste-os/llamaste/releases/latest`
- Download URLs: Direct links to release assets.

**Advantages**: No server infrastructure to maintain, global CDN, existing tooling, community trust.
**Disadvantages**: GitHub API rate limits (60/hour unauthenticated), requires GitHub account for publishing.

**Alternative** (future): Self-hosted update server at `updates.llamaste.dev` with a simple JSON API.

### 5.2 Update Check Flow

```
1. llamaste periodically checks (every 24 hours, configurable):
   GET https://api.github.com/repos/llamaste-os/llamaste/releases/latest
   → Parse JSON for tag_name (version), assets (download URLs)

2. Compare remote version with current version (semver).

3. If newer version available:
   → Show notification in web UI: "Update 1.2.0 available"
   → LLM mentions it in chat: "A system update is available (v1.2.0)..."
   → Display changelog

4. User approves (or auto-update if configured):
   → Download update bundle to /data/updates/staging/
   → Verify signature
   → Apply update (write to inactive partition)
   → Prompt for reboot
```

### 5.3 Download Resilience

- **Resume interrupted downloads**: Use HTTP Range headers. Store partial download with `.part` suffix. On resume, send `Range: bytes=<offset>-` header.
- **Integrity of partial downloads**: Check final SHA-256 after download completes. If mismatch, re-download from scratch.
- **Bandwidth awareness**: Allow user to set download speed limit in config. Default: unlimited. Option: "Download only on schedule" (e.g., overnight).
- **Storage management**: Staging area on DATA partition. Require at least 2x the update size free. Clean up old staging files on boot.

### 5.4 Background Download

The llamaste binary can download updates in a background thread while continuing to serve LLM requests. Since the download goes to the DATA partition (ext4, writable), it does not interfere with the running system on the SYSTEM partition (squashfs, read-only).

### 5.5 Update Notification

- **Web UI**: Banner at top of page: "Update v1.2.0 available. [View changelog] [Update now]"
- **LLM chat**: When user starts a conversation, the LLM can mention: "By the way, a system update (v1.2.0) is available. It includes improved network configuration and Llama 3.2 support. Would you like me to install it?"
- **API**: `GET /api/system/update/status` returns current version, available version, download progress.

### 5.6 Auto-Update vs Manual Approval

**Default**: Manual approval. Show notification, wait for user action.
**Optional**: Auto-update mode (enabled in settings). Downloads and stages the update automatically, but still requires reboot confirmation (or auto-reboot at a configured time, e.g., 3 AM if no active sessions).

---

## 6. Model Updates

### 6.1 Different from System Updates

Models are fundamentally different from system updates:

| Aspect | System Update | Model Update |
|--------|--------------|--------------|
| Size | 15-30 MB | 1-20+ GB |
| Location | SYSTEM partition (squashfs) | DATA partition (ext4) |
| Frequency | Monthly/quarterly | As needed |
| Rollback | A/B partition swap | Keep old file until verified |
| Downtime | Requires reboot | Hot-swap possible |
| Verification | Signature + hash | Hash only (models are public) |

### 6.2 Model Download (Already Planned)

The `model.download` tool already exists in the Llamaste design. It downloads GGUF files from Hugging Face to `/data/models/`. This same mechanism handles model updates.

### 6.3 Model Version Tracking

A `models.json` file on the DATA partition tracks installed models:
```json
{
  "models": [
    {
      "name": "qwen2.5-7b-instruct",
      "file": "qwen2.5-7b-instruct-q4_k_m.gguf",
      "version": "1.0",
      "sha256": "abc123...",
      "size": 4368438272,
      "installed": "2026-03-01T12:00:00Z",
      "source": "huggingface:Qwen/Qwen2.5-7B-Instruct-GGUF"
    }
  ]
}
```

### 6.4 Model Rollback

Since models live on the writable DATA partition, rollback is simple:
1. Before replacing a model, rename the old file: `model.gguf` -> `model.gguf.prev`.
2. Download or copy the new model file.
3. Verify SHA-256 hash.
4. If verification passes, reload the model (requires briefly stopping inference).
5. If the new model fails to load, rename `.prev` back.
6. Once confirmed working, delete `.prev` (or keep it if disk space allows).

### 6.5 Should Models Use the Same Mechanism as System Updates?

**No.** Keep them separate:
- System updates use the A/B partition mechanism (squashfs replacement + reboot).
- Model updates use file-level operations on the DATA partition (download + verify + hot-swap).
- An update bundle CAN include models (for USB sideloading), but the mechanisms are distinct.
- The manifest.json in the update bundle has a `models` section that is optional.

### 6.6 Model Integrity Verification

- **SHA-256 hash**: Published alongside model files on Hugging Face. Verified after download.
- **No signature**: Models are public artifacts from trusted sources (Hugging Face, official mirrors). Signing would require maintaining a separate model signing infrastructure, which is not practical for community models.
- **Source pinning**: The `models.json` file records the source URL. Models can only be updated from the same source or a configured trusted source list.

---

## 7. Rollback Mechanisms

### 7.1 Automatic Rollback (Boot Counter)

**Trigger**: Three consecutive failed boots.

**Mechanism**:
1. After applying an update, set `boot_counter=3` and `boot_success=0` in grubenv.
2. Reboot into the new SYSTEM partition.
3. GRUB decrements `boot_counter` on each boot where `boot_success=0`.
4. The llamaste binary performs a health check on startup:
   - Mount all filesystems successfully.
   - Load the model file.
   - Start the HTTP server.
   - Respond to `GET /health` with 200 OK.
5. If all health checks pass (within 120 seconds), set `boot_success=1` in grubenv.
6. If the binary crashes, the kernel panics, or health checks fail within 120 seconds, the watchdog triggers a reboot. After 3 failures, GRUB boots the previous slot.

**Timeline**:
```
Boot 1: boot_counter=3 → health check fails → reboot
Boot 2: boot_counter=2 → health check fails → reboot
Boot 3: boot_counter=1 → health check fails → reboot
Boot 4: boot_counter=0 → GRUB switches to previous slot → boots old system
```

### 7.2 Manual Rollback

- **Web UI**: "System > Rollback to previous version" button. Sets `active_slot` to the other slot in grubenv and reboots.
- **LLM chat**: User says "Roll back to the previous version." The LLM confirms and executes.
- **GRUB menu**: Advanced option "Llamaste (previous)" always available as a manual escape hatch.

### 7.3 Health Check Definition

A boot is "successful" when ALL of these are true:
1. Root filesystem mounted (squashfs).
2. DATA partition mounted (ext4).
3. At least one model file accessible.
4. HTTP server listening on port 3000.
5. `/health` endpoint returns 200 with `{"status": "ok"}`.
6. First inference request completes (optional, adds ~5-30 seconds).

This check runs within 120 seconds of boot. If any step fails, the boot is considered failed.

### 7.4 Rollback Speed

Rollback is effectively instant -- it is just a grubenv variable change plus a reboot. The reboot itself takes 5-15 seconds depending on hardware. Total rollback time: under 20 seconds.

### 7.5 Model Rollback

Models can be rolled back independently of the system:
- If a `.prev` file exists, swap it back.
- If not, re-download the previous version (requires network).
- Model rollback does not require reboot -- just a model reload (1-5 seconds for small models, 10-30 seconds for large ones).

---

## 8. Signature Verification

### 8.1 Ed25519 vs RSA

| Property | Ed25519 | RSA-3072 |
|----------|---------|----------|
| Security level | ~128 bits | ~128 bits |
| Public key size | 32 bytes | 384 bytes |
| Signature size | 64 bytes | 384 bytes |
| Sign speed | Very fast | Slow |
| Verify speed | Fast | Moderate |
| Deterministic | Yes (no RNG needed) | No (needs good RNG) |
| Side-channel resistance | By design | Requires careful impl |
| FIPS approved | Yes (FIPS 186-5, 2023) | Yes |
| Implementation complexity | Low | High (padding, etc.) |
| Post-quantum | Broken (same as RSA) | Broken |

**Recommendation**: Ed25519. It is smaller, faster, deterministic (critical for embedded -- no reliance on RNG quality), simpler to implement correctly, and FIPS-approved. The only argument for RSA is legacy compatibility, which does not apply to Llamaste.

### 8.2 Key Storage

- **Primary**: Ed25519 public key is compiled into the llamaste binary as a `constexpr` byte array. This is the root of trust.
- **Backup**: Same public key is also stored on the read-only squashfs at `/etc/llamaste/update-key.pub`.
- **The private key** never touches a Llamaste device. It lives on the build server (or HSM) and signs bundles during the release process.

### 8.3 Key Rotation

Key rotation is the hardest problem in update signing. Approaches:

**Approach 1 -- Dual-key transition** (recommended):
1. Embed TWO public keys in the binary: the current key and a "next" key.
2. Bundles are signed with the current key.
3. When rotating, publish a bundle signed with the current key that contains the new binary with an updated key pair (new current + new next).
4. Devices that install this update now trust the new key.
5. Subsequent bundles are signed with the new key.

**Approach 2 -- Key list in signed manifest**:
1. The manifest includes a `trusted_keys` list, signed by the current key.
2. New keys can be added to the list in a signed update.
3. More flexible but more complex.

**Recommendation**: Approach 1 (dual-key) for simplicity. The binary always knows two keys: the one that signed its current image and the one that will sign the next.

### 8.4 Chain of Trust

```
Build Server (private key on HSM or secure machine)
    │
    │ signs
    ▼
Update Bundle (manifest.json.sig)
    │
    │ verified by
    ▼
Llamaste Binary (embedded public key)
    │
    │ if valid, writes to
    ▼
Inactive SYSTEM Partition
```

### 8.5 User-Built Updates

Users who build Llamaste from source can generate their own signing keys:
```bash
# Generate keypair
openssl genpkey -algorithm ed25519 -out llamaste-update.key
openssl pkey -in llamaste-update.key -pubout -out llamaste-update.pub

# Build llamaste with custom public key
cmake -DUPDATE_PUBKEY_FILE=llamaste-update.pub ..
```

The custom public key is embedded at compile time. Only bundles signed with the matching private key will be accepted.

---

## 9. Partition Layout Implications

### 9.1 Current Layout (Phase 1 Plan)

```
Disk: /dev/sda (or /dev/nvme0n1)
┌──────────────────────────────────────────────────────────────┐
│ GPT Header                                                    │
├──────┬───────────┬────────┬──────────┬────────────────────────┤
│ P1   │ P2        │ P3     │ P4       │                        │
│ BIOS │ ESP       │ SYSTEM │ DATA     │ (unpartitioned)        │
│ BOOT │           │        │          │                        │
│ 1 MB │ 128 MB    │ 256 MB │ remainder│                        │
│ raw  │ FAT32     │ sqfs   │ ext4     │                        │
├──────┴───────────┴────────┴──────────┴────────────────────────┤
│ GPT Footer                                                    │
└──────────────────────────────────────────────────────────────┘
```

**Problem**: Only one SYSTEM partition -- no room for A/B updates without repartitioning.

### 9.2 Recommended A/B Layout (Phase 1, future-proof)

```
Disk: /dev/sda (or /dev/nvme0n1)
┌──────────────────────────────────────────────────────────────┐
│ GPT Header                                                    │
├──────┬───────────┬────────┬────────┬──────────────────────────┤
│ P1   │ P2        │ P3     │ P4     │ P5                       │
│ BIOS │ ESP       │ SYS-A  │ SYS-B  │ DATA                    │
│ BOOT │           │        │        │                          │
│ 1 MB │ 256 MB    │ 256 MB │ 256 MB │ remainder                │
│ raw  │ FAT32     │ sqfs   │ sqfs   │ ext4                     │
├──────┴───────────┴────────┴────────┴──────────────────────────┤
│ GPT Footer                                                    │
└──────────────────────────────────────────────────────────────┘

Total overhead vs current plan: +256 MB (SYS-B) + 128 MB (larger ESP)
```

**Changes from current plan**:
1. **ESP enlarged to 256 MB**: Holds GRUB, two kernels (vmlinuz-A, vmlinuz-B), grubenv, and GRUB config. Two kernels at ~8 MB each = ~16 MB; the rest is headroom for future use.
2. **SYS-A + SYS-B**: Two squashfs partitions of 256 MB each. Only one is active at a time.
3. **DATA stays the same**: Models, config, conversations, update staging area.

**Total fixed overhead**: 1 + 256 + 256 + 256 = 769 MB (vs 385 MB current plan). On a typical 32 GB USB drive, this leaves 31+ GB for DATA. On a 16 GB drive, 15+ GB for DATA. Perfectly acceptable.

### 9.3 Kernel Updates

The kernel lives on the ESP (FAT32), not in the squashfs. For A/B kernel updates:

**Option A -- Two kernel files on ESP** (recommended): Store `vmlinuz-A` and `vmlinuz-B` on the ESP. GRUB selects the correct one based on the active slot. When updating, write the new kernel as the inactive slot's file.

**Option B -- Kernel inside squashfs**: Bundle the kernel into the squashfs image. GRUB would need to read from squashfs, which it supports but is less common.

**Option C -- A/B ESP**: Two ESP partitions. Overkill and wastes space. ChromeOS does this but ChromeOS has special firmware integration.

**Recommendation**: Option A. The ESP is FAT32 and writable. Two kernel files (~16 MB total) are trivial. GRUB already knows how to load from FAT32. This keeps the ESP as a single partition and avoids complexity.

### 9.4 How Other Systems Handle This

**ChromeOS**: Uses paired KERN-A/KERN-B + ROOT-A/ROOT-B partitions (4 partitions for A/B). Kernel partitions are 16 MB each. GRUB or depthcharge firmware selects based on partition priority attributes. EFI System Partition is partition 12 (separate from kernel partitions).

**OpenWrt** (community A/B projects): Replaces the single rootfs partition with OpenWrt-A and OpenWrt-B. An `abupgrade` script downloads the rootfs to the inactive partition, copies config, and updates GRUB. The partition name (GPT label) is used for detection.

**Raspberry Pi OS**: Single partition layout by default (no A/B). OTA-capable setups use one autoboot + two boot + two root partitions. The bootloader's `tryboot.txt` mechanism provides one-shot boot override. Watchdog-triggered failover changes the boot partition.

**Fedora Silverblue/CoreOS**: Uses OSTree -- a single partition with content-addressed trees. Multiple deployments coexist via hardlinks. GRUB boot counter + Greenboot health checks for rollback. No A/B partitions needed because deployments share storage.

---

## 10. Web UI Integration

### 10.1 Update Status Page

The web UI should include a "System" page with an update section:

```
┌─────────────────────────────────────────────────┐
│  System Update                                    │
│                                                   │
│  Current version: 1.1.0 (2026-02-15)             │
│  Active slot: A                                   │
│  Previous slot: B (v1.0.0, healthy)               │
│                                                   │
│  ┌─────────────────────────────────────────────┐ │
│  │ ✓ Update available: v1.2.0                  │ │
│  │                                             │ │
│  │ Changes:                                    │ │
│  │ - Improved network auto-configuration       │ │
│  │ - Added Llama 3.2 model support             │ │
│  │ - Fixed memory leak in conversation history │ │
│  │                                             │ │
│  │ Size: 22 MB | Released: 2026-03-15          │ │
│  │                                             │ │
│  │ [Download & Install]  [Remind me later]     │ │
│  └─────────────────────────────────────────────┘ │
│                                                   │
│  Update history:                                  │
│  v1.1.0 - 2026-02-15 - Installed via USB         │
│  v1.0.0 - 2026-01-01 - Initial install           │
│                                                   │
│  [Rollback to v1.0.0]  [Check USB for updates]   │
└─────────────────────────────────────────────────┘
```

### 10.2 Progress Display

During download and installation:
```
┌─────────────────────────────────────────────────┐
│  Updating to v1.2.0...                            │
│                                                   │
│  Step 1/4: Downloading update         [====  ] 67%│
│            14.7 MB / 22.0 MB  (2.1 MB/s)         │
│                                                   │
│  Step 2/4: Verifying signature        [pending]   │
│  Step 3/4: Writing to system B        [pending]   │
│  Step 4/4: Verifying written image    [pending]   │
│                                                   │
│  [Cancel download]                                │
│                                                   │
│  ⚠ The system will need to reboot after update.  │
│    All active conversations will be saved.         │
└─────────────────────────────────────────────────┘
```

### 10.3 USB Update Workflow

When a USB drive with an update bundle is detected:
```
┌─────────────────────────────────────────────────┐
│  USB Update Detected                              │
│                                                   │
│  Found: llamaste-1.2.0.update on USB drive        │
│  Signature: Valid (signed by Llamaste official)    │
│  Version: 1.2.0 (current: 1.1.0)                 │
│                                                   │
│  Changes:                                         │
│  - Improved network auto-configuration            │
│  - Added Llama 3.2 model support                  │
│                                                   │
│  [Install update]  [Dismiss]                      │
└─────────────────────────────────────────────────┘
```

### 10.4 LLM Chat Integration

The LLM should be aware of update status and able to answer questions:

- "Is there an update available?" -- "Yes, version 1.2.0 is available. It includes improved network configuration and Llama 3.2 support. Would you like me to install it?"
- "What version am I running?" -- "You're running Llamaste v1.1.0, installed on February 15th."
- "Roll back to the previous version" -- "I'll switch back to version 1.0.0 on system slot B. This requires a reboot. Ready to proceed?"
- "What changed in the last update?" -- "Version 1.1.0 added voice input support and fixed a bug in file search."

This is implemented via system tools: `system.update_check`, `system.update_install`, `system.rollback`, `system.version`.

---

## 11. Recommendations for Llamaste

### 11.1 Partition Layout Changes for Phase 1

**The Phase 1 partition layout MUST change** to accommodate future A/B updates:

```
Current plan:  BIOS(1MB) + ESP(128MB) + SYSTEM(256MB) + DATA(remainder)
Recommended:   BIOS(1MB) + ESP(256MB) + SYS-A(256MB) + SYS-B(256MB) + DATA(remainder)
```

Even though updates are Phase 4, the partition layout is baked into the disk image and cannot be changed without a full re-flash. The cost is only ~384 MB of additional space (larger ESP + second SYSTEM partition), which is negligible on any target device (minimum 8 GB).

**Phase 1 implementation**: SYS-B is created but left empty (or contains a copy of SYS-A). GRUB only references SYS-A. The update infrastructure is not implemented yet.

### 11.2 Recommended Update Framework

**Custom implementation** (not RAUC, SWUpdate, or Mender).

Rationale:
- Llamaste is a single binary that IS the OS. External update frameworks expect to run alongside other services.
- Our update logic is simple: verify signature, write squashfs, update grubenv, reboot.
- We are already statically linked with musl -- adding GLib/D-Bus/libcurl dependencies for RAUC is undesirable.
- The update code is ~500-1000 lines of C++ compiled into the llamaste binary.
- We control the entire stack from bootloader to application, so we do not need framework abstractions.

**What we borrow from the frameworks**:
- RAUC's GRUB integration pattern (grubenv variables for slot selection).
- RAUC's bundle concept (signed archive with manifest + images).
- SWUpdate's web server integration (progress reporting via SSE).
- Mender's standalone mode concept (USB/offline updates without server).
- Greenboot's health check pattern (scripted boot success criteria).

### 11.3 Phase 1 Minimum (Version Checking + Manual Re-flash)

What to implement in Phase 1:
1. **Version embedded in binary**: `constexpr const char* LLAMASTE_VERSION = "1.0.0";`
2. **`/api/system/version` endpoint**: Returns current version in JSON.
3. **Web UI displays version**: Footer of every page shows "Llamaste v1.0.0".
4. **A/B partition layout**: Created by genimage but not actively used yet.
5. **grubenv setup**: Basic grubenv with `active_slot=A`, read/written by llamaste binary.
6. **Update tool stubs**: `system.update_check` and `system.update_install` tools exist but return "Updates not yet available -- re-flash to update."

### 11.4 Phase 4 Full Implementation

What to implement in Phase 4:
1. **Ed25519 signature verification** (using a lightweight Ed25519 library like TweetNaCl or libsodium's minimal build).
2. **Update bundle parser**: Reads the tar-based `.update` format, extracts manifest, verifies signature.
3. **A/B partition writer**: Writes squashfs to inactive partition, writes kernel to ESP.
4. **grubenv manager**: Atomic grubenv writer with boot counter support.
5. **Health check on boot**: Verify all critical systems before marking boot_success=1.
6. **USB sideloading**: Netlink listener for USB hotplug, auto-detection of update bundles.
7. **Online update checker**: HTTPS client (already needed for model downloads) checks GitHub Releases API.
8. **Background downloader**: Threaded download with resume support.
9. **Web UI update page**: Status, progress, changelog, rollback button.
10. **LLM tool integration**: `system.update_check`, `system.update_install`, `system.rollback`.
11. **Rollback via GRUB**: Boot counter logic in grub.cfg, manual rollback via web UI/chat.

### 11.5 Update Bundle Format Specification

```
File: llamaste-<version>.update
Format: POSIX tar (uncompressed -- contents are pre-compressed)
Max size: 2 GB (practical limit for FAT32 USB drives; actual ~30-50 MB for system-only)

Required files:
  manifest.json          - Update metadata (JSON)
  manifest.json.sig      - Ed25519 signature of manifest.json (64 bytes raw)

Optional files (at least one required):
  system.squashfs        - Root filesystem image for SYSTEM partition
  vmlinuz                - Linux kernel binary

Optional files (truly optional):
  models/<name>.gguf     - Model files for DATA partition

manifest.json schema:
{
  "format_version": 1,
  "version": "<semver>",
  "build_date": "<ISO 8601>",
  "arch": "x86-64" | "aarch64",
  "min_version": "<semver>",       // Minimum installed version required
  "components": {
    "system": {                     // Optional
      "file": "system.squashfs",
      "sha256": "<hex>",
      "size": <bytes>
    },
    "kernel": {                     // Optional
      "file": "vmlinuz",
      "sha256": "<hex>",
      "size": <bytes>
    }
  },
  "models": [                       // Optional
    {
      "file": "models/<name>.gguf",
      "sha256": "<hex>",
      "size": <bytes>
    }
  ],
  "changelog": ["<string>", ...],
  "signing_key_next": "<base64>"    // Optional: next public key for key rotation
}
```

### 11.6 Implementation Complexity Estimate

| Component | Lines of C++ (est.) | Phase |
|-----------|---------------------|-------|
| Ed25519 verify (TweetNaCl) | ~300 (vendored) | 4 |
| Update bundle parser | ~200 | 4 |
| Partition writer | ~150 | 4 |
| grubenv reader/writer | ~150 | 1 (basic), 4 (full) |
| Health check system | ~100 | 4 |
| USB hotplug listener | ~200 | 4 |
| Online update checker | ~150 | 4 |
| Background downloader | ~200 | 4 |
| Web UI update page | ~300 (JS/HTML) | 4 |
| LLM tool handlers | ~100 | 4 |
| **Total** | **~1,850** | |

This is a manageable amount of code -- about the size of a single medium-complexity source file. The heaviest dependency (Ed25519) can be satisfied by vendoring TweetNaCl (a single ~800 line C file that implements the full NaCl API including Ed25519).

---

## References

- RAUC documentation: https://rauc.readthedocs.io/en/stable/integration.html
- RAUC GitHub: https://github.com/rauc/rauc
- SWUpdate documentation: https://sbabic.github.io/swupdate/swupdate.html
- SWUpdate delta updates: https://sbabic.github.io/swupdate/delta-update.html
- Mender OTA overview: https://mender.io/blog/ota-update-embedded-linux
- Mender GitHub: https://github.com/mendersoftware/mender
- OSTree / Fedora Silverblue: https://fedoramagazine.org/pieces-of-fedora-silverblue/
- FOSDEM 2025 A/B update comparison: https://archive.fosdem.org/2025/events/attachments/fosdem-2025-6299-exploring-open-source-dual-a-b-update-solutions-for-embedded-linux/
- GNU GRUB grubenv format: https://www.gnu.org/software/grub/manual/grub/html_node/Environment-block.html
- Greenboot health checks: https://fedoraproject.org/wiki/Changes/Greenboot_RS_Change_Proposal
- ChromeOS disk format: https://chromium.googlesource.com/chromiumos/docs/+/refs/heads/stabilize-rust-14220.B/disk_format.md
- OpenWrt A/B partitions: https://github.com/eth-p/openwrt-abpp
- bsdiff: https://www.daemonology.net/bsdiff/
- xdelta3: https://github.com/jmacd/xdelta
- Squashfs delta updates (thesis): https://www.diva-portal.org/smash/get/diva2:1538229/FULLTEXT01.pdf
- Ed25519 for IoT (Foundries.io): https://www.foundries.io/insights/blog/tuf-ed25519-support/
- Secure firmware updates (Memfault): https://interrupt.memfault.com/blog/secure-firmware-updates-with-code-signing
- OTA update checklist: https://memfault.com/blog/ota-update-checklist-for-embedded-devices/
- OTA design patterns: https://arshon.com/blog/firmware-over-the-air-ota-updates-design-patterns-pitfalls-and-a-playbook-you-can-ship/
- musl libc: https://musl.libc.org/about.html
- fwup (firmware update tool): https://github.com/fwup-home/fwup
- Raspberry Pi boot config: https://www.raspberrypi.com/documentation/computers/config_txt.html
- Embedded update strategies overview: https://mkrak.org/2018/01/10/updating-embedded-linux-devices-part1/
- Embedded update comparison (embedded.com): https://www.embedded.com/ota-updates-for-embedded-linux-part-2-a-comparison-of-off-the-shelf-update-systems/
