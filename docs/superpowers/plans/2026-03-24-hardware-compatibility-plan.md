# Llamaste Hardware Compatibility — Implementation Plan

**Date**: 2026-03-24
**Design Spec**: `docs/superpowers/specs/2026-03-24-hardware-compatibility-design.md`
**Goal**: ~97% x86_64 hardware coverage (Phase B), ~99% (Phase C)
**Architecture**: eudev auto-detection + expanded kernel config + Bluetooth + 3-tier firmware
**Tech Stack**: C++ (init/supervisor), Buildroot, Linux 6.6.70 (Phase B) → 6.12 LTS (Phase C)

---

## Phase B: eudev Auto-Detection + Broad Hardware Support

### Step 1: Kernel Config — Critical Platform Support (Session 1)
**Time estimate**: 1 session
**Files**: `br2-external/board/llamaste/linux.config`

Add the missing platform infrastructure that everything else depends on:

```
1.1  Add I2C core subsystem (built-in)
     CONFIG_I2C=y, CONFIG_I2C_CHARDEV=y, CONFIG_I2C_COMPAT=y
     CONFIG_I2C_MUX=y
     TEST: Verify I2C bus appears in /sys/bus/i2c after boot

1.2  Add HID subsystem (built-in)
     CONFIG_HID=y, CONFIG_HID_GENERIC=y, CONFIG_HID_MULTITOUCH=y
     CONFIG_USB_HID=y, CONFIG_I2C_HID_CORE=y
     TEST: USB keyboard/mouse still work (regression check)

1.3  Add IOMMU support (built-in)
     CONFIG_IOMMU_SUPPORT=y, CONFIG_IOMMU_DEFAULT_DMA_LAZY=y
     (NOT PASSTHROUGH — avoids silent memory corruption on devices doing DMA)
     CONFIG_INTEL_IOMMU=y, CONFIG_INTEL_IOMMU_DEFAULT_ON=y
     CONFIG_AMD_IOMMU=y, CONFIG_AMD_IOMMU_V2=y
     TEST: Boot in VirtualBox with VT-x/VT-d enabled — no panic

1.4  Add ACPI platform support (built-in)
     CONFIG_ACPI_BUTTON=y, CONFIG_ACPI_VIDEO=y
     CONFIG_ACPI_BATTERY=y, CONFIG_ACPI_FAN=y
     CONFIG_ACPI_THERMAL=y, CONFIG_ACPI_WMI=y
     CONFIG_POWER_SUPPLY=y, CONFIG_BACKLIGHT_CLASS_DEVICE=y
     CONFIG_BACKLIGHT_GENERIC=y
     TEST: /sys/class/power_supply/ appears on laptop

1.5  Add PCIe infrastructure (built-in)
     CONFIG_PCIEPORTBUS=y, CONFIG_PCIEASPM=y
     CONFIG_HOTPLUG_PCI=y, CONFIG_HOTPLUG_PCI_PCIE=y
     TEST: lspci equivalent works, PCIe devices enumerated

1.5b Add RFKILL + firmware loader fix
     CONFIG_RFKILL=y                  # Laptop WiFi/BT hardware killswitches
     # CONFIG_FW_LOADER_USER_HELPER is not set
     # (CRITICAL: no shell means userspace helper causes 60s timeouts per firmware)
     TEST: WiFi not blocked by rfkill on laptop with hardware switch

1.6  Add pinctrl core (built-in) + platform drivers (modules)
     CONFIG_PINCTRL=y (built-in)
     CONFIG_MFD_INTEL_LPSS_PCI=m     # Intel LPSS parent (Kaby Lake+, REQUIRED for I2C)
     CONFIG_MFD_INTEL_LPSS_ACPI=m    # Intel LPSS ACPI variant
     CONFIG_PINCTRL_INTEL=m, CONFIG_PINCTRL_CANNONLAKE=m
     CONFIG_PINCTRL_TIGERLAKE=m, CONFIG_PINCTRL_ALDERLAKE=m
     CONFIG_PINCTRL_METEORLAKE=m, CONFIG_PINCTRL_SUNRISEPOINT=m
     CONFIG_PINCTRL_LEWISBURG=m, CONFIG_PINCTRL_BROXTON=m
     CONFIG_PINCTRL_GEMINILAKE=m, CONFIG_PINCTRL_ICELAKE=m
     CONFIG_PINCTRL_JASPERLAKE=m, CONFIG_PINCTRL_LAKEFIELD=m
     CONFIG_PINCTRL_ELKHARTLAKE=m, CONFIG_PINCTRL_EMMITSBURG=m
     CONFIG_PINCTRL_AMD=m
     TEST: pinctrl driver bound in /sys/bus/platform/drivers/

1.7  Add I2C platform controllers (modules)
     CONFIG_I2C_I801=m (Intel SMBus)
     CONFIG_I2C_DESIGNWARE_CORE=m
     CONFIG_I2C_DESIGNWARE_PLATFORM=m
     CONFIG_I2C_DESIGNWARE_PCI=m
     CONFIG_I2C_PIIX4=m (AMD SMBus)
     CONFIG_I2C_HID_ACPI=m
     TEST: I2C adapters appear after module load

1.8  Add input drivers (built-in for PS/2, modules for I2C)
     CONFIG_INPUT_KEYBOARD=y, CONFIG_KEYBOARD_ATKBD=y
     CONFIG_INPUT_MOUSE=y, CONFIG_MOUSE_PS2=y
     CONFIG_MOUSE_PS2_SYNAPTICS=y, CONFIG_MOUSE_PS2_SYNAPTICS_SMBUS=y
     CONFIG_MOUSE_PS2_ALPS=y, CONFIG_MOUSE_PS2_ELANTECH=y
     CONFIG_MOUSE_PS2_ELANTECH_SMBUS=y, CONFIG_MOUSE_PS2_FOCALTECH=y
     CONFIG_MOUSE_ELAN_I2C=m, CONFIG_MOUSE_ELAN_I2C_SMBUS=y
     TEST: Touchpad generates events on bare metal laptop

1.9  Add CPU frequency scaling (built-in)
     CONFIG_CPU_FREQ=y
     CONFIG_CPU_FREQ_STAT=y
     CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y
     CONFIG_CPU_FREQ_GOV_PERFORMANCE=y
     CONFIG_CPU_FREQ_GOV_POWERSAVE=y
     CONFIG_CPU_FREQ_GOV_ONDEMAND=y
     CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y
     CONFIG_X86_INTEL_PSTATE=y
     CONFIG_X86_AMD_PSTATE=m
     CONFIG_X86_ACPI_CPUFREQ=m
     TEST: /sys/devices/system/cpu/cpu0/cpufreq/ exists

COMMIT: "feat: kernel config — critical platform support (I2C, HID, IOMMU, ACPI, pinctrl, cpufreq)"
```

