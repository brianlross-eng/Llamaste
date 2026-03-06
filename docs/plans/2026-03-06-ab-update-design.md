# Phase 4: A/B Update System — Design Document

**Date**: 2026-03-06
**Status**: Design
**Estimated LOC**: ~1,000 C++ + ~200 JS/HTML
**Reference**: `research/20-update-mechanisms.md` (detailed research)

---

## 1. Overview

Llamaste uses an A/B partition scheme for atomic system updates with automatic rollback. The SYS-B partition (partition 4, 256MB) has been reserved since Phase 1. This phase activates it.

**Update flow**: Download update bundle → verify Ed25519 signature → write squashfs to inactive partition → update kernel on ESP → set grubenv to boot new slot → reboot → health check → mark success (or auto-rollback after 3 failures).

---

## 2. Partition Layout (Already Exists)

```
Part  Device   Label      FS         Size      Purpose
1     sda1     BIOS-BOOT  raw        1 MB      GRUB core.img
2     sda2     ESP        FAT16      256 MB    GRUB + kernel(s) + grubenv
3     sda3     SYS-A      squashfs   256 MB    Root filesystem (slot A)
4     sda4     SYS-B      squashfs   256 MB    Root filesystem (slot B, currently empty)
5     sda5     DATA       ext4       remainder Models, config, conversations
```

No partition layout changes needed — this was designed in Phase 1.

---

## 3. GRUB A/B Boot Switching

### 3.1 grubenv Variables

The grubenv file lives on the ESP at `/boot/grub/grubenv` (or `/EFI/BOOT/grubenv`). It's a 1024-byte file with a fixed header and `key=value` pairs padded with `#`.

| Variable | Values | Purpose |
|----------|--------|---------|
| `active_slot` | `A` or `B` | Which SYS partition to boot |
| `boot_success` | `0` or `1` | Whether current boot succeeded |
| `boot_counter` | `3`,`2`,`1`,`0` | Remaining boot attempts before rollback |

### 3.2 grub.cfg (Replaces Current)

```grub
set timeout=3
set default=0

# Load environment
load_env

# Default to slot A if no variable set
if [ -z "${active_slot}" ]; then
    set active_slot=A
fi

# Boot counter rollback logic
if [ "${boot_success}" = "0" -a -n "${boot_counter}" ]; then
    if [ "${boot_counter}" = "0" ]; then
        # Exhausted attempts — switch to other slot
        if [ "${active_slot}" = "A" ]; then
            set active_slot=B
        else
            set active_slot=A
        fi
        save_env active_slot
        set boot_counter=
        save_env boot_counter
    else
        # Decrement counter (GRUB has no math — use if chain)
        if [ "${boot_counter}" = "3" ]; then set boot_counter=2; fi
        if [ "${boot_counter}" = "2" ]; then set boot_counter=1; fi
        if [ "${boot_counter}" = "1" ]; then set boot_counter=0; fi
        save_env boot_counter
    fi
fi

# Select root partition based on slot
if [ "${active_slot}" = "B" ]; then
    set rootpart=/dev/sda4
else
    set rootpart=/dev/sda3
fi

menuentry "Llamaste Server" {
    linux /bzImage root=${rootpart} rootfstype=squashfs ro quiet \
        console=tty0 console=ttyS0,115200 \
        init=/opt/llamaste/llamaste \
        llamaste.mode=server llamaste.slot=${active_slot} \
        ip=dhcp
}

menuentry "Llamaste Desktop" {
    linux /bzImage root=${rootpart} rootfstype=squashfs ro quiet \
        console=tty0 console=ttyS0,115200 \
        init=/opt/llamaste/llamaste \
        llamaste.mode=desktop llamaste.slot=${active_slot} \
        ip=dhcp
}
```

Key points:
- Single kernel (`bzImage`) on ESP — we don't A/B the kernel separately for now (kernel is inside squashfs via `/opt/llamaste/` path, so it updates with the system)
- Actually: the kernel is on ESP, NOT in squashfs. For simplicity, we keep one kernel on ESP. Kernel updates replace it in-place. The squashfs contains the binary + rootfs only. Kernel-only updates are rare.
- `llamaste.slot=A|B` kernel param lets the binary know which slot it booted from
- Rollback: after 3 failed boots (boot_counter reaches 0), GRUB switches `active_slot` to the other value

### 3.3 grubenv C++ Reader/Writer

Pure C++ implementation (~100 LOC). The format is:
```
# GRUB Environment Block\n
key=value\n
key=value\n
####...#### (padding to 1024 bytes)
```

Functions:
- `grubenv_read(path) -> map<string,string>`
- `grubenv_write(path, map<string,string>) -> bool`
- Write uses atomic pattern: write `.tmp`, `fsync()`, rename over original

The ESP mount point: `/boot` (mounted rw briefly during grubenv writes, then remounted ro).

---

## 4. Health Check & Boot Success

### 4.1 Boot Success Criteria

