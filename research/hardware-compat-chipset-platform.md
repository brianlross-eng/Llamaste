# Hardware Compatibility: Chipset, ACPI, IOMMU & Platform Support

**Date**: 2026-03-23
**Kernel**: 6.6.70 (Buildroot 2024.02)
**Context**: Llamaste must boot on 10-year-old office PCs through modern gaming laptops

## Current State Assessment

The current `linux.config` has **critical gaps** for real hardware:
- No I2C support → touchpads broken on most modern laptops
- No HID bus → USB keyboards/mice may work but HID-over-I2C devices dead
- No pinctrl → Intel LPSS and AMD GPIO non-functional
- No IOMMU → some BIOS configurations may fail to boot
- No backlight control → laptop screens stuck at BIOS brightness
- No MMC/SD → card readers dead
- No power supply reporting → battery status unknown
- No PCIe hotplug → Thunderbolt docks dead
- No Intel/AMD platform device support → chipset peripherals invisible

---

## 1. Intel Chipset / PCH Support

### Essential (MUST HAVE)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_X86_INTEL_LPSS=y` | Intel Low Power Subsystem | Required for I2C, SPI, UART on Haswell+ (2013+). Without this, touchpads and sensors on Intel laptops are invisible. |
| `CONFIG_MFD_INTEL_LPSS_PCI=m` | LPSS devices in PCI mode | Skylake+ PCH (2015+). Enables I2C controllers behind PCI. |
| `CONFIG_MFD_INTEL_LPSS_ACPI=m` | LPSS devices in ACPI mode | Broadwell and some Skylake. Same devices, different enumeration. |
| `CONFIG_I2C=y` | I2C core | **CRITICAL.** Required for touchpads, sensors, display backlights on modern hardware. |
| `CONFIG_I2C_CHARDEV=m` | I2C /dev entries | Useful for diagnostics. |
| `CONFIG_I2C_I801=m` | Intel ICH/PCH SMBus | Covers every Intel chipset from ICH5 (2003) through modern PCH. SPD reading, hardware monitoring, etc. |
| `CONFIG_I2C_DESIGNWARE_CORE=m` | Synopsys DesignWare I2C core | Used by Intel LPSS I2C controllers internally. |
| `CONFIG_I2C_DESIGNWARE_PLATFORM=m` | DesignWare platform driver | ACPI-enumerated I2C on Intel LPSS. |
| `CONFIG_I2C_DESIGNWARE_PCI=m` | DesignWare PCI driver | PCI-enumerated I2C on Intel LPSS. |
| `CONFIG_LPC_ICH=m` | Intel ICH LPC bridge | Watchdog, GPIO on older Intel chipsets. |
| `CONFIG_LPC_SCH=m` | Intel SCH LPC bridge | Atom-based systems (some mini PCs). |

### Pinctrl (Required for LPSS GPIO)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_PINCTRL=y` | Pin control subsystem | Core framework, required by all pinctrl drivers. |
| `CONFIG_PINCTRL_INTEL=m` | Intel pinctrl core | Common code for all Intel PCH pin controllers. |
| `CONFIG_PINCTRL_SUNRISEPOINT=m` | Skylake/Kaby Lake PCH | 6th-7th gen Intel (2015-2017). |
| `CONFIG_PINCTRL_CANNONLAKE=m` | Coffee Lake PCH | 8th-9th gen Intel (2018-2019). |
| `CONFIG_PINCTRL_TIGERLAKE=m` | Tiger Lake PCH | 11th gen Intel (2020-2021). |
| `CONFIG_PINCTRL_ALDERLAKE=m` | Alder Lake PCH | 12th gen Intel (2021-2022). |
| `CONFIG_PINCTRL_RAPTORLAKE=m` | Raptor Lake PCH | 13th-14th gen Intel (2022-2024). |
| `CONFIG_PINCTRL_METEORLAKE=m` | Meteor Lake PCH | Core Ultra 1st gen (2023+). |
| `CONFIG_PINCTRL_LEWISBURG=m` | Lewisburg PCH | Server C620 series. |
| `CONFIG_PINCTRL_BROXTON=m` | Broxton/Apollo Lake | Low-end NUCs and mini PCs. |
| `CONFIG_PINCTRL_GEMINILAKE=m` | Gemini Lake | Budget laptops/NUCs (2017-2019). |
| `CONFIG_PINCTRL_BAYTRAIL=y` | Bay Trail | Atom tablets/mini PCs (2013-2016). Many still in use. Built-in because these often need GPIO for boot. |
| `CONFIG_PINCTRL_CHERRYVIEW=m` | Cherry View | Atom x5/x7 (2015-2016). |
| `CONFIG_PINCTRL_LYNXPOINT=m` | Haswell/Broadwell PCH | 4th-5th gen Intel. |
| `CONFIG_PINCTRL_ELKHARTLAKE=m` | Elkhart Lake | IoT/embedded Intel (2020+). |
| `CONFIG_PINCTRL_JASPERLAKE=m` | Jasper Lake | Budget N-series (2021+). |
| `CONFIG_PINCTRL_LAKEFIELD=m` | Lakefield | Hybrid architecture predecessor. |
| `CONFIG_PINCTRL_EMMITSBURG=m` | Emmitsburg | Server platform. |

