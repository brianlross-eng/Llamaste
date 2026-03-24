# Bluetooth HID Support for Llamaste

**Date**: 2026-03-23
**Status**: Research complete
**Purpose**: Enable Bluetooth keyboards/mice in desktop mode (labwc Wayland compositor)

---

## 1. Kernel Configuration

### Required CONFIG Options

Bluetooth HID requires a dependency chain of kernel options:

```
# Core Bluetooth subsystem
CONFIG_BT=m                    # Bluetooth subsystem support
CONFIG_BT_BREDR=y              # Bluetooth Classic (BR/EDR) - needed for HIDP
CONFIG_BT_LE=y                 # Bluetooth Low Energy (BLE) - needed for HoG (BLE keyboards/mice)
CONFIG_RFKILL=y                # RF switch subsystem (already likely enabled for WiFi)

# HID Profile (Classic Bluetooth keyboards/mice)
CONFIG_BT_HIDP=m               # HIDP protocol - depends on BT_BREDR && HID
                               # This is the classic Bluetooth HID transport

# HID subsystem (should already be enabled for USB HID)
CONFIG_HID=y                   # HID bus support
CONFIG_HID_GENERIC=y           # Generic HID driver
CONFIG_UHID=y                  # User-space I/O driver for HID - needed for BLE HoG
CONFIG_INPUT=y                 # Generic input layer (should already be =y)

# Crypto dependencies (Bluetooth uses these for pairing)
CONFIG_CRYPTO=y                # Already enabled
CONFIG_CRYPTO_CMAC=m           # CMAC for SMP pairing
CONFIG_CRYPTO_ECB=m            # ECB block cipher
CONFIG_CRYPTO_AES=y            # AES cipher
CONFIG_CRYPTO_ECDH=m           # ECDH for LE Secure Connections
CONFIG_CRYPTO_USER_API_HASH=m  # Optional, used by some BT features

# L2CAP (Logical Link Control and Adaptation Protocol)
# Auto-selected by CONFIG_BT, no separate option needed
```

### Bluetooth Controller Drivers

```
# USB Bluetooth dongles (most common on x86_64 desktops/laptops)
CONFIG_BT_HCIBTUSB=m           # HCI USB driver - depends on USB
CONFIG_BT_HCIBTUSB_BCM=y       # Broadcom protocol support (sub-option of HCIBTUSB)
CONFIG_BT_HCIBTUSB_MTK=y       # MediaTek protocol support (sub-option)
CONFIG_BT_HCIBTUSB_RTL=y       # Realtek protocol support (sub-option, default=y)

# Intel PCIe Bluetooth (newer Intel WiFi+BT combo cards like AX210, BE200)
CONFIG_BT_INTEL_PCIE=m          # Intel HCI PCIe driver - selects BT_INTEL, FW_LOADER

# Vendor firmware helpers (auto-selected by drivers above)
CONFIG_BT_INTEL=m              # Intel firmware helper (selected by HCIBTUSB, INTEL_PCIE)
CONFIG_BT_BCM=m                # Broadcom firmware helper (selected by HCIBTUSB_BCM)
CONFIG_BT_RTL=m                # Realtek firmware helper (selected by HCIBTUSB_RTL)
CONFIG_BT_QCA=m                # Qualcomm/Atheros firmware helper
CONFIG_BT_MTK=m                # MediaTek firmware helper

# UART Bluetooth (common in embedded, rare on x86_64 desktops)
# CONFIG_BT_HCIUART=m          # Only if targeting UART-attached BT controllers
# Most x86_64 laptops use USB or PCIe, not UART for Bluetooth

# SDIO Bluetooth (some embedded platforms)
# CONFIG_BT_HCIBTSDIO=m        # Only if targeting SDIO-attached BT controllers
```

### Built-in vs Module Recommendation

**Use modules (=m) for Bluetooth**, not built-in (=y). Reasons:
1. **Firmware loading**: BT drivers need firmware from `/lib/firmware/` which is only available after squashfs pivot. Built-in drivers try to load firmware too early (before rootfs is mounted).
2. **Consistency**: This matches the existing WiFi driver strategy (CONFIG_MODULES=y, loaded via `init_load_modules()` after squashfs pivot).
3. **Size**: Bluetooth modules only loaded when BT hardware is present.
4. **Exception**: `CONFIG_BT_BREDR=y`, `CONFIG_BT_LE=y`, `CONFIG_HID=y`, `CONFIG_UHID=y` should be built-in as they're protocol/subsystem features, not hardware drivers.

