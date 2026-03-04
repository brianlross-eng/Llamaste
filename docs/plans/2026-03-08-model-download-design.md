# Model Download & USB Sideload Design

**Date:** 2026-03-08
**Status:** Approved

---

## Goal

Enable users to get GGUF models onto the device via three paths: search and download from Hugging Face, copy from USB drive, or upload via the web file browser (already works).

## Architecture

Add libcurl to Buildroot for HTTPS downloads. Implement five new tools in `tools_model_download.cpp` that use the Hugging Face API and direct CDN downloads. USB sideload uses standard Linux block device scanning + mount/copy.

## New Tools

### `model.search`
Search Hugging Face for GGUF model repositories.

- **Input:** `query` (string, e.g. "qwen2.5 7b")
- **API:** `GET https://huggingface.co/api/models?search={query}+gguf&sort=downloads&direction=-1&limit=10`
- **Output:** Array of `{repo_id, author, downloads, likes, last_modified}`
- **Error handling:** Network timeout → return error message, no crash

### `model.files`
List GGUF files within a specific Hugging Face repository.

- **Input:** `repo_id` (string, e.g. "Qwen/Qwen2.5-3B-Instruct-GGUF")
- **API:** `GET https://huggingface.co/api/models/{repo_id}/tree/main`
- **Output:** Array of `{filename, size_bytes, size_human}` filtered to `*.gguf` only
- **Error handling:** 404 → "Repository not found"

### `model.download`
Download a GGUF file from Hugging Face to `/data/models/`.

- **Input:** `repo_id` (string), `filename` (string)
- **URL:** `https://huggingface.co/{repo_id}/resolve/main/{filename}`
- **Flow:**
  1. Check disk space on /data — fail if less than file_size * 1.1
  2. Download via libcurl to `/data/models/{filename}.part`
  3. Follow redirects (HF uses 302 → CDN)
  4. Track progress: bytes downloaded, total size, speed, ETA
  5. On completion: rename `.part` → `.gguf`
  6. On error: leave `.part` file for resume
- **Resume:** If `.part` file exists, send `Range` header to resume
- **Progress reporting:** Return JSON with `{status, progress_pct, bytes_downloaded, total_bytes, speed_mbps}`
- **Timeout:** 30s connect, no read timeout (large files)

### `model.usb_import`
Scan for USB drives and import GGUF files.

- **Input:** `action` ("scan" | "import"), optional `device`, optional `filename`
- **Scan flow:**
  1. Read `/proc/partitions` for block devices
  2. Skip `sda` (system disk) and `loop*`
  3. For each candidate: try mount at `/mnt/usb` (vfat, ext4, ntfs3)
  4. Scan for `*.gguf` files recursively (max depth 3)
  5. Return list of `{device, filename, size_bytes, size_human}`
  6. Unmount after scan
- **Import flow:**
  1. Mount specified device
  2. Copy specified file to `/data/models/` with progress
  3. Unmount

### `model.recommended`
Show the best model for this hardware.

- **Input:** none
- **Output:** `{model_name, filename, repo_id, ram_required_mb, ram_available_mb, download_url, estimated_size}`
- **Logic:** Same table as `main.cpp select_model()` but also returns download info

## Buildroot Changes

```
# In defconfig:
BR2_PACKAGE_OPENSSL=y
BR2_PACKAGE_LIBCURL=y

# In linux.config (for USB NTFS support):
CONFIG_NTFS3_FS=y
```

## CMake Changes

Link llamaste against libcurl:
```cmake
find_package(CURL REQUIRED)
target_link_libraries(llamaste ${CURL_LIBRARIES})
target_include_directories(llamaste PRIVATE ${CURL_INCLUDE_DIRS})
```

For host tests (no libcurl available), guard with `#ifdef HAVE_LIBCURL` or use a mock.

## Web UI Enhancement

In `dashboard.js`, when model status shows "No model loaded":
- Show "Download Recommended Model" button
- On click: POST to `/llamaste/model/download-recommended`
- Show progress bar updated via SSE notifications
- On completion: show "Model downloaded, restart to load" or auto-reload

New REST endpoint in child_main.cpp:
- `POST /llamaste/model/download-recommended` — triggers model.recommended + model.download

## File Structure

| File | Action |
|------|--------|
| `src/llamaste/tools_model_download.cpp` | CREATE — ~400 LOC, all 5 tools |
| `src/llamaste/tools.cpp` | MODIFY — register new tools |
| `src/llamaste/CMakeLists.txt` | MODIFY — link libcurl |
| `br2-external/configs/llamaste_x86_64_defconfig` | MODIFY — add openssl + libcurl |
| `br2-external/board/llamaste/linux.config` | MODIFY — add NTFS3_FS |
| `web/dashboard.js` | MODIFY — download button + progress |
| `src/llamaste/child_main.cpp` | MODIFY — download-recommended endpoint |
| `tests/test_model_download.cpp` | CREATE — unit tests |
| `scripts/host-test.sh` | MODIFY — add Suite 9 |

## Security Considerations

- Download URLs validated: must start with `https://huggingface.co/`
- Filenames validated: must end with `.gguf`, no path traversal
- USB mount is read-only (`mount -o ro`)
- Disk space checked before download starts
- `.part` files cleaned up on explicit cancel (not on crash — allows resume)

## Not Building (YAGNI)

- No model repository browser UI (tools + agent handle this)
- No model conversion/quantization
- No torrent/P2P/distributed downloads
- No automatic first-boot download (user-initiated only)
- No model deletion tool (use existing `fs.delete`)
- No model format validation beyond GGUF magic bytes (existing `model.info` does this)