### Intel Thermal / Power (Nice-to-Have for Laptops)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_INTEL_PCH_THERMAL=m` | PCH thermal sensor | Reports PCH temperature. |
| `CONFIG_INTEL_RAPL=m` | Running Average Power Limit | Power monitoring for Intel CPUs. Useful for thermal management. |
| `CONFIG_INT340X_THERMAL=m` | Intel SoC DTS thermal | Dynamic thermal sensor on modern Intel SoCs. |
| `CONFIG_INTEL_POWERCLAMP=m` | Intel powerclamp | Emergency thermal throttle by injecting idle. |
| `CONFIG_ACPI_DPTF=m` | Intel DPTF | Dynamic Platform and Thermal Framework. Coordinates cooling. |

### Intel Management Engine (Recommended)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_INTEL_MEI=m` | Intel ME Interface | Present on all Intel since 2008. Some firmware updates and platform features depend on it. |
| `CONFIG_INTEL_MEI_ME=m` | ME Interface (ME) | Standard desktop/laptop ME. |
| `CONFIG_INTEL_MEI_TXE=m` | ME Interface (TXE) | Atom/Celeron/Pentium variant. |

---

## 2. AMD Chipset Support

### Essential

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_X86_AMD_PLATFORM_DEVICE=y` | AMD ACPI-to-platform devices | **CRITICAL for AMD.** Translates AMD ACPI devices (I2C, UART, GPIO) to platform devices. Carrizo (2015) and later. Without this, touchpads and sensors on AMD laptops are invisible. |
| `CONFIG_I2C_PIIX4=m` | AMD SMBus (PIIX4-compatible) | AMD SB700, SB800, SP5100, Hudson-2, Bolton, Zen chipsets. Covers virtually all AMD systems from 2008+. |
| `CONFIG_PINCTRL_AMD=m` | AMD GPIO pin control | GPIO on AMD Carrizo+ SoCs. Required for some touchpad interrupt routing. |

### AMD Thermal / Power

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_AMD_PMC=m` | AMD Platform Management Controller | Power management on Zen 2+ (Ryzen 3000+). S2idle support. |
| `CONFIG_K10TEMP=m` | AMD K10 temperature sensor | Works on Family 10h through Zen 4. |
| `CONFIG_SENSORS_K10TEMP=m` | Alternate config name | Same driver, depends on Kconfig version. |
| `CONFIG_AMD_NB=y` | AMD Northbridge support | Auto-selected by various AMD drivers. |

---

## 3. ACPI and Power Management

### Essential ACPI (MUST HAVE)

| Config Option | Purpose | Current | Recommendation |
|---|---|---|---|
| `CONFIG_ACPI=y` | ACPI core | YES | Keep. |
| `CONFIG_ACPI_EC=y` | Embedded Controller | auto | Verify enabled. Essential for keyboard, battery, thermal on laptops. |
| `CONFIG_ACPI_AC=y` | AC adapter status | YES | Keep. |
| `CONFIG_ACPI_BATTERY=y` | Battery status | YES | Keep. |
| `CONFIG_ACPI_THERMAL=y` | Thermal zones | YES | Keep. |
| `CONFIG_ACPI_BUTTON=y` | Power/sleep/lid buttons | MISSING | **ADD.** Without this, lid close/open events are lost. Power button events may not work. |
| `CONFIG_ACPI_VIDEO=y` | ACPI video extensions | MISSING | **ADD.** Required for backlight control on most laptops. |
| `CONFIG_ACPI_FAN=m` | Fan control | MISSING | **ADD.** Allows fan speed reporting/control via ACPI. |
| `CONFIG_ACPI_DOCK=m` | Docking station | MISSING | **ADD.** ThinkPad docks, HP docks. |
| `CONFIG_ACPI_PROCESSOR=y` | Processor ACPI | auto | Verify enabled. Required for CPU frequency scaling. |
| `CONFIG_ACPI_PROCESSOR_AGGREGATOR=m` | Processor aggregator | MISSING | ADD. Logical processor idling. |
| `CONFIG_ACPI_CONTAINER=y` | Container devices | auto | For hotplug. |
| `CONFIG_ACPI_SBS=m` | Smart Battery System | MISSING | ADD. Some laptops use SBS protocol for battery. |
| `CONFIG_ACPI_HED=m` | Hardware Error Device | MISSING | ADD. Platform error reporting. |
| `CONFIG_ACPI_FPDT=y` | Firmware Performance Data | MISSING | ADD. Boot time diagnostics. |

### ACPI WMI (Important for Laptop Fn Keys)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_ACPI_WMI=m` | ACPI-WMI core | **IMPORTANT.** Many laptop vendor features (brightness hotkeys, fan modes, keyboard backlight) are exposed via WMI. |
| `CONFIG_WMI_BMOF=m` | WMI Binary MOF | Parsing WMI object data. |

