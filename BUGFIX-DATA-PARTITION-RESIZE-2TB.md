# Bug Report: DATA Partition Resize Crashes on Large NVMe Drives

**Date:** 2026-03-23  
**Severity:** Critical — prevents installed system from booting  
**Hardware:** GMKtec EVO-X2, Lexar SSD NQ790 2TB NVMe (`/dev/nvme0n1`)

---

## Symptom

Llamaste installs successfully via the live ISO installer, but crashes immediately on first boot from the installed disk:

```
Kernel Offset: disabled
---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000 ]---
```

- GRUB loads correctly ✅
- Kernel starts correctly ✅
- Llamaste binary starts as PID 1 ✅
- Binary exits with code 0 → kernel panics ❌

The same binary on the same hardware works perfectly in live mode from USB.

---

## Root Cause

`init.cpp` performs a DATA partition auto-resize on first boot — it expands GPT partition 5
to fill the disk, then resizes the ext4 filesystem via `EXT4_IOC_RESIZE_FS`.

The GPT manipulation code updates the Protective MBR (PMBR) after the resize. The PMBR
disk size field at offset 458 is a **32-bit field**, max value `0xFFFFFFFF` sectors (~2.2TB).

The Lexar 2TB NVMe has **3,907,029,168 sectors**. This fits in 32 bits, BUT the CHS end
fields at offsets 451-453 are only 3 bytes and may be overflowing or miscalculating for
a disk of this size, causing the GPT to become invalid.

When the PMBR update writes invalid data, the binary hits an error and calls `exit(0)`,
which as PID 1 triggers a kernel panic.

---

## Hardware Details

| Field | Value |
|-------|-------|
| Machine | GMKtec EVO-X2 |
| CPU | AMD Ryzen AI MAX+ 395 (32 threads) |
| RAM | 64GB visible (128GB installed, 64GB reserved for Radeon 8060S iGPU) |
| NVMe | Lexar SSD NQ790 2TB |
| NVMe device | `/dev/nvme0n1` |
| Total sectors | 3,907,029,168 |
| Total size | 1,863 GB |
| Partition count after install | 5 (correct Llamaste layout) |

---

## Fix

In `init.cpp`, in the DATA partition auto-resize / GPT manipulation code:

**1. Clamp the PMBR disk size field:**
```cpp
// PMBR disk size at offset 458 is 32-bit — clamp to max uint32
uint32_t pmbr_size = (uint32_t)std::min(total_sectors, (uint64_t)0xFFFFFFFF);
memcpy(mbr + 458, &pmbr_size, 4);
```

**2. Fix CHS end fields for large disks:**
For disks larger than the CHS addressing limit (~8GB), set CHS end to max value:
```cpp
// CHS end at offsets 451-453 — use max value for large disks
mbr[451] = 0xFE; // head 254
mbr[452] = 0xFF; // sector 63, cylinder high bits 0xFF
mbr[453] = 0xFF; // cylinder low bits
```

**3. Add error handling — do NOT exit() on resize failure:**
```cpp
// Instead of exit(0) on failure, log and continue
if (resize_failed) {
    log_warn("DATA partition resize failed — continuing with original size");
    // Do not exit — let the system boot anyway
}
```

---

## Verification

After the fix, test with:
- A 2TB+ NVMe drive (this exact scenario)
- A 4TB+ NVMe drive (tests the actual uint32 overflow boundary)
- A standard 256GB drive (regression test — must still work)

---

## Workaround (until fix is deployed)

None available without a shell on the installed system. The machine must run in live mode
from USB until this bug is fixed and a new ISO is built.

---

## Additional Notes

- **Live mode works perfectly** on this hardware — binary, kernel, and all 67 tools functional
- This is a **first boot only** crash — the resize is idempotent and skips on subsequent boots
- The GMKtec EVO-X2 also has **WiFi 7** built in — worth adding WiFi support for this hardware class
- The EVO-X2 has an **NPU (XDNA 2, 50+ TOPS)** — future opportunity for NPU-accelerated inference
- The **Radeon 8060S** (40 RDNA 3.5 CUs) shares the full 128GB unified memory pool —
  ROCm/HIP support would allow running very large models at GPU speeds
- BIOS key: **ESC** (not F2) — boot menu: **F7**