### Module Load Order

Add to `init_load_modules()` in init.cpp:
```
// Bluetooth modules (after squashfs pivot, firmware available)
bluetooth.ko          // Core BT subsystem
btintel.ko           // Intel firmware helper
btrtl.ko             // Realtek firmware helper
btbcm.ko             // Broadcom firmware helper
btmtk.ko             // MediaTek firmware helper
btusb.ko             // USB HCI driver (depends on bluetooth.ko + vendor helpers)
btintel_pcie.ko      // Intel PCIe driver (for newer Intel cards)
hidp.ko              // Bluetooth HID Profile (depends on bluetooth.ko)
```

---

## 2. Userspace Bluetooth Stack

### Option A: BlueZ (Recommended)

BlueZ is the official Linux Bluetooth stack. It provides:
- `bluetoothd` daemon — manages BT adapters, discovery, pairing, connection
- `bluetoothctl` — CLI tool for interactive pairing
- D-Bus API — primary programmatic interface
- HID plugin — handles Bluetooth Classic HID (keyboard/mouse)
- HoG plugin — handles Bluetooth LE HID over GATT (BLE keyboards/mice)

#### BlueZ Dependencies

**Hard dependencies (cannot avoid):**
- **dbus** — BlueZ 5.x uses D-Bus as its IPC mechanism. Cannot build without it.
- **libglib2** — GLib main loop, data structures. Cannot build without it.

**Optional dependencies:**
- libical — only for OBEX (file transfer). Not needed for HID.
- readline — only for `bluetoothctl` CLI. Can be disabled if using programmatic control.
- systemd — NOT required. bluetoothd runs fine standalone.

#### Buildroot BlueZ5 Packages

```
# Required
BR2_PACKAGE_BLUEZ5_UTILS=y           # Main BlueZ5 package
# Auto-selects: BR2_PACKAGE_DBUS, BR2_PACKAGE_LIBGLIB2

# HID support plugins
BR2_PACKAGE_BLUEZ5_UTILS_PLUGINS_HID=y   # Classic Bluetooth HID (keyboard/mouse)
BR2_PACKAGE_BLUEZ5_UTILS_PLUGINS_HOG=y   # BLE HID over GATT (BLE keyboard/mouse)

# Tools (pick what you need)
BR2_PACKAGE_BLUEZ5_UTILS_CLIENT=y    # bluetoothctl (interactive pairing)
# BR2_PACKAGE_BLUEZ5_UTILS_TOOLS=y   # btmgmt, btmon, etc (debug tools)
# BR2_PACKAGE_BLUEZ5_UTILS_MONITOR=y # btmon (BT packet monitor)
# BR2_PACKAGE_BLUEZ5_UTILS_DEPRECATED_TOOLS=y  # hcitool, hciconfig (legacy)

# NOT needed for HID
# BR2_PACKAGE_BLUEZ5_UTILS_OBEX=y    # File transfer (needs libical + C++)
# BR2_PACKAGE_BLUEZ5_UTILS_MESH=y    # BT Mesh (IoT)
# BR2_PACKAGE_BLUEZ5_UTILS_AUDIO=y   # A2DP/SCO audio profiles
```

#### Buildroot Dependencies Chain

```
BR2_PACKAGE_BLUEZ5_UTILS
  ├── BR2_USE_WCHAR=y              (already enabled for musl)
  ├── BR2_TOOLCHAIN_HAS_THREADS=y  (already enabled)
  ├── BR2_USE_MMU=y                (x86_64, already enabled)
  ├── !BR2_STATIC_LIBS             (already dynamic linking)
  ├── BR2_TOOLCHAIN_HEADERS_AT_LEAST_3_4  (already ≥ 3.4)
  ├── BR2_TOOLCHAIN_HAS_SYNC_4    (already enabled)
  ├── BR2_PACKAGE_DBUS             (auto-selected)
  │   └── expat or libxml2
  └── BR2_PACKAGE_LIBGLIB2         (auto-selected)
      ├── BR2_PACKAGE_LIBFFI
      ├── BR2_PACKAGE_PCRE2
      └── BR2_PACKAGE_ZLIB
```

**D-Bus size impact**: dbus + libdbus is ~500KB. libglib2 is ~2.5MB. This is the main cost of BlueZ.