### Laptop Vendor ACPI Drivers (Module — load on detection)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_THINKPAD_ACPI=m` | Lenovo ThinkPad | ThinkPad hotkeys, LED control, fan control, battery thresholds. Very common in office environments. |
| `CONFIG_DELL_WMI=m` | Dell WMI | Dell hotkeys, backlight, battery. |
| `CONFIG_DELL_LAPTOP=m` | Dell laptop platform | BIOS settings, backlight. |
| `CONFIG_DELL_SMBIOS=m` | Dell SMBIOS interface | Underlies Dell laptop/WMI drivers. |
| `CONFIG_DELL_RBTN=m` | Dell radio button | WiFi kill switch on Dell laptops. |
| `CONFIG_HP_WMI=m` | HP WMI | HP hotkeys, backlight. |
| `CONFIG_ASUS_WMI=m` | ASUS WMI | ASUS hotkeys, backlight, fan curves. |
| `CONFIG_ASUS_LAPTOP=m` | ASUS laptop (legacy) | Older ASUS/Medion laptops. |
| `CONFIG_ASUS_NB_WMI=m` | ASUS notebook WMI | Modern ASUS notebooks. |
| `CONFIG_ACER_WMI=m` | Acer WMI | Acer hotkeys, wireless toggle. |
| `CONFIG_TOSHIBA_ACPI=m` | Toshiba ACPI | Toshiba/Dynabook laptops. |
| `CONFIG_SAMSUNG_LAPTOP=m` | Samsung laptop | Samsung-specific features. |
| `CONFIG_MSI_LAPTOP=m` | MSI laptop | MSI notebook features. |
| `CONFIG_PANASONIC_LAPTOP=m` | Panasonic Toughbook | Brightness, hotkeys. |
| `CONFIG_SONY_LAPTOP=m` | Sony VAIO | Legacy Sony features. |

### ACPI DSDT Overrides

**NOT NEEDED for Llamaste.** DSDT override (`CONFIG_ACPI_CUSTOM_DSDT` / initrd table upgrade) is for working around specific buggy BIOS tables. It requires per-machine custom tables. An appliance OS should work with vendor DSDT as-is. If a specific machine has a broken DSDT, the better approach is kernel boot parameters (`acpi_osi=`, `acpi_enforce_resources=lax`).

### Suspend / Hibernate

| Config Option | Recommendation | Rationale |
|---|---|---|
| `CONFIG_SUSPEND=y` | **YES** | S3 sleep. Laptops close lid → suspend. Essential for laptop UX. |
| `CONFIG_HIBERNATION=n` | **NO** | S4 hibernate requires swap partition. Llamaste has no swap. Not needed for appliance. |
| `CONFIG_PM=y` | **YES** | Core power management. Required by ACPI, USB, PCI power states. |
| `CONFIG_PM_SLEEP=y` | **YES** | Sleep framework. Auto-selected by SUSPEND. |

**Appliance consideration**: Even though Llamaste is an appliance, users will close laptop lids. Without suspend support, the system keeps running with screen off (wastes battery) or the laptop overheats in a bag. S3 suspend is the right behavior for lid close.

---

## 4. Touchpad / Trackpad Support

### PS/2 Touchpads (MUST HAVE — covers ~60% of laptops)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_INPUT=y` | Input subsystem | Already present. |
| `CONFIG_INPUT_MOUSEDEV=y` | /dev/input/mice | Already present. |
| `CONFIG_INPUT_EVDEV=y` | Event device | Already present. |
| `CONFIG_MOUSE_PS2=y` | PS/2 mouse | **ADD.** Core PS/2 mouse driver. |
| `CONFIG_MOUSE_PS2_ALPS=y` | ALPS touchpad | **ADD.** Very common in Dell, Lenovo, HP. |
| `CONFIG_MOUSE_PS2_BYD=y` | BYD touchpad | ADD. Some budget laptops. |
| `CONFIG_MOUSE_PS2_CYPRESS=y` | Cypress touchpad | ADD. Some HP models. |
| `CONFIG_MOUSE_PS2_ELANTECH=y` | Elantech PS/2 | **ADD.** Common in ASUS, Acer, Lenovo. |
| `CONFIG_MOUSE_PS2_FOCALTECH=y` | FocalTech touchpad | ADD. Some Lenovo IdeaPad. |
| `CONFIG_MOUSE_PS2_LOGIPS2PP=y` | Logitech PS/2++ | ADD. External PS/2 mice. |
| `CONFIG_MOUSE_PS2_SENTELIC=y` | Sentelic touchpad | ADD. Some ASUS models. |
| `CONFIG_MOUSE_PS2_SYNAPTICS=y` | Synaptics PS/2 | **ADD.** Most common touchpad vendor. |
| `CONFIG_MOUSE_PS2_SYNAPTICS_SMBUS=y` | Synaptics SMBus | **ADD.** Modern Synaptics use SMBus transport for better performance. |
| `CONFIG_MOUSE_PS2_TRACKPOINT=y` | TrackPoint | **ADD.** ThinkPad red nub. |
| `CONFIG_MOUSE_PS2_TOUCHKIT=y` | eGalax touchscreen | ADD. PS/2 touchscreens. |