### Step 2: Kernel Config — Expanded Network Drivers (Session 1, continued)
**Time estimate**: Same session as Step 1
**Files**: `br2-external/board/llamaste/linux.config`

```
2.1  Add missing Ethernet drivers (modules)
     # Broadcom (HP/Dell servers + desktops, ~15-20% of market)
     CONFIG_NET_VENDOR_BROADCOM=y
     CONFIG_TIGON3=m            # tg3 — Broadcom GbE (very common)
     CONFIG_BNX2=m              # bnx2 — Broadcom NetXtreme II
     CONFIG_BNXT=m              # bnxt_en — Broadcom NetXtreme-C/E
     CONFIG_B44=m               # Broadcom 440x (older)

     # Realtek 2.5GbE (r8169 already covers r8125, just ensure it's enabled)
     # CONFIG_R8169=y already set — covers RTL8125/RTL8126 since kernel 5.9

     # Marvell / Aquantia (multi-gig NICs)
     CONFIG_NET_VENDOR_MARVELL=y
     CONFIG_ATLANTIC=m          # Aquantia/Marvell AQC 2.5G/5G/10G
     CONFIG_SKGE=m              # Marvell Yukon GbE (older)
     CONFIG_SKY2=m              # Marvell Yukon2 GbE

     # Qualcomm/Atheros (Killer NICs in gaming laptops)
     CONFIG_NET_VENDOR_QUALCOMM=y
     CONFIG_ALX=m               # Qualcomm Atheros AR816x/AR817x
     CONFIG_ATL1C=m             # Atheros AR8131/AR8132/AR8152

     # Mellanox (workstation/server)
     CONFIG_NET_VENDOR_MELLANOX=y
     CONFIG_MLX4_EN=m
     CONFIG_MLX5_CORE=m

     # JMicron (some motherboards)
     CONFIG_NET_VENDOR_MICREL=y
     CONFIG_JME=m

     TEST: VirtualBox e1000 still works (regression), tg3 loads on HP hardware

2.2  Add missing WiFi drivers (modules)
     # Realtek RTL8xxxU (very common cheap USB dongles)
     CONFIG_RTL8XXXU=m
     CONFIG_RTL8XXXU_UNTESTED=y  # Enable all supported chipsets

     # Qualcomm ath12k (WiFi 7, basic support on 6.6)
     CONFIG_ATH12K=m
     CONFIG_ATH12K_PCI=m

     # MediaTek MT7921 USB (WiFi 6E USB dongles)
     CONFIG_MT7921U=m
     # Also ensure MT7921 PCIe is covered (mt7921e already in list)
     # MT7925 needs kernel 6.7+ — deferred to Phase C

     # Ralink/MediaTek RT2800 PCIe (not just USB)
     CONFIG_RT2800PCI=m
     CONFIG_RT2X00PCI=m

     # Broadcom brcmsmac (older Broadcom PCIe, pre-FullMAC)
     CONFIG_BRCMSMAC=m

     TEST: RTL8188EU USB dongle detected, ath12k loads without crash

2.3  Add missing WiFi firmware to defconfig
     # (Buildroot firmware selections)
     BR2_PACKAGE_LINUX_FIRMWARE_RTL_87XX=y      # RTL8xxxU firmware
     BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA9984=y
     BR2_PACKAGE_LINUX_FIRMWARE_ATHEROS_10K_QCA99X0=y
     BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7915=y
     BR2_PACKAGE_LINUX_FIRMWARE_MEDIATEK_MT7921=y
     BR2_PACKAGE_LINUX_FIRMWARE_RALINK_RT61=y
     BR2_PACKAGE_LINUX_FIRMWARE_RALINK_RT73=y
     TEST: firmware files present in /lib/firmware/ after build

COMMIT: "feat: kernel config — expanded ethernet + WiFi drivers for ~97% coverage"
```