#### Running bluetoothd Without systemd

bluetoothd does NOT require systemd. It can run standalone:

```bash
# Run in foreground (no detach) — ideal for Llamaste's PID 1 supervisor
bluetoothd --nodetach --debug --configfile /etc/bluetooth/main.conf
```

For Llamaste, launch bluetoothd as a supervised child process (similar to wpa_supplicant):
1. `fork()` + `execv("/usr/libexec/bluetooth/bluetoothd", args)`
2. Args: `--nodetach` (foreground), `-f /etc/bluetooth/main.conf`
3. Requires dbus-daemon running first (bluetoothd connects to system bus)

**dbus-daemon requirement**: bluetoothd communicates via D-Bus system bus. Must start `dbus-daemon --system` before bluetoothd. Buildroot's dbus package provides the daemon. dbus-daemon needs `/var/run/dbus/` directory and `/etc/dbus-1/system.conf`.

#### Minimal BlueZ Configuration

`/etc/bluetooth/main.conf`:
```ini
[General]
# Name that appears to other BT devices
Name = Llamaste

# Only enable input (keyboard/mouse) profiles
# Disable audio, file transfer, etc
Discoverable = true
DiscoverableTimeout = 0

[Policy]
AutoEnable=true

[GATT]
# Enable for BLE HID keyboards/mice
```

`/etc/bluetooth/input.conf`:
```ini
[General]
# Use kernel HIDP for classic BT HID
UserspaceHID=false

# Idle timeout (0 = never disconnect)
IdleTimeout=0
```

Setting `UserspaceHID=false` uses the kernel's HIDP driver directly, which is simpler and more reliable for basic keyboard/mouse.

### Option B: Without BlueZ (tinyhidd)