### I2C Touchpads (MUST HAVE — covers ~40% of modern laptops)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_I2C_HID=m` | I2C HID core | Umbrella option. |
| `CONFIG_I2C_HID_ACPI=m` | I2C HID ACPI transport | **CRITICAL.** All modern laptop touchpads and touchscreens connected via I2C. Without this + I2C + LPSS, touchpad is dead on 2016+ laptops. |
| `CONFIG_I2C_HID_OF=m` | I2C HID Device Tree | Not needed on x86. |
| `CONFIG_MOUSE_ELAN_I2C=m` | Elan I2C touchpad | **ADD.** Dedicated Elan I2C driver, used by some ASUS and Acer. |
| `CONFIG_MOUSE_ELAN_I2C_I2C=y` | Elan I2C transport | Sub-option of above. |
| `CONFIG_MOUSE_ELAN_I2C_SMBUS=y` | Elan SMBus transport | Sub-option of above. |

### HID Support (MUST HAVE)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_HID=y` | HID core | **ADD.** Required for USB keyboards, mice, touchpads. Currently missing — may work via legacy USB HID but fragile. |
| `CONFIG_HID_GENERIC=y` | Generic HID driver | **ADD.** Catches devices without specific drivers. |
| `CONFIG_HID_MULTITOUCH=m` | Multitouch HID | **ADD.** Windows Precision Touchpads (common on 2017+ laptops). Also USB touchscreens. |
| `CONFIG_USB_HID=y` | USB HID | **ADD.** USB keyboards and mice. |
| `CONFIG_HID_BATTERY_STRENGTH=y` | HID battery reporting | Wireless mouse/keyboard battery level. |

### Synaptics RMI4 (Important for modern Synaptics)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_RMI4_CORE=m` | RMI4 core | Modern Synaptics touchpad protocol. |
| `CONFIG_RMI4_I2C=m` | RMI4 over I2C | I2C transport for RMI4. |
| `CONFIG_RMI4_SMB=m` | RMI4 over SMBus | SMBus transport for RMI4. |
| `CONFIG_RMI4_F03=y` | RMI4 PS/2 guest | PS/2 passthrough (touchpad + TrackPoint). |
| `CONFIG_RMI4_F11=y` | RMI4 2D pointing | Basic pointing. |
| `CONFIG_RMI4_F12=y` | RMI4 2D pointing v2 | Newer protocol. |

### libinput Configuration

Llamaste uses labwc (wlroots-based compositor) which uses libinput. Default libinput behavior for touchpads:
- Tap-to-click: disabled by default (enable via `libinput_device_config_tap_set_enabled`)
- Natural scrolling: disabled by default
- Click method: depends on hardware capabilities

For an appliance OS, sensible defaults would be:
- **Enable tap-to-click** (most users expect it)
- **Enable natural scrolling** (matches phone/tablet muscle memory)
- Configure via `/etc/libinput/local-overrides.quirks` or wlroots config

---

## 5. Laptop-Specific Hardware

### Battery Monitoring

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_POWER_SUPPLY=y` | Power supply framework | **ADD.** Core framework for battery/AC reporting. Required by ACPI battery driver. |
| `CONFIG_ACPI_BATTERY=y` | ACPI battery | Already present. Needs POWER_SUPPLY. |
| `CONFIG_ACPI_AC=y` | AC adapter | Already present. |
| `CONFIG_BATTERY_BQ27XXX=m` | TI BQ27xxx gauge | Some embedded/tablet-style devices. Optional. |

### Backlight Control

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_BACKLIGHT_CLASS_DEVICE=y` | Backlight class | **ADD.** Core backlight framework. Without this, no /sys/class/backlight/. |
| `CONFIG_ACPI_VIDEO=y` | ACPI video driver | **ADD.** Creates ACPI backlight interface. Works on most laptops. |
| `CONFIG_DRM_I915_BACKLIGHT=y` | Intel GPU backlight | Auto-enabled with i915. Native backlight for Intel GPUs. |
| `CONFIG_BACKLIGHT_GENERIC=m` | Generic backlight | Fallback. |
| `CONFIG_LCD_CLASS_DEVICE=m` | LCD class | Some older laptops. |

**Backlight priority** (kernel handles this automatically since 6.1):
1. Native (GPU driver backlight) — preferred
2. ACPI video — fallback
3. Vendor-specific — last resort

### Webcam (USB Video Class)

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_MEDIA_SUPPORT=m` | Media subsystem | **ADD.** Required for any video capture. Currently explicitly disabled! |
| `CONFIG_MEDIA_CAMERA_SUPPORT=y` | Camera support | ADD. Sub-option under media. |
| `CONFIG_VIDEO_DEV=m` | V4L2 core | ADD. Video for Linux 2 framework. |
| `CONFIG_USB_VIDEO_CLASS=m` | UVC driver | **ADD.** Covers 99% of USB webcams (built-in and external). |
| `CONFIG_USB_VIDEO_CLASS_INPUT_EVDEV=y` | UVC button events | Webcam button → input event. |

**Note**: Webcam support adds ~200KB of modules but enables a major laptop feature. Worth including.

### SD Card Readers

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_MMC=m` | MMC/SD core | **ADD.** Core framework for all SD/MMC card readers. |
| `CONFIG_MMC_BLOCK=m` | MMC block device | ADD. Makes cards appear as /dev/mmcblkN. |
| `CONFIG_MMC_SDHCI=m` | SDHCI core | **ADD.** Standard SD Host Controller Interface. Most built-in readers. |
| `CONFIG_MMC_SDHCI_PCI=m` | SDHCI PCI | **ADD.** PCI-connected SD readers (most desktops/laptops). |
| `CONFIG_MMC_SDHCI_ACPI=m` | SDHCI ACPI | **ADD.** ACPI-enumerated SD readers (Intel Bay Trail, etc). |
| `CONFIG_MMC_REALTEK_PCI=m` | Realtek PCI-E SD | **ADD.** Very common in laptops (RTS5209, RTS5229, etc). |
| `CONFIG_MMC_REALTEK_USB=m` | Realtek USB SD | ADD. USB-connected Realtek readers. |
| `CONFIG_MISC_RTSX_PCI=m` | Realtek PCIe card reader | **ADD.** Base driver for Realtek PCIe card readers. Many laptops. |
| `CONFIG_MISC_RTSX_USB=m` | Realtek USB card reader | ADD. USB variant. |