### Step 3: Kernel Config — GPU Support (Session 2)
**Time estimate**: 1 session
**Files**: `br2-external/board/llamaste/linux.config`, `br2-external/configs/llamaste_x86_64_defconfig`

```
3.1  Add AMD GPU support (module)
     CONFIG_DRM_AMDGPU=m
     CONFIG_DRM_AMDGPU_CIK=y     # GCN 2.0 (older Radeon)
     CONFIG_DRM_AMDGPU_SI=y      # GCN 1.0 (even older)
     CONFIG_DRM_AMD_DC=y          # Display Core (required for display output)
     CONFIG_DRM_AMD_DC_FP=y       # Floating point for DCN
     # Dependencies (auto-selected but list for clarity)
     CONFIG_DRM_TTM=m
     CONFIG_DRM_BUDDY=m
     CONFIG_DRM_SCHED=m
     CONFIG_BACKLIGHT_CLASS_DEVICE=y  # (already in Step 1)
     TEST: amdgpu.ko loads, /dev/dri/card0 appears on AMD hardware

3.2  Add NVIDIA nouveau support (module)
     CONFIG_DRM_NOUVEAU=m
     # nouveau uses TTM (shared with amdgpu)
     CONFIG_NOUVEAU_DEBUG=5       # Minimal logging
     CONFIG_NOUVEAU_DEBUG_DEFAULT=3
     TEST: nouveau.ko loads, basic display on NVIDIA GT/GTX hardware

3.3  Add AMD GPU firmware to defconfig
     BR2_PACKAGE_LINUX_FIRMWARE_AMDGPU=y
     # This pulls ALL amdgpu firmware (~60MB uncompressed, ~25MB in squashfs)
     # Covers: GCN 1.0-5.0, RDNA 1.0-3.0, all Ryzen APUs
     TEST: /lib/firmware/amdgpu/ populated with .bin files

3.4  Add Intel GPU firmware
     BR2_PACKAGE_LINUX_FIRMWARE_I915=y
     # GuC/HuC/DMC firmware for Gen9+ (Skylake through Raptor Lake)
     TEST: /lib/firmware/i915/ populated

3.5  Add NVIDIA nouveau firmware
     BR2_PACKAGE_LINUX_FIRMWARE_NOUVEAU=y
     TEST: /lib/firmware/nvidia/ populated

3.6  Add Mesa Gallium drivers to defconfig
     BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_RADEONSI=y  # AMD
     BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_NOUVEAU=y   # NVIDIA
     BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_CROCUS=y    # Intel Gen4-7 (old)
     BR2_PACKAGE_MESA3D_GALLIUM_DRIVER_LLVMPIPE=y  # Software fallback
     # iris already enabled for modern Intel
     # LLVM dependency
     BR2_PACKAGE_LLVM=y
     TEST: glxinfo/eglinfo shows hardware renderer on AMD/NVIDIA

3.7  Verify simpledrm fallback still works
     Remove WLR_RENDERER=pixman when real GPU driver detected
     Add logic to child_main.cpp: check /sys/class/drm for non-simpledrm
     If hardware GPU found: unset WLR_RENDERER (use GL)
     If only simpledrm: keep WLR_RENDERER=pixman
     TEST: Boot with and without GPU driver — both show display

COMMIT: "feat: GPU support — amdgpu, nouveau, Mesa radeonsi/nouveau/llvmpipe"
```

