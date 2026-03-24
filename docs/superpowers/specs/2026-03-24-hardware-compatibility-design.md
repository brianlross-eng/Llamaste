# Llamaste Hardware Compatibility Expansion — Design Spec

**Date**: 2026-03-24
**Status**: Approved
**Approach**: B (eudev auto-detection) + C (kernel 6.12 LTS upgrade, deferred)

## Goal

Transform Llamaste from ~75% x86_64 hardware coverage to ~97% (Phase B) and ~99% (Phase C) by:
1. Replacing the hardcoded module loading list with eudev-based auto-detection
2. Adding missing kernel drivers for ethernet, WiFi, GPU, Bluetooth, touchpads, and platform devices
3. Implementing a 3-tier firmware distribution strategy
4. Adding Bluetooth HID support (BlueZ + dbus)
5. Fixing critical laptop support gaps (I2C, HID, ACPI, pinctrl)

## Architecture

### Current State

```
init_load_modules() → hardcoded list of ~80 .ko paths → finit_module() syscall
                      No dependency resolution
                      No hardware detection (loads everything, skips missing)
                      Must be manually updated for each new driver
```

### Target State (Phase B)

```
PID 1 (main.cpp)
├─ init_mount_filesystems()
├─ do_live_pivot()                        ← /lib/firmware + /lib/modules available
├─ init_start_udevd()                     ★ NEW: fork udevd --daemon
├─ init_load_critical_modules()           ★ CHANGED: only GPU + deps via finit_module
├─ udevadm trigger (all except input)     ★ NEW: auto-load all drivers via modalias
├─ udevadm settle --timeout=30            ★ NEW: wait for probing to finish
├─ init_start_dbus()                      ★ NEW: dbus-daemon for BlueZ
├─ init_start_bluetoothd()                ★ NEW: Bluetooth daemon
├─ detect_hardware()                      ← Now sees all probed devices
├─ ... existing init steps ...
└─ supervisor_run()
    └─ child_main()
         ├─ ... existing init ...
         ├─ WiFi init (eudev already loaded modules)
         ├─ Ethernet scan (eudev already loaded modules)
         └─ Desktop compositor
              └─ udevadm trigger --subsystem-match=input  ← DELAYED for safety
```

### Module Loading Strategy

**Built-in (=y)** — Must be available before squashfs pivot:
- Storage: SATA AHCI, NVMe, SCSI, USB storage, MMC core
- Filesystem: squashfs, ext4, vfat, overlayfs
- USB: xHCI, EHCI, USB core, USB HID (for BIOS keyboards)
- Input: evdev, mousedev, keyboard
- DRM core + simpledrm (universal EFI fallback display)
- Network core: cfg80211, mac80211 (WiFi framework)
- I2C core, HID core (needed for touchpads)
- ACPI core, IOMMU (Intel + AMD)

**Modules (=m)** — Loaded by eudev after pivot:
- All WiFi vendor drivers (Intel, Realtek, Atheros, MediaTek, Broadcom)
- All Ethernet vendor drivers (Intel, Broadcom, Realtek, Marvell)
- GPU drivers (amdgpu, nouveau, i915 can stay =y, xe)
- Bluetooth (BT core, btusb, btintel, btrtl, btbcm, btmtk)
- Sound (HDA codecs, USB audio)
- Touchpad (i2c-hid, psmouse variants)
- Platform (pinctrl-*, i2c-i801, i2c-designware, intel-lpss)
- USB ethernet adapters
- SD card readers

**Critical modules** (loaded explicitly before udev trigger):
- amdgpu.ko, nouveau.ko, xe.ko — GPU must be ready before compositor
- Dependencies for the above (drm_buddy, drm_ttm, gpu-sched, etc.)

### Firmware Strategy (3-Tier)

**Tier 1 — Bundled in squashfs** (~50-70MB compressed):
- Intel WiFi (iwlwifi-*): ~15MB — covers AX200/210/BE200
- Realtek WiFi (rtw88/rtw89): ~5MB
- Atheros/Qualcomm WiFi (ath10k/ath11k): ~10MB
- MediaTek WiFi (mt76/mt7921): ~3MB
- Broadcom WiFi (brcmfmac): ~5MB
- AMD GPU (amdgpu): ~25MB compressed — all GCN/RDNA generations + APUs
- Intel GPU (i915/xe): ~5MB — GuC/HuC/DMC
- NVIDIA (nouveau): ~3MB — basic display firmware
- Bluetooth (Intel ibt, Realtek rtl_bt, Broadcom brcm): ~5-8MB
- Regulatory database: ~0.5MB

**Tier 2 — Downloadable firmware pack** (future, stored on DATA partition):
- Full linux-firmware snapshot for exotic hardware
- Downloaded on first-boot or via LLM tool (`firmware.download`)

**Tier 3 — On-demand download** (future):
- Kernel firmware helper can request firmware from DATA partition
- LLM detects missing firmware from dmesg and offers to download

### Bluetooth Architecture