[tinyhidd](https://github.com/abrasive/tinyhidd) is a minimal BT HID daemon that:
- Uses BTstack (not BlueZ) to access BT hardware directly
- No dbus dependency
- No libglib dependency
- Only ~500 lines of C code
- Supports keyboard and mouse pairing

**Problems with tinyhidd for Llamaste:**
- Requires BTstack daemon running (another dependency)
- BTstack takes exclusive control of BT hardware (no other BT features possible)
- Only 4 commits, last update years ago — essentially unmaintained
- No BLE HoG support (modern BLE keyboards won't work)
- No Buildroot package — would need custom packaging

**Verdict: Use BlueZ.** The dbus+glib overhead (~3MB) is acceptable. BlueZ is actively maintained, handles all BT variants (Classic + BLE), and has Buildroot packaging ready.

### Option C: Kernel-Only BT HID (No Userspace Daemon)

In theory, kernel HIDP + kernel BT subsystem can handle reconnection of *already-paired* devices without any userspace daemon. But:
- **Initial pairing requires userspace** — the SMP (Security Manager Protocol) pairing exchange needs a userspace agent to handle PIN/passkey
- **Discovery requires userspace** — scanning for devices is done via HCI commands from userspace
- **BLE HoG requires userspace** — GATT attribute protocol is handled in userspace by bluetoothd

**Verdict: Not viable.** You always need userspace for initial pairing. After pairing, kernel HIDP handles reconnection, but bluetoothd manages the adapter state.

---

## 3. Bluetooth Firmware

### Firmware by Vendor

#### Intel (ibt-*.sfi, ibt-*.ddc)
- **Files**: `intel/ibt-{hw}-{fw}.sfi` (main firmware) + `.ddc` (device config)
- **Typical size**: 700-800 KB per .sfi file, ~2KB per .ddc file
- **Chips**: AX200 (ibt-20), AX201 (ibt-20), AX210 (ibt-0041), BE200 (ibt-0180)
- **Total for common Intel chips**: ~5-8 MB
- **Note**: Intel BT firmware is separate from Intel WiFi firmware (iwlwifi-*)
- **Driver**: btusb.ko (USB) or btintel_pcie.ko (PCIe, newer chips)

#### Realtek (rtl_bt/*.bin)
- **Files**: `rtl_bt/rtl8xxxxx_fw.bin` + `rtl_bt/rtl8xxxxx_config.bin`
- **Typical size**: 30-60 KB per fw file, ~1KB per config file
- **Chips**: RTL8761B (USB dongle), RTL8821C, RTL8822C, RTL8852A
- **Total for common Realtek chips**: ~2-4 MB
- **Note**: RTL8821CE (in test laptop) has BT via USB interface internally
- **Driver**: btusb.ko with BT_RTL helper

#### Broadcom/Cypress (brcm/*.hcd)
- **Files**: `brcm/BCMxxxx-xxxx-xxxx.hcd` (patchram files)
- **Typical size**: 20-30 KB per file
- **Chips**: BCM20702 (common USB dongle), BCM4356, BCM43142
- **Total for common Broadcom chips**: ~1-2 MB
- **Driver**: btusb.ko with BT_BCM helper, or btbcm203x.ko

#### Qualcomm/Atheros (qca/*.bin)
- **Files**: `qca/htbtfw*.tlv` + `qca/htnv*.bin`
- **Typical size**: 50-100 KB per file
- **Chips**: QCA6174, QCA9377, WCN3991
- **Total for common QCA chips**: ~2-3 MB
- **Driver**: btusb.ko with BT_QCA helper

#### MediaTek (mediatek/*.bin)
- **Files**: `mediatek/BT_RAM_CODE_*.bin`
- **Typical size**: 500 KB - 1 MB per file
- **Chips**: MT7921, MT7922 (WiFi 6/6E combo)
- **Total for common MTK chips**: ~3-5 MB
- **Driver**: btusb.ko with BT_MTK helper, or btmtksdio.ko

### Firmware Strategy for Llamaste

**Option 1: Include all BT firmware** (~15-22 MB)
- Copy entire `intel/`, `rtl_bt/`, `brcm/`, `qca/`, `mediatek/` dirs from linux-firmware
- Pros: Works with any BT hardware
- Cons: Adds ~20 MB to image

**Option 2: Include only common chipsets** (~5-8 MB)
- Intel ibt-20/0041/0180 (covers AX200/201/210, BE200)
- Realtek rtl8761b, rtl8821c, rtl8852a (common USB dongles + laptops)
- Broadcom BCM20702 (most common USB dongle)
- Pros: Covers 80%+ of hardware
- Cons: Some rare chipsets won't work

**Option 3: Dynamic firmware download** (0 MB base)
- Download firmware on first BT use from DATA partition or network
- Pros: No image bloat
- Cons: Complex, BT won't work offline without prior download

**Recommendation**: Option 2. Match the WiFi firmware strategy — include firmware for the most common chipsets in the overlay. The existing overlay already has WiFi firmware (~10 MB), adding BT firmware for ~5-8 MB is acceptable.

---

## 4. Bluetooth Controller Types

### USB Bluetooth Dongles
- **Most common type** for adding BT to desktops
- **Driver**: `btusb.ko` (CONFIG_BT_HCIBTUSB)
- **Examples**: CSR8510 (very common cheap dongle), Broadcom BCM20702, Intel 8265
- **Detection**: USB class 0xE0, subclass 0x01, protocol 0x01
- **Firmware**: Loaded at USB probe time by btusb driver
- **No special setup needed** — just load btusb.ko module

### PCIe/M.2 Combo Cards (WiFi + BT)
- **Common in laptops** — Intel AX200/201/210, Realtek RTL8821CE, MediaTek MT7921
- **BT interface**: Even on PCIe WiFi cards, the BT controller usually appears as a **USB device** on an internal USB bus
- **Driver**: Still `btusb.ko` for most (Intel, Realtek, Broadcom)
- **Exception**: Newer Intel cards (BE200+) may use `btintel_pcie.ko` (direct PCIe BT)
- **Note**: The test laptop's RTL8821CE has its BT controller at USB, handled by btusb.ko

### UART-Based
- **Common in embedded/ARM** — Raspberry Pi (BCM43438), some Qualcomm SoCs
- **Driver**: `hci_uart.ko` (CONFIG_BT_HCIUART)
- **Requires**: `hciattach` or `btattach` tool to initialize the UART port
- **Rare on x86_64 desktops/laptops** — can skip for initial Llamaste BT support

---

## 5. Pairing, Reconnection, and Storage

### Pairing Flow
1. User initiates discovery (bluetoothctl: `scan on`)
2. Device found, user initiates pairing (`pair XX:XX:XX:XX:XX:XX`)
3. SMP exchange — PIN/passkey displayed or confirmed
4. Link keys exchanged and stored
5. Device marked as trusted (`trust XX:XX:XX:XX:XX:XX`)
6. Device connected (`connect XX:XX:XX:XX:XX:XX`)

### Pairing Info Storage
BlueZ stores pairing data in `/var/lib/bluetooth/`:
```
/var/lib/bluetooth/
  <adapter-MAC>/
    settings           # Adapter settings
    <device-MAC>/
      info              # Device info, link keys, name, type
      attributes        # GATT attribute cache (BLE devices)
```

The `info` file contains the link key (encryption key) needed for reconnection.

### Reconnection on Boot
1. bluetoothd starts and reads `/var/lib/bluetooth/` for known devices
2. Adapter is powered on (`AutoEnable=true` in main.conf)
3. For Classic BT devices: adapter listens for page requests from known devices
4. For BLE devices: adapter listens for directed advertisements from known devices
5. Device reconnects automatically using stored link keys
6. **No user interaction needed** after initial pairing

### Storage Strategy for Llamaste
- Symlink `/var/lib/bluetooth` → `/data/bluetooth/` (DATA partition)
- Pairing info persists across squashfs updates (A/B system)
- On first boot or factory reset, `/data/bluetooth/` is empty
- After pairing, data is written to DATA partition
- On A/B update, DATA partition untouched → BT devices stay paired

---

## 6. USB Wireless (2.4GHz RF) Keyboards and Mice

### No Bluetooth Needed
Most "wireless" keyboards/mice use proprietary 2.4GHz RF with a USB dongle, NOT Bluetooth:
- **Logitech Unifying** — single USB receiver for up to 6 devices
- **Logitech Bolt** — newer, similar concept
- **Generic 2.4GHz** — cheap wireless KB/mouse with dedicated USB dongle
- **Microsoft wireless** — USB transceiver based

### Kernel Support
These devices appear as standard **USB HID devices**. They need:
```
CONFIG_USB_HID=y               # USB HID transport (should already be =y)
CONFIG_HID_GENERIC=y           # Generic HID driver
```

**No additional support needed.** They work identically to wired USB keyboards/mice. The dongle handles all RF communication transparently.

### Logitech Unifying (Optional Enhancement)
```
CONFIG_HID_LOGITECH=m          # Logitech HID++ protocol
CONFIG_HID_LOGITECH_DJ=m       # Logitech Unifying multi-device
CONFIG_HID_LOGITECH_HIDPP=m    # Logitech HID++ features (battery, DPI)
```
These are optional — Logitech devices work without them (just as basic HID), but these modules enable pairing new devices to the Unifying receiver and expose battery level.

---

## 7. Implementation Plan for Llamaste

### Phase 1: Kernel Config (Minimal)
Add to `linux.config`:
```
# Bluetooth core
CONFIG_BT=m
CONFIG_BT_BREDR=y
CONFIG_BT_LE=y
CONFIG_BT_HIDP=m
CONFIG_UHID=y

# Crypto for BT pairing
CONFIG_CRYPTO_CMAC=m
CONFIG_CRYPTO_ECB=m
CONFIG_CRYPTO_ECDH=m

# USB Bluetooth driver (covers 95% of BT hardware)
CONFIG_BT_HCIBTUSB=m
CONFIG_BT_HCIBTUSB_BCM=y
CONFIG_BT_HCIBTUSB_MTK=y
CONFIG_BT_HCIBTUSB_RTL=y

# Intel PCIe Bluetooth (newer Intel combo cards)
CONFIG_BT_INTEL_PCIE=m

# Vendor firmware helpers (auto-selected but list for clarity)
CONFIG_BT_INTEL=m
CONFIG_BT_BCM=m
CONFIG_BT_RTL=m
CONFIG_BT_MTK=m
```

### Phase 2: Buildroot Packages
Add to `llamaste_defconfig`:
```
# Bluetooth userspace
BR2_PACKAGE_BLUEZ5_UTILS=y
BR2_PACKAGE_BLUEZ5_UTILS_PLUGINS_HID=y
BR2_PACKAGE_BLUEZ5_UTILS_PLUGINS_HOG=y
BR2_PACKAGE_BLUEZ5_UTILS_CLIENT=y

# Dependencies (auto-selected but verify)
BR2_PACKAGE_DBUS=y
BR2_PACKAGE_LIBGLIB2=y
```

### Phase 3: Firmware
Add to overlay or build-iso.sh firmware copy:
```bash
# Intel BT firmware
cp -a linux-firmware/intel/ibt-* overlay/lib/firmware/intel/

# Realtek BT firmware
cp -a linux-firmware/rtl_bt/ overlay/lib/firmware/rtl_bt/

# Broadcom BT firmware (common dongles)
mkdir -p overlay/lib/firmware/brcm/
cp linux-firmware/brcm/BCM20702*.hcd overlay/lib/firmware/brcm/
cp linux-firmware/brcm/BCM4356*.hcd overlay/lib/firmware/brcm/
```

### Phase 4: System Integration
1. **init.cpp**: Add BT modules to `init_load_modules()` load order
2. **supervisor.cpp**: Start `dbus-daemon --system` before bluetoothd
3. **child_main.cpp**: Start `bluetoothd --nodetach -f /etc/bluetooth/main.conf`
4. **child_main.cpp**: Symlink `/var/lib/bluetooth` → `/data/bluetooth/`
5. **Web UI**: Add Bluetooth pairing page (scan, pair, trust, connect)
6. **API**: Add `/llamaste/bluetooth/scan`, `/llamaste/bluetooth/pair`, `/llamaste/bluetooth/devices` endpoints

### Phase 5: Web UI Bluetooth Controls
- Scan for nearby BT devices
- Display device name, type (keyboard/mouse/other), signal strength
- Pair button with PIN/passkey confirmation
- List paired devices with connect/disconnect/forget actions
- Status indicator showing BT adapter state

---

## 8. Size Impact Estimate

| Component | Size |
|-----------|------|
| Kernel BT modules (.ko) | ~500 KB |
| BlueZ (bluetoothd + bluetoothctl) | ~1.5 MB |
| dbus-daemon + libdbus | ~500 KB |
| libglib2 | ~2.5 MB |
| BT firmware (common chipsets) | ~5-8 MB |
| **Total** | **~10-13 MB** |

This is comparable to the existing WiFi stack (wpa_supplicant + firmware ~10 MB).

---

## 9. Risks and Gotchas

1. **dbus-daemon must start before bluetoothd** — bluetoothd connects to system D-Bus. If dbus isn't running, bluetoothd exits immediately.
2. **dbus needs /var/run/dbus/ directory** — must be created at boot (tmpfs).
3. **BT firmware must be available before btusb loads** — use module loading (not built-in) so firmware is available from squashfs.
4. **BLE keyboards need HoG plugin** — many modern keyboards are BLE-only. Without the HoG (Host over GATT) plugin, they won't work.
5. **Pairing via web UI requires D-Bus agent** — bluetoothd's pairing agent runs over D-Bus. The web UI will need to communicate with bluetoothd via D-Bus (or use `bluetoothctl` subprocess).
6. **libglib2 pulls in pcre2, libffi, zlib** — check if these are already in the build. If not, adds another ~1 MB.
7. **USB power management** — some BT dongles auto-suspend and fail to wake. May need `CONFIG_BT_HCIBTUSB_AUTOSUSPEND=n` or udev rules.
8. **Combo WiFi+BT cards share antenna** — on PCIe combo cards, BT and WiFi coexist on the same radio. The kernel handles coexistence automatically, but both drivers must be loaded.
9. **Kernel HIDP vs userspace HID**: Set `UserspaceHID=false` in `/etc/bluetooth/input.conf` to use kernel HIDP for classic BT keyboards/mice. This is more reliable and lower latency.

---

## 10. References

- [Arch Wiki: Bluetooth](https://wiki.archlinux.org/title/Bluetooth)
- [Gentoo Wiki: Bluetooth input devices](https://wiki.gentoo.org/wiki/Bluetooth_input_devices)
- [BlueZ official site](https://www.bluez.org/)
- [Linux kernel Bluetooth Kconfig](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/Kconfig)
- [Linux kernel HIDP Kconfig](https://github.com/torvalds/linux/blob/master/net/bluetooth/hidp/Kconfig)
- [Buildroot BlueZ5 package](https://github.com/buildroot/buildroot/blob/master/package/bluez5_utils/bluez5_utils.mk)
- [tinyhidd - minimal BT HID daemon](https://github.com/abrasive/tinyhidd)
- [Linux From Scratch: BlueZ-5.86](https://www.linuxfromscratch.org/blfs/view/svn/general/bluez.html)
- [Alpine Wiki: Bluetooth](https://wiki.alpinelinux.org/wiki/Bluetooth)