### Step 4: Bluetooth Support (Session 2, continued)
**Time estimate**: Same session
**Files**: `br2-external/board/llamaste/linux.config`, `br2-external/configs/llamaste_x86_64_defconfig`, `src/llamaste/init.cpp`, `src/llamaste/init.h`

```
4.1  Add Bluetooth kernel config (modules)
     CONFIG_BT=m
     CONFIG_BT_BREDR=y           # Classic Bluetooth (HID keyboards/mice)
     CONFIG_BT_LE=y              # Bluetooth Low Energy
     CONFIG_BT_HIDP=m            # HID Profile (keyboard/mouse)
     CONFIG_BT_BNEP=m            # Network encapsulation
     CONFIG_BT_RFCOMM=m          # Serial port emulation
     CONFIG_BT_HCIBTUSB=m        # USB Bluetooth controllers
     CONFIG_BT_HCIUART=m         # UART Bluetooth (laptop combo cards)
     CONFIG_BT_INTEL=m           # Intel BT protocol
     CONFIG_BT_RTL=m             # Realtek BT protocol
     CONFIG_BT_BCM=m             # Broadcom BT protocol
     CONFIG_BT_MTK=m             # MediaTek BT protocol
     CONFIG_BT_QCA=m             # Qualcomm BT protocol
     # CRC dependency for some BT drivers
     CONFIG_CRC_CCITT=y          # (already needed for rt2800)
     TEST: BT modules load, /sys/class/bluetooth/ appears with USB dongle

4.2  Add Bluetooth firmware to defconfig
     BR2_PACKAGE_LINUX_FIRMWARE_IBT=y           # Intel Bluetooth
     BR2_PACKAGE_LINUX_FIRMWARE_RTL_87XX_BT=y   # Realtek Bluetooth
     BR2_PACKAGE_LINUX_FIRMWARE_BCM43XXX=y      # Broadcom Bluetooth
     BR2_PACKAGE_LINUX_FIRMWARE_QCA=y            # Qualcomm Bluetooth
     TEST: /lib/firmware/intel/, /lib/firmware/rtl_bt/ populated

4.3  Add BlueZ + dbus to Buildroot defconfig
     BR2_PACKAGE_DBUS=y
     BR2_PACKAGE_BLUEZ5_UTILS=y
     BR2_PACKAGE_BLUEZ5_UTILS_TOOLS=y   # bluetoothctl, hcitool
     TEST: bluetoothd and dbus-daemon binaries exist in rootfs

4.4  Create init_start_dbus() in init.cpp
     - mkdir -p /run/dbus
     - fork() + setsid() in child for signal isolation
     - execl("/usr/bin/dbus-daemon", "--system", "--nofork") in child
     - Store PID in global g_dbus_pid for restart monitoring
     - Poll for /run/dbus/system_bus_socket (up to 3s)
     - Non-fatal on failure (BT just won't work)
     TEST: dbus socket appears after boot

4.5  Create init_start_bluetoothd() in init.cpp
     - Symlink /var/lib/bluetooth → /data/bluetooth (pairing persistence)
     - fork() + setsid() in child
     - execl("/usr/libexec/bluetooth/bluetoothd", "-n") — foreground, no daemonize
     - Store PID in global g_bluetoothd_pid for restart monitoring
     - Non-fatal on failure
     TEST: bluetoothd running, BT adapter visible

4.5b Add dbus + bluetoothd restart monitoring to supervisor
     In supervisor_run() main loop (alongside existing child_main restart):
     - waitpid(g_dbus_pid, WNOHANG) — if exited, respawn dbus-daemon
     - waitpid(g_bluetoothd_pid, WNOHANG) — if exited, respawn bluetoothd
     - Max 3 restarts per daemon per 5 minutes (prevent restart storm)
     - Log restart events
     This ensures BT keyboard stays working even if dbus/bluetoothd crash
     TEST: kill -9 bluetoothd → supervisor respawns within 5s

4.6  Add BlueZ config to overlay
     Create br2-external/board/llamaste/overlay/etc/bluetooth/input.conf:
       [General]
       UserspaceHID=false     # Use kernel HIDP (lower latency)
       ClassicBondedOnly=false # Allow connections from non-bonded devices
     Create br2-external/board/llamaste/overlay/etc/bluetooth/main.conf:
       [General]
       Name=llamaste
       DiscoverableTimeout=0  # Always discoverable
       Discoverable=true
       FastConnectable=true
       [Policy]
       AutoEnable=true
     TEST: BT keyboard can pair and type

4.7  Wire dbus + bluetoothd into main.cpp boot sequence
     After init_setup_audio() and before detect_hardware():
       init_start_dbus();
       init_start_bluetoothd();
     TEST: Full boot with BT keyboard works end-to-end

COMMIT: "feat: Bluetooth HID support — BlueZ, dbus, kernel BT stack, pairing persistence"
```