**Note**: Realtek PCIe card readers (rtsx_pci) are by far the most common in laptops. The ASUS VivoBook test machine likely has one.

### Thunderbolt / USB4

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_USB4=m` | USB4/Thunderbolt | ADD as module. Needed for Thunderbolt 3/4 and USB4 docks. |
| `CONFIG_USB4_KUNIT_TEST=n` | Test suite | Not needed. |
| `CONFIG_HOTPLUG_PCI_PCIE=y` | PCIe native hotplug | **ADD.** Required for Thunderbolt PCIe tunneling. |

---

## 6. IOMMU / UEFI

### IOMMU

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_IOMMU_SUPPORT=y` | IOMMU framework | **ADD.** Core IOMMU support. |
| `CONFIG_INTEL_IOMMU=y` | Intel VT-d | **ADD.** Present on most Intel systems since 2010. Some BIOS enable it by default — without kernel support, DMA may fail on certain hardware (particularly with >4GB RAM). |
| `CONFIG_INTEL_IOMMU_DEFAULT_ON=n` | IOMMU on by default | **NO.** Let BIOS/ACPI decide. Use `intel_iommu=on` boot param if needed. Default off avoids performance overhead. |
| `CONFIG_INTEL_IOMMU_SVM=y` | Shared Virtual Memory | For GPU compute. Auto-selected. |
| `CONFIG_AMD_IOMMU=y` | AMD IOMMU (AMD-Vi) | **ADD.** Present on all AMD systems since Bulldozer (2011). Same rationale as Intel. |
| `CONFIG_AMD_IOMMU_V2=m` | AMD IOMMU v2 | Pasid/PRI support for AMD GPUs. |
| `CONFIG_IOMMU_DEFAULT_PASSTHROUGH=y` | Default passthrough mode | **YES.** `iommu=pt` by default. Better performance — only enables IOMMU for devices that need it. Standard for desktop Linux. |

**Why IOMMU matters for Llamaste**: Some BIOSes enable VT-d/AMD-Vi by default. Without kernel IOMMU support, DMA remapping may cause devices to fail (especially NVMe, GPU, USB controllers). Having IOMMU support compiled in but in passthrough mode (no performance impact) prevents mysterious device failures.

### UEFI Secure Boot

**NOT NEEDED for v1.0.** Secure Boot requires:
1. A signed bootloader (shim + GRUB signed by Microsoft or own MOK)
2. A signed kernel
3. Lockdown mode (restricts kernel features)

This is a significant engineering effort. For now, users must disable Secure Boot in BIOS. This is acceptable for an appliance OS that users intentionally install.

**Future consideration**: Sign with own MOK key + document how to enroll it. Avoids Microsoft signing dependency.

### TPM

| Config Option | Recommendation | Rationale |
|---|---|---|
| `CONFIG_TCG_TPM=m` | Optional | TPM hardware is present on most 2016+ systems. Not needed for core Llamaste function. Could be useful for future disk encryption or attestation features. Low cost to include as module. |
| `CONFIG_TCG_TIS=m` | Optional | TPM Interface Specification (LPC/memory-mapped). |
| `CONFIG_TCG_CRB=m` | Optional | Command Response Buffer (modern TPM 2.0 interface). |

---

## 7. PCI Express / Hotplug

| Config Option | Purpose | Current | Recommendation |
|---|---|---|---|
| `CONFIG_PCI=y` | PCI support | YES | Keep. |
| `CONFIG_PCI_MMCONFIG=y` | MMCONFIG access | MISSING | **ADD.** Enhanced PCI config space access. Required for PCIe. |
| `CONFIG_PCIEPORTBUS=y` | PCIe Port Bus | MISSING | **ADD.** Core PCIe infrastructure. |
| `CONFIG_HOTPLUG_PCI=y` | PCI hotplug core | MISSING | **ADD.** Required for Thunderbolt, ExpressCard, docks. |
| `CONFIG_HOTPLUG_PCI_PCIE=y` | PCIe native hotplug | MISSING | **ADD.** Thunderbolt PCIe tunneling. |
| `CONFIG_HOTPLUG_PCI_ACPI=y` | ACPI PCI hotplug | MISSING | **ADD.** Standard ACPI-based hotplug (most systems). |
| `CONFIG_HOTPLUG_PCI_SHPC=m` | SHPC hotplug | MISSING | ADD. Server standard hotplug controller. |
| `CONFIG_PCIEASPM=y` | PCIe ASPM | MISSING | **ADD.** Active State Power Management. Significant power savings on laptops. |
| `CONFIG_PCIEASPM_DEFAULT=y` | BIOS default ASPM | MISSING | ADD. Use BIOS ASPM settings (safest default). |
| `CONFIG_PCIEAER=y` | PCIe AER | MISSING | ADD. Advanced Error Reporting. Better error handling. |
| `CONFIG_PCI_MSI=y` | Message Signaled Interrupts | likely auto | Verify. Required for modern device drivers. |