A boot is "successful" when:
1. Root filesystem mounted (squashfs)
2. DATA partition mounted (ext4)
3. HTTP server listening on port 80
4. Can serve `GET /health` with 200 OK

### 4.2 Implementation

In `child_main.cpp`, after `svr.listen()` starts:
1. Read `llamaste.slot` from `/proc/cmdline` to know current slot
2. If `boot_counter` is set in grubenv (meaning we're in a trial boot):
   - Wait for HTTP server to be ready (it already is at this point)
   - Set `boot_success=1` in grubenv
   - Clear `boot_counter` from grubenv
   - Log: `[update] Boot success confirmed for slot X`

This runs once per boot, only when `boot_counter` exists (i.e., after an update).

---

## 5. Update Bundle Format

### 5.1 File Format

```
llamaste-<version>.update (tar archive, uncompressed)
├── manifest.json          # Metadata + SHA-256 hashes + changelog
├── manifest.json.sig      # Ed25519 signature (64 bytes raw binary)
└── system.squashfs.xz     # XZ-compressed squashfs image
```

### 5.2 manifest.json Schema

```json
{
  "format_version": 1,
  "version": "1.1.0",
  "build_date": "2026-03-15T10:30:00Z",
  "arch": "x86-64",
  "min_version": "1.0.0",
  "components": {
    "system": {
      "file": "system.squashfs.xz",
      "sha256": "abc123...",
      "size_compressed": 45000000,
      "size_uncompressed": 162000000
    }
  },
  "changelog": [
    "Added mesh clustering support",
    "Improved TTS audio quality"
  ]
}
```

### 5.3 Signature Verification

- **Algorithm**: Ed25519 (TweetNaCl, public domain, ~300 LOC C)
- **What's signed**: The raw bytes of `manifest.json`
- **Public key**: Compiled into the llamaste binary as `constexpr unsigned char UPDATE_PUBKEY[32] = {...};`
- **Verification**: `crypto_sign_verify_detached(sig, manifest_bytes, manifest_len, pubkey)`

For development/testing, we generate a keypair and embed the public key. A real release process would use an HSM.

---

## 6. Update Installation Process

### 6.1 Steps

```
1. Obtain update bundle (download or USB)
2. Extract manifest.json and manifest.json.sig from tar
3. Verify Ed25519 signature of manifest.json
4. Parse manifest.json — check version > current, arch matches, min_version satisfied
5. Extract system.squashfs.xz from tar
6. Verify SHA-256 of compressed file matches manifest
7. Decompress XZ and write to inactive SYS partition (streaming, no temp file)
8. Verify written image by re-reading and checking SHA-256 of uncompressed data
9. Update grubenv: active_slot=<new>, boot_counter=3, boot_success=0
10. Report success, prompt for reboot
```

### 6.2 Determining Active/Inactive Slot

- Read `llamaste.slot` from `/proc/cmdline` (set by GRUB)
- If slot=A → inactive is B (partition 4, /dev/sda4)
- If slot=B → inactive is A (partition 3, /dev/sda3)
- If not set (pre-update systems) → assume A, inactive is B

### 6.3 Partition Detection

Reuse existing logic from `init.cpp` and `tools_install.cpp`:
- Try `/dev/sda{3,4}`, `/dev/vda{3,4}`, `/dev/nvme0n1p{3,4}`
- Active partition number = 3 for slot A, 4 for slot B

### 6.4 Streaming Decompression

Reuse `decompress_xz_to_device()` from `tools_install.cpp`. This already:
- Opens XZ stream
- Reads chunks from source
- Writes decompressed data directly to block device
- No intermediate storage needed

---

## 7. Update Sources

### 7.1 Online (GitHub Releases)

Check: `GET https://api.github.com/repos/<owner>/<repo>/releases/latest`
- Parse JSON for `tag_name` (version), `assets` (download URLs)
- Compare with current version (semver string compare)
- Download `.update` file from release assets
- Store in `/data/llamaste/updates/` staging area

libcurl is already linked. The model download code provides a working pattern for progress-tracked downloads with resume support.

### 7.2 USB Sideloading

- User mounts USB manually or it's auto-mounted
- Tool `update.check` scans common mount points: `/mnt/usb`, `/media/*`, `/tmp/usb`
- Looks for files matching `llamaste-*.update`
- Also: web UI "Upload Update" button (POST multipart file upload to `/llamaste/update/upload`)

### 7.3 File Upload via Web UI

- `POST /llamaste/update/upload` accepts multipart file upload
- Saves to `/data/llamaste/updates/`
- Returns JSON with filename and size
- Then user triggers install via `POST /llamaste/update/install`

---

## 8. Update Tools

### 8.1 Tool Definitions

| Tool | Description | Parameters |
|------|-------------|------------|
| `update.check` | Check for available updates (online + USB) | `source`: "online", "usb", or "all" (default) |
| `update.install` | Install an update from file or URL | `path`: local file path or URL |
| `update.status` | Current system version, active slot, available updates | (none) |
| `update.rollback` | Switch to other slot and reboot | (none) |

### 8.2 Tool Examples

```json
// update.check
{"available": true, "current_version": "1.0.0", "latest_version": "1.1.0",
 "source": "online", "download_url": "https://...", "changelog": [...]}

// update.status
{"version": "1.0.0", "active_slot": "A", "inactive_slot": "B",
 "inactive_version": null, "boot_success": true, "update_in_progress": false}

// update.rollback
{"success": true, "message": "Switched to slot B. Reboot to activate."}
```

---

## 9. Web UI

### 9.1 Update Card (System Panel)

Add an "Updates" card to the System panel (between About and MCP cards):

```html
<div class="system-card" id="update-card">
  <h3>System Update</h3>
  <div class="dash-row">
    <span class="label">Version</span>
    <span id="update-version" class="value">1.0.0</span>
  </div>
  <div class="dash-row">
    <span class="label">Active Slot</span>
    <span id="update-slot" class="value">A</span>
  </div>
  <div class="dash-row" id="update-available-row" style="display:none">
    <span class="label">Available</span>
    <span id="update-available" class="value" style="color:#00e5a0"></span>
  </div>
  <div id="update-changelog" style="display:none">...</div>
  <div id="update-progress" style="display:none">...</div>
  <div class="system-actions">
    <button id="update-check-btn" class="btn-sm">Check for Updates</button>
    <button id="update-install-btn" class="btn-sm" style="display:none">Install Update</button>
    <button id="update-rollback-btn" class="btn-sm btn-danger" style="display:none">Rollback</button>
  </div>
</div>
```

### 9.2 Progress Display

During installation, show a progress bar with steps:
1. Downloading (with % and speed)
2. Verifying signature
3. Writing to partition
4. Verifying written image
5. Ready to reboot

### 9.3 HTTP Endpoints

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/llamaste/update/status` | Version, slot, available updates |
| POST | `/llamaste/update/check` | Trigger update check |
| POST | `/llamaste/update/install` | Start installation (body: `{"source":"..."}`) |
| POST | `/llamaste/update/upload` | Upload .update file |
| POST | `/llamaste/update/rollback` | Switch slot and prepare reboot |
| GET | `/llamaste/update/progress` | SSE stream of install progress |

---

## 10. Version Tracking

### 10.1 Version String

`LLAMASTE_VERSION` constant in a header file (e.g., `version.h`):
```cpp
constexpr const char* LLAMASTE_VERSION = "1.0.0";
```

### 10.2 Slot Metadata

After installing an update, write a metadata file to the DATA partition:
```
/data/llamaste/slots/A.json  →  {"version": "1.0.0", "installed": "2026-03-06T..."}
/data/llamaste/slots/B.json  →  {"version": "1.1.0", "installed": "2026-03-07T..."}
```

This lets us know what version is on each slot without reading the squashfs.

---

## 11. New Files

| File | Purpose | ~LOC |
|------|---------|------|
| `updater.h` | UpdateManager class, grubenv reader/writer, bundle parser | 50 |
| `updater.cpp` | Full implementation | 400 |
| `tools_update.cpp` | 4 update tools | 150 |
| `tweetnacl.h` / `tweetnacl.c` | Ed25519 signature verification (vendored, public domain) | 300 |
| `version.h` | Version constant | 5 |

### Modified Files

| File | Changes |
|------|---------|
| `child_main.cpp` | Boot success check, update endpoints, register tools |
| `grub.cfg` | A/B slot switching logic |
| `grub-live.cfg` | Keep as-is (ISO always boots from partition 3) |
| `index.html` | Update card in system panel |
| `system.js` | Update card logic, progress polling |
| `tools.h` | register_update_tools declaration |
| `CMakeLists.txt` | Add updater.cpp, tools_update.cpp, tweetnacl.c |
| `host-test.sh` | Suite 12: Update tests |

---

## 12. Scoping Decisions

### In Scope (Phase 4a)
- GRUB A/B switching with boot counter
- grubenv reader/writer (pure C++)
- Health check + boot success marking
- Ed25519 signature verification (TweetNaCl)
- Update bundle parsing (tar + manifest)
- Partition writer (XZ decompress to block device)
- 4 update tools
- Web UI update card
- Update HTTP endpoints
- Online check via GitHub API
- File upload via web UI

### Out of Scope (Future)
- Delta/binary diff updates
- USB hotplug auto-detection (netlink)
- Auto-update mode
- Key rotation
- Kernel A/B (separate bzImage-A/bzImage-B)
- Fleet management
- Build script for creating .update bundles (manual for now)

---

## 13. Testing Strategy

### Host Tests (Suite 12)
- grubenv parse/write round-trip
- grubenv atomic write (tmp + rename)
- Version comparison (semver)
- Manifest JSON parsing
- Slot determination (A→B, B→A, missing→B)
- Ed25519 signature verify (valid + invalid)
- Bundle tar extraction

### Integration Tests
- Write squashfs to block device (loopback)
- Full update flow on VDI (download → verify → write → reboot → health check)