### Step 5: eudev Auto-Detection — Replace Hardcoded Module Loading (Session 3)
**Time estimate**: 1 session (critical, most complex)
**Files**: `src/llamaste/init.cpp`, `src/llamaste/init.h`, `src/llamaste/main.cpp`, `src/llamaste/child_main.cpp`, `br2-external/configs/llamaste_x86_64_defconfig`

```
5.1  Add kmod to Buildroot defconfig
     BR2_PACKAGE_KMOD=y
     BR2_PACKAGE_KMOD_TOOLS=y
     TEST: depmod and modprobe binaries exist in rootfs

5.2  Create init_start_udevd() function in init.cpp
     - Fork child with setsid() (same pattern as wpa_supplicant fix)
     - In child: execl("/sbin/udevd", "udevd") — NO --daemon flag
       (--daemon double-forks, losing PID tracking — same anti-pattern
        already fixed for wpa_supplicant per CLAUDE.md)
     - Fallback path: /usr/sbin/udevd
     - Store PID in global g_udevd_pid for monitoring
     - Wait up to 5s for /run/udev/control socket to appear
     - Log success/failure
     - Called from main.cpp BEFORE module loading
     TEST: udevd running after boot, /run/udev/control exists, PID tracked

5.3  Create init_load_critical_modules() — stripped-down version
     Keep ONLY modules that must load before udev trigger:
     - GPU: amdgpu.ko + dependencies (drm_ttm, drm_buddy, drm_sched, gpu_sched)
     - GPU: nouveau.ko + dependencies
     - GPU: xe.ko (Phase C)
     These must load early so compositor has a proper DRM device.
     Use same finit_module() approach as current code.
     If module doesn't exist, skip silently (same as now).
     TEST: GPU modules loaded before udevadm trigger

5.4  Create init_udev_trigger_all() — trigger everything except input
     Fork + exec: udevadm trigger --action=add --subsystem-nomatch=input
       (MUST use --subsystem-nomatch=input explicitly on the command line)
     Then: udevadm settle --timeout=30
     This auto-loads ALL drivers via modalias matching:
       WiFi, Ethernet, Bluetooth, Sound, I2C, pinctrl, etc.
     TEST: WiFi interface appears without explicit finit_module calls

5.5  Run depmod in post_build.sh
     Add to br2-external/board/llamaste/post_build.sh:
       depmod -a -b ${TARGET_DIR} $(cat ${TARGET_DIR}/usr/include/linux/version.h | ...)
     OR: Buildroot should run depmod automatically with KMOD enabled
     Verify: /lib/modules/<ver>/modules.dep exists in rootfs
     Verify: /lib/modules/<ver>/modules.alias exists in rootfs
     TEST: modules.dep + modules.alias present in squashfs

5.6  Update main.cpp boot sequence
     Replace:
       init_load_modules();
     With:
       init_start_udevd();
       init_load_critical_modules();  // GPU only
       init_udev_trigger_all();       // everything except input
       init_start_dbus();             // for BlueZ
       init_start_bluetoothd();       // Bluetooth daemon
     TEST: Full boot with automatic driver loading

5.7  Remove old init_load_modules() hardcoded list
     Delete the ~130-line module_paths[] array
     Replace entire function body with call to init_load_critical_modules()
     as a backward-compat wrapper (or just delete and rename)
     TEST: No regression — same drivers load via eudev

5.8  Update child_main.cpp — desktop compositor udev handling
     Current: udevd starts inside desktop compositor block
     Change: Remove udevd startup from desktop block (already running)
     Keep: udevadm trigger --subsystem-match=input AFTER Wayland socket
     This is critical for the known wlroots SIGSEGV prevention
     TEST: Desktop mode boots, keyboard works, no SIGSEGV

5.9  Update child_main.cpp — GPU-aware renderer selection
     After detect_hardware() in child_main:
       If GPU detected AND not simpledrm:
         unsetenv("WLR_RENDERER")  // Let wlroots auto-detect GL
         unsetenv("WLR_NO_HARDWARE_CURSORS")
       Else:
         setenv("WLR_RENDERER", "pixman")
         setenv("WLR_NO_HARDWARE_CURSORS", "1")
     TEST: GL renderer used on AMD/Intel, pixman on simpledrm

5.10 Update child_main.cpp — WiFi init no longer needs retry
     eudev + udevadm settle already loaded all WiFi modules
     WiFiManager::init() retry loop can be reduced from 10s to 3s
     (some USB WiFi dongles still need settling time)
     TEST: WiFi connects on first try on ASUS VivoBook

5.11 Update child_main.cpp — wired ethernet scan
     eudev already loaded ethernet modules
     Interface UP + carrier detect flow stays the same
     But modules are already loaded, so no race condition
     TEST: Wired ethernet gets DHCP on Broadcom NIC

COMMIT: "feat: eudev auto-detection — replace hardcoded module list, modalias-based loading"
```