---

## 8. CPU Feature Support

### CPU Frequency Scaling

| Config Option | Current | Recommendation |
|---|---|---|
| `CONFIG_CPU_FREQ=y` | YES | Keep. |
| `CONFIG_CPU_FREQ_DEFAULT_GOV_PERFORMANCE=y` | YES | **CHANGE to schedutil** for laptops. Performance governor wastes power. |
| `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y` | MISSING | **ADD.** Best general-purpose governor. Scheduler-integrated. |
| `CONFIG_CPU_FREQ_GOV_PERFORMANCE=y` | YES | Keep as option. |
| `CONFIG_CPU_FREQ_GOV_POWERSAVE=y` | YES | Keep. |
| `CONFIG_CPU_FREQ_GOV_ONDEMAND=y` | YES | Keep as fallback. |
| `CONFIG_CPU_FREQ_GOV_CONSERVATIVE=y` | MISSING | ADD. Gradual frequency changes. |
| `CONFIG_X86_INTEL_PSTATE=y` | MISSING | **ADD.** Modern Intel CPU frequency driver. Replaces acpi-cpufreq on Haswell+. |
| `CONFIG_X86_AMD_PSTATE=m` | MISSING | **ADD.** AMD CPPC frequency driver for Zen 2+. Significantly better than acpi-cpufreq. |
| `CONFIG_X86_AMD_PSTATE_DEFAULT_MODE=2` | N/A | Guided mode (2) is best default. Active (3) can cause issues. |
| `CONFIG_X86_ACPI_CPUFREQ=m` | MISSING | **ADD.** Legacy ACPI frequency scaling. Fallback for older Intel/AMD. |
| `CONFIG_X86_SPEEDSTEP_CENTRINO=m` | MISSING | ADD. Very old Intel laptops (pre-Core). |

**Recommended default governor**: `schedutil` balances performance and power. Llamaste does LLM inference (CPU-intensive) but also has idle periods. schedutil ramps up fast under load and drops when idle. For dedicated server mode, users can switch to `performance` via boot parameter.

### Microcode

| Config Option | Purpose | Recommendation |
|---|---|---|
| `CONFIG_MICROCODE=y` | CPU microcode loading | **ADD.** Security patches and bug fixes for CPUs. |
| `CONFIG_MICROCODE_INTEL=y` | Intel microcode | ADD. Loads from /lib/firmware/intel-ucode/. |
| `CONFIG_MICROCODE_AMD=y` | AMD microcode | ADD. Loads from /lib/firmware/amd-ucode/. |

**Note**: Microcode requires firmware files in initramfs or /lib/firmware/. Buildroot's `BR2_PACKAGE_LINUX_FIRMWARE_INTEL_CPU_MICROCODE` and `BR2_PACKAGE_LINUX_FIRMWARE_AMD_CPU_MICROCODE` packages provide these. Important for Spectre/Meltdown mitigations.

### Performance Monitoring

| Config Option | Recommendation | Notes |
|---|---|---|
| `CONFIG_PERF_EVENTS=y` | Optional | Useful for profiling LLM inference. ~10KB kernel overhead. |

### Thermal Throttling

Already have `CONFIG_THERMAL=y`. Add:

| Config Option | Purpose | Notes |
|---|---|---|
| `CONFIG_THERMAL_GOV_STEP_WISE=y` | Step-wise throttling | Default thermal governor. Gradually reduces performance. |
| `CONFIG_THERMAL_GOV_USER_SPACE=y` | Userspace thermal | Allow userspace thermal control. |
| `CONFIG_THERMAL_WRITABLE_TRIPS=y` | Writable trip points | Allow adjusting thermal thresholds. |
| `CONFIG_CPU_THERMAL=y` | CPU cooling device | Register CPU as a cooling device. |
| `CONFIG_X86_PKG_TEMP_THERMAL=m` | Package temperature | Per-package thermal monitoring for Intel CPUs. |

---

## 9. Real Hardware Test Matrix

### Priority Tier 1 — MUST Work (Most Common Target Hardware)