```
dbus-daemon (/run/dbus/system_bus_socket)
    └─ bluetoothd (BlueZ 5.x)
         ├─ Kernel: CONFIG_BT=m, CONFIG_BT_HIDP=m, CONFIG_BT_HCIBTUSB=m
         ├─ btusb.ko auto-loaded by eudev for USB BT controllers
         ├─ Pairing info: /data/bluetooth/ (symlinked from /var/lib/bluetooth)
         └─ Config: /etc/bluetooth/input.conf (UserspaceHID=false)
```

Buildroot packages: `BR2_PACKAGE_BLUEZ5_UTILS=y`, `BR2_PACKAGE_DBUS=y`

### Boot Sequence Safety

**Known crash risks and mitigations:**

| Risk | Cause | Mitigation |
|------|-------|------------|
| wlroots SIGSEGV on input trigger | udevadm trigger before Wayland socket | Delay input trigger until socket exists |
| amdgpu slow probe (~5-10s) | Module init takes time | Load GPU explicitly + udevadm settle with 30s timeout |
| dbus crash kills BT input | dbus is single point of failure | Supervisor restart logic for dbus+bluetoothd |
| Watchdog timeout during module load | init_load_modules blocks PID 1 | Watchdog not open yet at this stage (opens in supervisor_run) |
| Missing firmware → driver fails silently | Firmware not in squashfs | 3-tier firmware + dmesg monitoring |
| IOMMU boot failure | VT-d/AMD-Vi without IOMMU driver | Add CONFIG_INTEL_IOMMU=y, CONFIG_AMD_IOMMU=y |

### Kernel Config Changes (Phase B)

**New built-in (=y):**
```
# I2C core (touchpads, sensors)
CONFIG_I2C=y
CONFIG_I2C_CHARDEV=y

# HID subsystem
CONFIG_HID=y
CONFIG_HID_GENERIC=y
CONFIG_HID_MULTITOUCH=y
CONFIG_USB_HID=y

# IOMMU
CONFIG_IOMMU_SUPPORT=y
CONFIG_INTEL_IOMMU=y
CONFIG_AMD_IOMMU=y
CONFIG_IOMMU_DEFAULT_PASSTHROUGH=y

# ACPI
CONFIG_ACPI_BUTTON=y
CONFIG_ACPI_VIDEO=y
CONFIG_ACPI_BATTERY=y
CONFIG_ACPI_FAN=y
CONFIG_POWER_SUPPLY=y
CONFIG_BACKLIGHT_CLASS_DEVICE=y

# PCIe
CONFIG_PCIEPORTBUS=y
CONFIG_PCIEASPM=y
CONFIG_HOTPLUG_PCI=y
CONFIG_HOTPLUG_PCI_PCIE=y

# Input
CONFIG_INPUT_KEYBOARD=y
CONFIG_KEYBOARD_ATKBD=y
CONFIG_INPUT_MOUSE=y
CONFIG_MOUSE_PS2=y
CONFIG_MOUSE_PS2_SYNAPTICS=y
CONFIG_MOUSE_PS2_SYNAPTICS_SMBUS=y
CONFIG_MOUSE_PS2_ALPS=y
CONFIG_MOUSE_PS2_ELANTECH=y
CONFIG_MOUSE_PS2_ELANTECH_SMBUS=y
```

**New modules (=m):**
```
# GPU
CONFIG_DRM_AMDGPU=m
CONFIG_DRM_NOUVEAU=m
CONFIG_DRM_XE=m  # (Phase C — needs kernel 6.8+)

# Bluetooth
CONFIG_BT=m
CONFIG_BT_HIDP=m
CONFIG_BT_HCIBTUSB=m
CONFIG_BT_HCIUART=m
CONFIG_BT_INTEL=m
CONFIG_BT_RTL=m
CONFIG_BT_BCM=m
CONFIG_BT_MTK=m

# I2C platform
CONFIG_I2C_I801=m
CONFIG_I2C_DESIGNWARE_CORE=m
CONFIG_I2C_DESIGNWARE_PLATFORM=m
CONFIG_I2C_DESIGNWARE_PCI=m
CONFIG_I2C_HID_ACPI=m

# Pinctrl (Intel LPSS, AMD)
CONFIG_PINCTRL=y
CONFIG_PINCTRL_INTEL=m
CONFIG_PINCTRL_CANNONLAKE=m
CONFIG_PINCTRL_TIGERLAKE=m
CONFIG_PINCTRL_ALDERLAKE=m
CONFIG_PINCTRL_METEORLAKE=m
CONFIG_PINCTRL_AMD=m

# Ethernet (missing families)
CONFIG_NET_VENDOR_BROADCOM=y
CONFIG_TIGON3=m
CONFIG_BNX2=m
CONFIG_BNXT=m
CONFIG_NET_VENDOR_MARVELL=y
CONFIG_ATLANTIC=m
CONFIG_NET_VENDOR_QUALCOMM=y
CONFIG_ALX=m

# WiFi (missing)
CONFIG_RTL8XXXU=m
CONFIG_ATH12K=m
CONFIG_MT7921U=m

# Sound (broader)
CONFIG_SND_HDA_CODEC_CA0110=m
CONFIG_SND_HDA_CODEC_CA0132=m
CONFIG_SND_HDA_CODEC_CIRRUS=m
CONFIG_SND_HDA_CODEC_CONEXANT=m
CONFIG_SND_HDA_CODEC_SIGMATEL=m
CONFIG_SND_HDA_CODEC_VIA=m
CONFIG_SND_SOC=m

# CPU frequency
CONFIG_CPU_FREQ=y
CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND=y
CONFIG_X86_INTEL_PSTATE=y
CONFIG_X86_AMD_PSTATE=m
```