### Step 6: Firmware Expansion + Sound Broadening (Session 3, continued)
**Time estimate**: Same session
**Files**: `br2-external/board/llamaste/linux.config`, `br2-external/configs/llamaste_x86_64_defconfig`

```
6.1  Add broader HDA codec support (modules)
     CONFIG_SND_HDA_CODEC_CA0110=m
     CONFIG_SND_HDA_CODEC_CA0132=m
     CONFIG_SND_HDA_CODEC_CIRRUS=m
     CONFIG_SND_HDA_CODEC_CONEXANT=m
     CONFIG_SND_HDA_CODEC_SIGMATEL=m
     CONFIG_SND_HDA_CODEC_VIA=m
     CONFIG_SND_SOC=m
     CONFIG_SND_SOC_INTEL_SST=m
     CONFIG_SND_SOC_SOF_TOPLEVEL=m
     (Intel SOF for newer laptop audio — Cannon Lake+)
     TEST: Audio works on non-Realtek laptop

6.2  Clean up firmware overlay
     Remove manually-added firmware from overlay that's now in Buildroot:
     - iwlwifi-ty-*.ucode (now via BR2_PACKAGE_LINUX_FIRMWARE_IWLWIFI_*)
     - rtw88/rtw8821c_fw.bin (now via BR2_PACKAGE_LINUX_FIRMWARE_RTL_RTW88)
     Keep in overlay:
     - iwlwifi-gl-* (BE200 — check if Buildroot has it)
     - regulatory.db + .p7s (unless Buildroot wireless-regdb provides it)
     - rtl_nic/* (PXE boot USB ethernet firmware)
     TEST: All existing WiFi chips still get their firmware

COMMIT: "feat: broader audio support + firmware cleanup"
```

### Step 7: Hardware Detection Enhancement (Session 4)
**Time estimate**: 1 session
**Files**: `src/llamaste/hwdetect.cpp`, `src/llamaste/hwdetect.h`

```
7.1  Enhance detect_hardware() — Bluetooth detection
     Scan /sys/class/bluetooth/ for hci0, hci1, etc.
     Report: hw.bluetooth_detected, hw.bluetooth_name
     TEST: BT adapter shows in hardware info

7.2  Enhance detect_hardware() — Network interface enumeration
     Scan /sys/class/net/ for all interfaces
     Report: hw.ethernet_interfaces[], hw.wifi_interfaces[]
     Report: hw.has_ethernet, hw.has_wifi
     TEST: Hardware info page shows all network interfaces

7.3  Enhance detect_hardware() — Touchpad detection
     Scan /sys/bus/i2c/devices/ for HID-over-I2C touchpads
     Scan /sys/class/input/ for touchpad event devices
     Report: hw.has_touchpad, hw.touchpad_name
     TEST: Touchpad detected on laptop

7.4  Enhance detect_hardware() — Battery detection
     Read /sys/class/power_supply/BAT*/status, capacity
     Report: hw.has_battery, hw.battery_percent, hw.battery_status
     TEST: Battery info visible on laptop

7.5  Enhance detect_hardware() — Storage detection
     Scan /sys/block/ for all block devices
     Report: hw.storage_devices[] (name, size, type: SSD/HDD/NVMe/USB)
     TEST: All drives visible in hardware info

7.6  Add hardware info to web UI debug endpoint
     Expand /debug/hardware (or create new) to show all detected hardware
     Include: CPU, RAM, GPU, WiFi, Ethernet, BT, Touchpad, Battery, Storage
     Include: loaded kernel modules (from /proc/modules)
     Include: loaded firmware (from dmesg)
     TEST: Web UI shows comprehensive hardware inventory

7.7  Add hardware info to system prompt
     Update build_system_prompt() in prompt_builder.cpp
     Include: "This device has: [GPU], [WiFi chip], [BT], [touchpad], [battery at X%]"
     LLM can now answer "What hardware does this device have?"
     TEST: LLM correctly reports hardware

COMMIT: "feat: enhanced hardware detection — BT, touchpad, battery, storage, network"
```