| Category | Example Hardware | Key Requirements |
|---|---|---|
| **Old Office PCs (2013-2018)** | Dell OptiPlex 3020-7060, HP ProDesk 400-600, Lenovo ThinkCentre M700-M920 | Intel 4th-8th gen, i2c-i801, AHCI SATA, Intel iGPU (i915), Realtek Ethernet, USB 3.0 |
| **Old Laptops (2015-2020)** | Lenovo ThinkPad T440-T480, Dell Latitude 5000 series, HP ProBook 440-450 | Intel 5th-8th gen, PS/2 or I2C touchpad, ACPI battery/backlight, Intel WiFi, SD reader |
| **Budget Mini PCs** | Intel NUC (6th-10th gen), Beelink SEi/SER, MinisForum | Intel/AMD SoC, NVMe, HDMI/DP, USB 3.0, often fanless |

### Priority Tier 2 — SHOULD Work

| Category | Example Hardware | Key Requirements |
|---|---|---|
| **Modern Gaming Desktops** | AMD Ryzen 5000-7000, Intel 12th-14th gen | AMD/Intel IOMMU, NVMe, PCIe 4.0/5.0, dedicated GPU (amdgpu), USB 3.1/3.2 |
| **Modern Laptops (2020+)** | ThinkPad T14/X1 Carbon, Dell XPS 13/15, HP EliteBook | Intel 11th-14th gen or AMD Ryzen 5000-7000, I2C touchpad (Precision), Thunderbolt, Wi-Fi 6/6E |
| **Budget Laptops** | ASUS VivoBook (test machine!), Acer Aspire, HP 15/17, Lenovo IdeaPad | Intel 10th-12th gen, Elan/Synaptics I2C touchpad, Realtek WiFi, Realtek SD reader |

### Priority Tier 3 — NICE TO HAVE

| Category | Example Hardware | Key Requirements |
|---|---|---|
| **Servers** | Dell PowerEdge R640/R740, HP ProLiant DL360/DL380, Supermicro | IPMI/BMC (out of scope), multiple NICs, hardware RAID, ECC RAM |
| **Thin Clients / Refurbished** | Dell Wyse 5070, HP t630/t640, Lenovo M75q Tiny | AMD/Intel embedded, eMMC or small SSD, low RAM (4-8GB), USB boot |
| **Chromebook-class** | Various (after disabling ChromeOS) | Limited RAM, eMMC storage, unusual audio/touchpad controllers |

### Hardware Testing Checklist

For each test machine, verify:
1. **Boot**: UEFI boot to GRUB → kernel → Llamaste binary
2. **Display**: Console output visible (simpledrm or native GPU)
3. **Keyboard**: PS/2 or USB keyboard functional
4. **Network**: Ethernet or WiFi connection established
5. **Storage**: System drive detected (SATA or NVMe)
6. **Audio**: ALSA device detected (speaker/headphone output)
7. **Touchpad**: (laptops) Functional with tap/scroll
8. **Battery**: (laptops) Status reported correctly
9. **Backlight**: (laptops) Brightness adjustable
10. **Webcam**: (laptops) /dev/videoN present
11. **SD card**: (if present) Card detected on insert
12. **USB**: External devices (keyboard, mouse, flash drive) work
13. **Suspend**: (laptops) Lid close → suspend, lid open → resume
14. **Web UI**: Accessible from browser
15. **LLM inference**: Model loads and generates tokens

---

## 10. Implementation Priority

### Phase 1 — Critical (Do Now)
These are needed for Llamaste to work on most real hardware:

```
# I2C core (touchpads, sensors)
CONFIG_I2C=y
CONFIG_I2C_CHARDEV=m
CONFIG_I2C_I801=m
CONFIG_I2C_DESIGNWARE_CORE=m
CONFIG_I2C_DESIGNWARE_PLATFORM=m
CONFIG_I2C_DESIGNWARE_PCI=m
CONFIG_I2C_PIIX4=m

# Intel platform
CONFIG_X86_INTEL_LPSS=y
CONFIG_MFD_INTEL_LPSS_PCI=m
CONFIG_MFD_INTEL_LPSS_ACPI=m

# AMD platform
CONFIG_X86_AMD_PLATFORM_DEVICE=y

# Pinctrl (required for LPSS and AMD GPIO)
CONFIG_PINCTRL=y
CONFIG_PINCTRL_INTEL=m
CONFIG_PINCTRL_AMD=m
# Plus all Intel PCH pinctrl drivers as modules (see Section 1)

# HID (keyboards, mice, touchpads)
CONFIG_HID=y
CONFIG_HID_GENERIC=y
CONFIG_USB_HID=y
CONFIG_HID_MULTITOUCH=m
CONFIG_I2C_HID_ACPI=m

# PS/2 touchpads
CONFIG_MOUSE_PS2=y
CONFIG_MOUSE_PS2_ALPS=y
CONFIG_MOUSE_PS2_ELANTECH=y
CONFIG_MOUSE_PS2_SYNAPTICS=y
CONFIG_MOUSE_PS2_SYNAPTICS_SMBUS=y
CONFIG_MOUSE_PS2_TRACKPOINT=y
CONFIG_MOUSE_ELAN_I2C=m

# ACPI essentials
CONFIG_ACPI_BUTTON=y
CONFIG_ACPI_VIDEO=y
CONFIG_ACPI_FAN=m

# Power supply / backlight
CONFIG_POWER_SUPPLY=y
CONFIG_BACKLIGHT_CLASS_DEVICE=y

# IOMMU (prevents boot failures)
CONFIG_IOMMU_SUPPORT=y
CONFIG_INTEL_IOMMU=y
CONFIG_AMD_IOMMU=y
CONFIG_IOMMU_DEFAULT_PASSTHROUGH=y

# PCIe infrastructure
CONFIG_PCI_MMCONFIG=y
CONFIG_PCIEPORTBUS=y
CONFIG_PCIEASPM=y
CONFIG_HOTPLUG_PCI=y
CONFIG_HOTPLUG_PCI_PCIE=y
CONFIG_HOTPLUG_PCI_ACPI=y

# CPU frequency (power management)
CONFIG_X86_INTEL_PSTATE=y
CONFIG_X86_ACPI_CPUFREQ=m
CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y
```