### Buildroot Config Changes (Phase B)

```
# Module loading
BR2_PACKAGE_KMOD=y
BR2_PACKAGE_KMOD_TOOLS=y

# Bluetooth
BR2_PACKAGE_BLUEZ5_UTILS=y
BR2_PACKAGE_BLUEZ5_UTILS_TOOLS=y
BR2_PACKAGE_DBUS=y

# Firmware expansion
BR2_PACKAGE_LINUX_FIRMWARE_AMDGPU=y
BR2_PACKAGE_LINUX_FIRMWARE_I915=y
BR2_PACKAGE_LINUX_FIRMWARE_NOUVEAU=y
BR2_PACKAGE_LINUX_FIRMWARE_IWLWIFI_7260=y
BR2_PACKAGE_LINUX_FIRMWARE_IWLWIFI_7265D=y
BR2_PACKAGE_LINUX_FIRMWARE_RTL_87XX=y
BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA9984=y
BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA99X0=y
BR2_PACKAGE_LINUX_FIRMWARE_QUALCOMM_WCN36XX=y
BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7915=y
BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7921=y
BR2_PACKAGE_LINUX_FIRMWARE_RALINK_RT61=y
BR2_PACKAGE_LINUX_FIRMWARE_RALINK_RT73=y
BR2_PACKAGE_LINUX_FIRMWARE_RALINK_RT2XX=y
# Bluetooth firmware
BR2_PACKAGE_LINUX_FIRMWARE_IBT=y
BR2_PACKAGE_LINUX_FIRMWARE_RTL_87XX_BT=y
BR2_PACKAGE_LINUX_FIRMWARE_BCM43XXX=y

# Mesa GPU drivers
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_RADEONSI=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_NOUVEAU=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_CROCUS=y
BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_LLVMPIPE=y
BR2_PACKAGE_LLVM=y
```

### Size Budget

| Component | Compressed Size | Notes |
|-----------|----------------|-------|
| AMD GPU firmware | ~25MB | All GCN/RDNA generations |
| Intel GPU firmware | ~5MB | GuC/HuC/DMC |
| NVIDIA nouveau firmware | ~3MB | Basic display only |
| Bluetooth firmware | ~5-8MB | Intel + Realtek + Broadcom |
| Additional WiFi firmware | ~5MB | RTL8xxxU, ath12k, MT7921U |
| Kernel modules (all new) | ~5-8MB | Compressed in squashfs |
| kmod (depmod/modprobe) | ~0.5MB | Static binary |
| BlueZ + dbus | ~4.5MB | bluetoothd + dbus-daemon + libs |
| Mesa radeonsi + nouveau + LLVM | ~15-20MB | GPU-accelerated Wayland |
| **Total Phase B** | **~70-100MB** | Current squashfs ~200MB → ~300MB |

### Phase C Additions (Kernel 6.12 LTS Upgrade)

Deferred items requiring kernel upgrade:
- WiFi 7: MediaTek MT7925 driver (needs 6.7+)
- Intel Xe GPU driver (needs 6.8+)
- Improved ath12k WiFi 7 support
- rtw89 USB stability improvements
- Better amd-pstate CPU frequency scaling
- Thunderbolt/USB4 improvements
- Webcam (UVC) support
- Full CPU microcode loading

### Testing Strategy

1. **VirtualBox**: Basic boot, module loading, eudev trigger timing
2. **QEMU with OVMF**: EFI boot, GPU fallback (simpledrm), ACPI
3. **Bare metal — ASUS VivoBook** (existing test machine): Full WiFi + touchpad + display
4. **Bare metal — AMD desktop** (new test target): amdgpu, AMD IOMMU, Ryzen APU
5. **USB WiFi dongles**: RTL8188EU, MT7612U, AX210
6. **Bluetooth keyboard**: Test pairing + reconnection persistence
7. **Old office PC** (Dell OptiPlex / HP ProDesk): Broadcom ethernet, Intel HD Graphics

### Success Criteria

- Boot to web UI on Intel, AMD, and NVIDIA GPU hardware
- Touchpad works on modern laptops (I2C HID)
- Bluetooth keyboard pairs and reconnects across reboots
- WiFi connects on Intel, Realtek, Atheros, MediaTek, Broadcom chips
- Ethernet works on Broadcom, Marvell, Realtek 2.5GbE NICs
- No regressions on existing ASUS VivoBook test machine
- Module loading is fully automatic (no hardcoded paths)
- Squashfs size stays under 350MB