### Step 8: Testing + Regression Validation (Session 4, continued)
**Time estimate**: Same session
**Files**: `scripts/run-all-tests.sh`, test scripts

```
8.1  Build full image in WSL2
     cd /root/llamaste-build/output
     make llamaste-dirclean && make llamaste && make
     Verify: squashfs size < 350MB
     Verify: modules.dep + modules.alias in /lib/modules/
     Verify: firmware files present for all supported families

8.2  VirtualBox smoke test
     Deploy to VDI, boot
     Verify: eudev starts, modules auto-load
     Verify: e1000 ethernet works (VBox default NIC)
     Verify: Web UI accessible on port 80
     Verify: No kernel panics or oops in dmesg

8.3  QEMU EFI boot test
     scripts/test-efi-boot.sh
     Verify: simpledrm fallback works (no GPU in QEMU)
     Verify: WLR_RENDERER=pixman set correctly

8.4  Bare metal test — ASUS VivoBook (existing test machine)
     Verify: WiFi (RTL8821CE) still works
     Verify: Touchpad works (was broken, now should work with I2C HID)
     Verify: Audio works
     Verify: Display works (Intel i915)
     Verify: No regressions from previous bare metal tests

8.5  Update QEMU integration tests for new boot sequence
     Adjust timeouts if needed (more modules = slightly longer boot)
     Add assertion: /sys/class/bluetooth/ exists if BT module loaded
     Add assertion: modules.dep exists
     TEST: scripts/run-all-tests.sh --quick passes

COMMIT: "test: hardware compatibility validation on VBox, QEMU, bare metal"
```

### Step 9: Documentation + Cleanup (Session 5)
**Time estimate**: 0.5 session

```
9.1  Update CLAUDE.md with new gotchas
     - eudev module loading replaces hardcoded list
     - init_start_udevd() + init_udev_trigger_all() boot sequence
     - dbus + bluetoothd in init
     - GPU module explicit loading before udev
     - Input trigger delayed until Wayland socket
     - New Buildroot packages (kmod, dbus, bluez5-utils)
     - Firmware overlay cleanup notes

9.2  Update DEVELOPER.md
     - New boot sequence diagram
     - Kernel config categories explained
     - Adding new hardware support guide
     - Firmware addition procedure

9.3  Update SESSION-STATUS.md
     - Phase B hardware compatibility status
     - What's tested, what's pending

9.4  Update INSTALL.md
     - Supported hardware list (now much broader)
     - Known limitations

COMMIT: "docs: hardware compatibility expansion documentation"
```

---

## Phase C: Kernel 6.12 LTS Upgrade (Deferred)

### Timeline: After Phase B is stable and tested (estimated 2-3 sessions after Phase B)

### Step C1: Kernel Upgrade Preparation
**Time estimate**: 1 session

```
C1.1 Update Buildroot kernel version
     BR2_LINUX_KERNEL_CUSTOM_VERSION_VALUE="6.12.x"  # Latest 6.12 LTS
     Full kernel config review — new options in 6.12

C1.2 Migrate linux.config to 6.12
     make linux-menuconfig (or manual merge)
     Handle renamed/removed/new CONFIG options
     Enable new drivers:
       CONFIG_DRM_XE=m           # Intel Xe GPU (Meteor Lake+)
       CONFIG_MT7925=m           # MediaTek WiFi 7
       ath12k improvements       # Better WiFi 7 support
       CONFIG_THUNDERBOLT=m      # Thunderbolt/USB4
       CONFIG_USB_VIDEO_CLASS=m  # Webcams (UVC)
       CONFIG_MEDIA_SUPPORT=m    # V4L2 framework
       CONFIG_VIDEO_DEV=m
       CONFIG_INTEL_IOMMU_SVM=y  # Shared virtual memory

C1.3 Update Buildroot
     May need Buildroot version bump for kernel 6.12 support
     Check package compatibility

COMMIT: "feat: kernel upgrade to 6.12 LTS"
```

### Step C2: New 6.12 Features
**Time estimate**: 1 session