### Phase 2 — Important (Next Sprint)
```
# SD card readers
CONFIG_MMC=m
CONFIG_MMC_SDHCI=m
CONFIG_MMC_SDHCI_PCI=m
CONFIG_MMC_SDHCI_ACPI=m
CONFIG_MISC_RTSX_PCI=m
CONFIG_MMC_REALTEK_PCI=m

# Webcam
CONFIG_MEDIA_SUPPORT=m
CONFIG_USB_VIDEO_CLASS=m

# Laptop vendor drivers
CONFIG_THINKPAD_ACPI=m
CONFIG_DELL_WMI=m
CONFIG_DELL_LAPTOP=m
CONFIG_HP_WMI=m
CONFIG_ASUS_WMI=m
CONFIG_ASUS_NB_WMI=m
CONFIG_ACER_WMI=m
CONFIG_ACPI_WMI=m

# Thunderbolt
CONFIG_USB4=m

# Thermal
CONFIG_INTEL_PCH_THERMAL=m
CONFIG_INTEL_RAPL=m
CONFIG_X86_PKG_TEMP_THERMAL=m
CONFIG_K10TEMP=m
CONFIG_AMD_PMC=m

# Suspend
CONFIG_SUSPEND=y

# Synaptics RMI4
CONFIG_RMI4_CORE=m
CONFIG_RMI4_I2C=m
CONFIG_RMI4_SMB=m

# Microcode
CONFIG_MICROCODE=y
CONFIG_MICROCODE_INTEL=y
CONFIG_MICROCODE_AMD=y

# AMD frequency scaling
CONFIG_X86_AMD_PSTATE=m
```

### Phase 3 — Nice-to-Have (Future)
```
# TPM
CONFIG_TCG_TPM=m
CONFIG_TCG_TIS=m
CONFIG_TCG_CRB=m

# Intel MEI
CONFIG_INTEL_MEI=m
CONFIG_INTEL_MEI_ME=m

# Intel DPTF
CONFIG_ACPI_DPTF=m
CONFIG_INT340X_THERMAL=m

# More laptop vendors
CONFIG_TOSHIBA_ACPI=m
CONFIG_SAMSUNG_LAPTOP=m
CONFIG_MSI_LAPTOP=m
CONFIG_SONY_LAPTOP=m
CONFIG_PANASONIC_LAPTOP=m
```

---

## 11. Kernel Size Impact Estimate

| Phase | Approximate Size Impact | Notes |
|---|---|---|
| Phase 1 | +150-200KB (built-in) + ~500KB modules | I2C, HID, pinctrl core are small. Pinctrl platform drivers as modules. |
| Phase 2 | +800KB-1MB modules | Media subsystem is the biggest chunk (~400KB). Laptop vendor drivers ~200KB total. |
| Phase 3 | +200KB modules | Minimal impact. |

Total: ~1.5-2MB additional modules. Negligible for a system that already ships a 3B+ parameter model.

---

## 12. References

- [ODI Kernel Config Guide](https://www.odi.ch/prog/kernel-config.php) — Comprehensive x86_64 kernel config recommendations
- [ArchWiki ACPI Modules](https://wiki.archlinux.org/title/ACPI_modules) — ACPI module overview
- [ArchWiki CPU Frequency Scaling](https://wiki.archlinux.org/title/CPU_frequency_scaling) — CPU frequency scaling drivers and governors
- [Kernel i2c-i801 docs](https://www.kernel.org/doc/html/latest/i2c/busses/i2c-i801.html) — Intel SMBus driver coverage
- [Kernel i2c-piix4 docs](https://docs.kernel.org/i2c/busses/i2c-piix4.html) — AMD SMBus driver
- [Kernel IOMMU docs](https://docs.kernel.org/arch/x86/iommu.html) — x86 IOMMU support
- [Kernel Thunderbolt/USB4 docs](https://docs.kernel.org/admin-guide/thunderbolt.html) — USB4/Thunderbolt configuration
- [CONFIG_PINCTRL_AMD](https://cateee.net/lkddb/web-lkddb/PINCTRL_AMD.html) — AMD GPIO pin control
- [CONFIG_X86_AMD_PLATFORM_DEVICE](https://cateee.net/lkddb/web-lkddb/X86_AMD_PLATFORM_DEVICE.html) — AMD ACPI platform devices
- [CONFIG_I2C_HID_ACPI](https://cateee.net/lkddb/web-lkddb/I2C_HID_ACPI.html) — HID over I2C ACPI driver