```
C2.1 Intel Xe GPU support
     CONFIG_DRM_XE=m
     Mesa xe driver (if separate from iris)
     Firmware for Meteor Lake / Lunar Lake / Arrow Lake
     TEST: Display output on Intel Core Ultra laptop

C2.2 WiFi 7 full support
     MediaTek MT7925 driver + firmware
     Qualcomm ath12k improvements
     Intel BE200/BE202 WiFi 7 improvements
     TEST: WiFi 7 connection at >1Gbps

C2.3 Thunderbolt/USB4
     CONFIG_THUNDERBOLT=m
     CONFIG_USB4=m
     Dock support (ethernet, display via USB-C)
     TEST: Thunderbolt dock devices enumerated

C2.4 Webcam (UVC) support
     CONFIG_MEDIA_SUPPORT=m
     CONFIG_USB_VIDEO_CLASS=m
     CONFIG_VIDEO_DEV=m
     Add v4l-utils to Buildroot (optional)
     TEST: /dev/video0 appears with USB webcam

C2.5 CPU microcode loading
     CONFIG_MICROCODE=y
     CONFIG_MICROCODE_INTEL=y
     CONFIG_MICROCODE_AMD=y
     BR2_PACKAGE_LINUX_FIRMWARE_INTEL_CPU_MICROCODE=y
     BR2_PACKAGE_LINUX_FIRMWARE_AMD_CPU_MICROCODE=y
     TEST: dmesg shows microcode updated

COMMIT: "feat: kernel 6.12 features — Xe GPU, WiFi 7, Thunderbolt, webcam, microcode"
```

### Step C3: Regression Testing (Full)
**Time estimate**: 1 session

```
C3.1 Full regression suite
     All QEMU tests: install, cluster, update, recovery
     VirtualBox smoke test
     Bare metal ASUS VivoBook
     New bare metal target if available (AMD Ryzen desktop)

C3.2 Performance comparison
     Boot time: 6.6 vs 6.12
     Inference speed: verify no regression
     Module load time comparison

COMMIT: "test: kernel 6.12 regression testing complete"
```

---

## Summary Timeline

| Phase | Step | Description | Sessions | Cumulative |
|-------|------|-------------|----------|------------|
| **B** | 1 | Kernel config — platform (I2C, HID, IOMMU, ACPI, pinctrl) | 1 | 1 |
| **B** | 2 | Kernel config — ethernet + WiFi expansion | (same) | 1 |
| **B** | 3 | Kernel config — GPU (amdgpu, nouveau, Mesa) | 1 | 2 |
| **B** | 4 | Bluetooth (BlueZ, dbus, kernel BT, pairing) | (same) | 2 |
| **B** | 5 | eudev auto-detection (replace hardcoded modules) | 1 | 3 |
| **B** | 6 | Firmware expansion + sound broadening | (same) | 3 |
| **B** | 7 | Hardware detection enhancement | 1 | 4 |
| **B** | 8 | Testing + regression validation | (same) | 4 |
| **B** | 9 | Documentation + cleanup | 0.5 | 4.5 |
| | | **Phase B Total** | | **~5-7 sessions** |
| **C** | C1 | Kernel 6.12 upgrade + migration | 1 | 5.5 |
| **C** | C2 | New 6.12 features (Xe, WiFi 7, TB, webcam) | 1 | 6.5 |
| **C** | C3 | Full regression testing | 1 | 7.5 |
| | | **Phase B + C Total** | | **~8-10 sessions** |

## Risk Register

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| Buildroot kmod + eudev integration broken | High | Low | kmod is well-tested; eudev has built-in kmod support |
| amdgpu module too large for squashfs | Medium | Low | Squashfs zstd compression; firmware is biggest chunk |
| BlueZ dbus dependency adds complexity | Medium | Medium | dbus is lightweight; restart logic in supervisor |
| Kernel config regression (wrong option breaks boot) | High | Medium | Incremental approach; test after each step |
| Boot time increase (more modules) | Low | Medium | eudev parallel probing; settle timeout |
| Mesa LLVM dependency bloat | Medium | Medium | ~15-20MB compressed; acceptable for GPU accel |
| Overlay firmware conflicts with Buildroot | Low | Medium | Clean up overlay in Step 6.2 |

## Dependencies

- Buildroot 2024.02.x must support kmod, dbus, bluez5-utils (it does)
- Linux 6.6.70 must support all Phase B drivers (verified)
- WSL2 build environment available
- VirtualBox + ASUS VivoBook for testing
- USB WiFi dongle(s) for testing (nice-to-have)
- Bluetooth USB dongle for testing (nice-to-have)
