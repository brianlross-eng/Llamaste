# Ethernet & WiFi Driver Coverage Research for Llamaste

**Date**: 2026-03-23
**Kernel**: Linux 6.6.70 (LTS)
**Goal**: Achieve ~99% x86_64 hardware coverage for wired and wireless networking

---

## Table of Contents

1. [Current Driver Inventory](#1-current-driver-inventory)
2. [Missing Ethernet Drivers](#2-missing-ethernet-drivers)
3. [Missing WiFi Drivers](#3-missing-wifi-drivers)
4. [Market Share Analysis](#4-market-share-analysis)
5. [USB Dongle Compatibility](#5-usb-dongle-compatibility)
6. [Kernel Version Considerations](#6-kernel-version-considerations)
7. [Firmware Requirements](#7-firmware-requirements)
8. [Kernel Config Impact](#8-kernel-config-impact)
9. [Recommendations](#9-recommendations)
10. [Implementation Plan](#10-implementation-plan)

---

## 1. Current Driver Inventory

### Currently Enabled Ethernet Drivers

| Driver | CONFIG | Type | Coverage |
|--------|--------|------|----------|
| e1000 | CONFIG_E1000=y | Built-in | Intel PRO/1000 (legacy PCI) |
| e1000e | CONFIG_E1000E=y | Built-in | Intel PRO/1000 (PCIe, most Intel desktops/laptops) |
| igb | CONFIG_IGB=y | Built-in | Intel I210/I211/I350 (server/desktop) |
| igc | CONFIG_IGC=y | Built-in | Intel I225/I226 2.5GbE (newer desktops) |
| ixgbe | CONFIG_IXGBE=y | Built-in | Intel 10GbE (server) |
| r8169 | CONFIG_R8169=y | Built-in | Realtek 8111/8168/8169 1GbE (VERY common in desktops) |
| r8152 | CONFIG_USB_RTL8152=y | Built-in | Realtek USB Ethernet (RTL8152/8153/8153B) |
| virtio_net | CONFIG_VIRTIO_NET=y | Built-in | Virtual machines |
| ax88179 | CONFIG_USB_NET_AX88179_178A=y | Built-in | ASIX USB 3.0 Ethernet |
| ax8817x | CONFIG_USB_NET_AX8817X=y | Built-in | ASIX USB 2.0 Ethernet |
| cdc_ether | CONFIG_USB_NET_CDC_ETHER=y | Built-in | USB CDC Ethernet (generic) |
| cdc_ncm | CONFIG_USB_NET_CDC_NCM=y | Built-in | USB NCM (modern USB-C docks) |
| cdc_mbim | CONFIG_USB_NET_CDC_MBIM=y | Built-in | Mobile broadband |
| rndis_host | CONFIG_USB_NET_RNDIS_HOST=y | Built-in | RNDIS USB Ethernet (Android tethering) |
| smsc75xx | CONFIG_USB_NET_SMSC75XX=y | Built-in | SMSC USB Ethernet |
| smsc95xx | CONFIG_USB_NET_SMSC95XX=y | Built-in | SMSC USB Ethernet |
| aqc111 | CONFIG_USB_NET_AQC111=y | Built-in | Aquantia USB 5GbE |

### Currently Enabled WiFi Drivers

| Driver | CONFIG | Type | Coverage |
|--------|--------|------|----------|
| iwlwifi/iwlmvm | =m | Module | Intel WiFi 4/5/6/6E/7 (AX200/AX201/AX210/AX211/BE200) |
| rtw88 | =m | Module | Realtek WiFi 5 PCIe+USB (8821CE, 8822BE/BU, 8822CE/CU, 8812BU) |
| rtw89 | =m | Module | Realtek WiFi 6 PCIe (8852AE, 8852BE, 8852CE) |
| ath9k | =m | Module | Atheros WiFi 4 (legacy, still common in old hardware) |
| ath10k | =m | Module | Qualcomm Atheros WiFi 5 (QCA6174, QCA9377, QCA9984) |
| ath11k | =m | Module | Qualcomm WiFi 6/6E (QCA6390, WCN6855) |
| mt76 | =m | Module | MediaTek WiFi 5/6 (MT7601U, MT7610U, MT7612U, MT7921E) |
| brcmfmac | =m | Module | Broadcom FullMAC WiFi (BCM43xx SDIO/USB/PCIe) |
| rt2800usb | =m | Module | Ralink/MediaTek WiFi 4 USB (RT3070, RT3572, RT5370, RT5572) |

---

## 2. Missing Ethernet Drivers

### Priority 1: HIGH IMPACT (common in real-world hardware)

#### Broadcom tg3 — HP/Dell/Lenovo Desktops & Servers
- **CONFIG_TIGON3=m** (or =y)
- **Devices**: BCM5700/5701/5702/5703/5704/5705/5750/5751/5752/5755/5756/5782/5787/5788/5789
- **Where found**: HP ProLiant, HP EliteDesk, Dell OptiPlex/PowerEdge (pre-2015), Lenovo ThinkCentre
- **Coverage impact**: ~10-15% of enterprise/business desktops and most pre-2018 HP/Dell servers
- **Firmware**: None required (firmware embedded in hardware ROM)
- **Kernel 6.6**: Fully supported, mature driver

#### Broadcom bnx2 — Dell/HP Servers
- **CONFIG_BNX2=m**
- **Devices**: BCM5706/5708/5709/5716 NetXtreme II
- **Where found**: Dell PowerEdge G4-G6, HP ProLiant DL360/DL380 G4-G7
- **Coverage impact**: ~5% of server hardware still in use
- **Firmware**: Needs `bnx2/bnx2-mips-*.fw` (~200KB)
- **Kernel 6.6**: Fully supported

#### Broadcom bnx2x — 10GbE Servers
- **CONFIG_BNX2X=m**
- **Devices**: BCM57710/57711/57712/57800/57810/57840 NetXtreme II 10GbE
- **Where found**: Dell/HP 10GbE server NICs
- **Coverage impact**: ~3% of server hardware
- **Firmware**: Needs `bnx2x/bnx2x-e1*.fw`, `bnx2x-e2*.fw` (~1MB total)
- **Kernel 6.6**: Fully supported

#### Broadcom bnxt_en — Modern Broadcom Ethernet
- **CONFIG_BNXT=m**
- **Devices**: BCM573xx/574xx/575xx/576xx (NetXtreme-C/E)
- **Where found**: HPE ProLiant Gen10+, Dell PowerEdge 14th gen+, modern server/workstation NICs
- **Coverage impact**: ~5% of modern server hardware, growing
- **Firmware**: Needs `bnxt/bnxt_en*.fw` (~500KB)
- **Kernel 6.6**: Fully supported

#### Qualcomm/Atheros alx — Gaming Laptops & Budget Desktops
- **CONFIG_ALX=m**
- **Devices**: AR8161/AR8162/QCA8171/QCA8172, Killer E220x/E2400/E2500
- **Where found**: MSI gaming laptops, Gigabyte/ASRock gaming motherboards, Dell gaming laptops
- **Coverage impact**: ~5-8% of gaming laptops, ~3% of consumer desktops
- **Firmware**: None required
- **Kernel 6.6**: Fully supported since kernel 3.10

#### Qualcomm/Atheros atl1c — Older Atheros Ethernet
- **CONFIG_ATL1C=m**
- **Devices**: AR8131/AR8132/AR8151/AR8152
- **Where found**: Older laptops (2008-2014)
- **Coverage impact**: ~2% of legacy hardware
- **Firmware**: None required
- **Kernel 6.6**: Fully supported

### Priority 2: MODERATE IMPACT (niche but important)

#### Marvell/Aquantia atlantic — 2.5G/5G/10G NICs
- **CONFIG_AQTION=m** (driver name: atlantic)
- **Devices**: AQC-100/107/108/113/114/115
- **Where found**: High-end motherboards (ASUS ROG, MSI MEG), Thunderbolt docks, NAS devices
- **Coverage impact**: ~3-5% of enthusiast/high-end desktops
- **Firmware**: None required (hardware ROM)
- **Kernel 6.6**: Fully supported
- **Note**: Already have USB variant (CONFIG_USB_NET_AQC111=y), this adds PCIe

#### Marvell sky2/skge — Older Marvell Ethernet
- **CONFIG_SKY2=m**
- **Devices**: 88E8001/88E8021/88E8035/88E8036/88E8038/88E8050/88E8052/88E8053/88E8055/88E8061/88E8062
- **Where found**: Older ASUS, Gigabyte motherboards (2005-2012)
- **Coverage impact**: ~2% of legacy hardware
- **Firmware**: None required

#### Mellanox/NVIDIA mlx4_en — Server NICs
- **CONFIG_MLX4_EN=m** + **CONFIG_MLX4_CORE=m**
- **Devices**: ConnectX-2/ConnectX-3 (10/40GbE)
- **Where found**: Data center servers, high-performance workstations
- **Coverage impact**: ~2% of server/workstation hardware
- **Firmware**: Needs `mlx4/*.fw` (~500KB)
- **Kernel 6.6**: Fully supported

#### Mellanox/NVIDIA mlx5_core — Modern Server NICs
- **CONFIG_MLX5_CORE=m** + **CONFIG_MLX5_CORE_EN=y**
- **Devices**: ConnectX-4/5/6/7 (10/25/40/50/100GbE)
- **Where found**: Modern data centers, cloud servers
- **Coverage impact**: ~3% of server hardware, dominant in cloud
- **Firmware**: Needs `mellanox/*.fw` (~2MB)
- **Kernel 6.6**: Fully supported

### Priority 3: NICE TO HAVE

#### Intel i40e — Intel XL710/X710 10/40GbE
- **CONFIG_I40E=m**
- **Devices**: Intel Ethernet Controller X710/XL710/XXV710
- **Where found**: Server NICs, PCIe add-in cards
- **Coverage impact**: ~2% of server hardware
- **Firmware**: None required
- **Kernel 6.6**: Fully supported

#### Intel ice — Intel E810 100GbE
- **CONFIG_ICE=m**
- **Devices**: Intel Ethernet Controller E810
- **Coverage impact**: <1% (data center only)
- **Firmware**: Needs `intel/ice/*.pkg` (~2MB)

#### Realtek r8125 for 2.5GbE
- **STATUS**: The in-kernel `r8169` driver actually handles RTL8125/RTL8125B/RTL8126 2.5GbE chips since kernel 5.9+
- **No separate driver needed** — r8169 already covers these on kernel 6.6
- **Note**: Realtek provides an out-of-tree `r8125` driver with some optimizations, but r8169 works correctly
- **CONFIG_R8169=y already enabled** — RTL8125/8126 2.5GbE is already covered!

#### stmmac/dwmac — SoC Ethernet
- **CONFIG_STMMAC_ETH=m**
- **Devices**: Synopsys DesignWare MAC (found in some Intel SoCs, mini-PCs)
- **Coverage impact**: <1% of x86_64 hardware
- **Kernel 6.6**: Supported

---

## 3. Missing WiFi Drivers

### Priority 1: HIGH IMPACT

#### Qualcomm ath12k — WiFi 7
- **CONFIG_ATH12K=m** + **CONFIG_ATH12K_PCI=m**
- **Devices**: QCN9274, WCN7850, QCNCM865
- **Where found**: Framework laptops, some Lenovo/Dell 2024+ models
- **Coverage impact**: ~5% of 2024+ laptops, growing rapidly
- **Firmware**: `ath12k/WCN7850/hw2.0/*.bin` (~3MB)
- **Kernel 6.6 status**: ath12k was added in kernel 6.3. Basic support in 6.6, but stability improvements continued through 6.8-6.12. Uploading bug fixed in 6.11. **Usable on 6.6 but not perfect**
- **Recommendation**: Enable now, upgrade kernel later for stability

#### MediaTek MT7925 — WiFi 7
- **CONFIG_MT7925E=m** + **CONFIG_MT7925U=m** (via mt76 framework)
- **Devices**: MT7925 PCIe + USB
- **Where found**: New laptops from HP, Lenovo, ASUS (2024+)
- **Coverage impact**: ~3-5% of 2024+ laptops, growing
- **Firmware**: `mediatek/mt7925/*.bin` (~1MB)
- **Kernel 6.6 status**: MT7925 driver went into kernel 6.7. **NOT available on 6.6!**
- **Recommendation**: Requires kernel upgrade to 6.7+ or backport

#### RTL8xxxU — Cheap USB WiFi Dongles
- **CONFIG_RTL8XXXU=m** + sub-options
- **Devices**: RTL8188EU, RTL8192EU, RTL8723AU/BU, RTL8191EU, RTL8188RU
- **Where found**: $5-15 USB WiFi dongles (TP-Link TL-WN725N, TL-WN722N v2+, countless no-name dongles)
- **Coverage impact**: ~15-20% of all USB WiFi dongles sold globally
- **Firmware**: `rtlwifi/rtl8188eufw.bin`, `rtl8192eu*.bin` (~200KB total)
- **Kernel 6.6 status**: In-tree since kernel 4.3. Limited features (no 40MHz, basic mac80211). Works for basic connectivity
- **Recommendation**: Enable — covers huge number of cheap USB adapters

#### Realtek rtw88 USB expansions — RTL8812BU, RTL8821CU
- **STATUS**: Already have CONFIG_RTW88_8821CU=m and CONFIG_RTW88_8822BU=m
- **MISSING**: RTL8812BU support (CONFIG_RTW88_8812BU=m)
- **Kernel 6.6 status**: rtw88 USB support was added in kernel 6.2, but dramatically improved in 6.12+. On 6.6, USB support exists but may be buggy
- **Recommendation**: Already mostly covered, verify RTL8812BU config

### Priority 2: MODERATE IMPACT

#### Broadcom brcmsmac — Older Broadcom PCIe WiFi
- **CONFIG_BRCMSMAC=m**
- **Devices**: BCM4313, BCM43224, BCM43225 (SoftMAC, pre-FullMAC era)
- **Where found**: HP/Dell laptops 2010-2016
- **Coverage impact**: ~3% of older laptops
- **Firmware**: None (open-source SoftMAC driver)
- **Kernel 6.6**: Fully supported
- **Note**: Already have brcmfmac (FullMAC). brcmsmac covers the older SoftMAC chips

#### Broadcom b43 — Very Old Broadcom WiFi
- **CONFIG_B43=m**
- **Devices**: BCM4306/4311/4312/4318/4321/4322
- **Where found**: MacBooks 2006-2010, old HP/Dell laptops
- **Coverage impact**: ~1-2% of legacy hardware
- **Firmware**: Needs proprietary firmware extracted from Broadcom driver (~600KB)
- **Kernel 6.6**: Supported but maintenance mode
- **Recommendation**: Skip — very old hardware, firmware extraction complexity

#### Realtek rtw89 USB — WiFi 6 USB
- **STATUS**: Currently only have PCIe variants (8852AE, 8852BE, 8852CE)
- **MISSING**: USB support CONFIG_RTW89_USB=m + CONFIG_RTW89_8852AU=m etc.
- **Kernel 6.6 status**: rtw89 USB support was NOT in 6.6 — added later
- **Recommendation**: Cannot enable on 6.6, would need kernel upgrade

#### MediaTek MT7921U — WiFi 6E USB
- **CONFIG_MT7921U=m** (via mt76)
- **STATUS**: Already have MT7921E (PCIe), missing USB variant
- **Devices**: MT7921AU USB WiFi 6E adapters
- **Where found**: USB WiFi 6E dongles (recommended by Linux community)
- **Coverage impact**: ~2% of USB WiFi market, growing (recommended adapter)
- **Firmware**: `mediatek/WIFI_MT7961_patch_mcu_1_2_hdr.bin` + `WIFI_RAM_CODE_MT7961_1.bin` (~1MB)
- **Kernel 6.6 status**: USB support since kernel 5.18. **Available on 6.6!**
- **Recommendation**: Enable — best-supported USB WiFi 6E option

#### Ralink/MediaTek rt2800pci — PCIe Ralink WiFi
- **CONFIG_RT2800PCI=m** + **CONFIG_RT2X00_LIB_PCI=m**
- **STATUS**: Already have rt2800usb, missing PCIe variant
- **Devices**: RT3090, RT3290, RT3562, RT5390, RT5392
- **Where found**: Older HP/Lenovo laptops (2010-2015)
- **Coverage impact**: ~2% of legacy hardware
- **Firmware**: `rt2860.bin`, `rt3290.bin` (~20KB each)
- **Kernel 6.6**: Fully supported

### Priority 3: NICE TO HAVE (WiFi 7 future-proofing)

#### Intel BE200/BE202 WiFi 7
- **STATUS**: Already covered by iwlwifi/iwlmvm
- **Kernel 6.6 status**: BE200 PCI ID added in kernel 6.5. Works on 6.6 with correct firmware
- **Firmware**: Already have `iwlwifi-gl-c0-fm-c0-*.ucode` in overlay
- **No additional config needed** — iwlwifi already handles WiFi 7

#### Realtek RTL8812AU/RTL8814AU — Popular USB Dongles
- **CONFIG_RTW88_8812AU=m** (NEW in kernel 6.14 via rtw88 framework)
- **Devices**: RTL8812AU (Alfa AWUS036ACH), RTL8814AU (Alfa AWUS1900)
- **Where found**: Popular penetration testing / high-power USB adapters
- **Kernel 6.6 status**: **NOT available in 6.6** — in-kernel driver added in 6.14
- **Recommendation**: Cannot enable on 6.6. Out-of-tree driver available from lwfinger/rtw88 repo

---

## 4. Market Share Analysis

### WiFi Chip Market Share (2024-2025, by shipment volume)

| Manufacturer | Market Share | Primary Chips in Laptops | Linux Driver |
|-------------|-------------|------------------------|--------------|
| Realtek | ~25-30% | RTL8821CE, RTL8852BE, RTL8852CE | rtw88/rtw89 ✅ |
| MediaTek | ~20-25% | MT7921, MT7922, MT7925 | mt76 ✅ (except MT7925 needs 6.7+) |
| Intel | ~20-25% | AX201, AX211, BE200, BE202 | iwlwifi ✅ |
| Qualcomm | ~15-20% | WCN6855, WCN7850, QCNCM865 | ath11k/ath12k ✅ (ath12k needs enable) |
| Broadcom | ~5-10% | BCM4378, BCM4387, BCM4388 | brcmfmac ✅ |

**Current Llamaste WiFi coverage estimate: ~80-85%**
- Missing: ath12k (Qualcomm WiFi 7), MT7925 (MediaTek WiFi 7), RTL8xxxU (cheap USB)
- With ath12k + MT7925 + RTL8xxxU: **~95%**

### Ethernet Controller Market Share (2024-2025)

| Manufacturer | Market Share | Primary Chips | Linux Driver |
|-------------|-------------|---------------|--------------|
| Realtek | ~50-60% (consumer) | RTL8111/8168 (1G), RTL8125 (2.5G) | r8169 ✅ |
| Intel | ~20-25% (consumer), ~40% (server) | I219 (1G), I225/I226 (2.5G) | e1000e/igc ✅ |
| Broadcom | ~15-20% (server) | BCM5720/BCM57416 | tg3/bnxt_en ❌ MISSING |
| Qualcomm/Killer | ~5-8% (gaming) | QCA8171, Killer E2600 | alx ❌ MISSING |
| Marvell/Aquantia | ~3-5% (enthusiast) | AQC107/113 | atlantic ❌ MISSING |

**Current Llamaste Ethernet coverage estimate: ~75-80%**
- Missing: Broadcom (tg3/bnx2/bnxt_en), Qualcomm (alx), Marvell (atlantic/sky2)
- With Broadcom + Qualcomm + Marvell: **~97%**

### Combined Coverage Estimates

| Scenario | Ethernet | WiFi | Overall |
|----------|----------|------|---------|
| Current config | ~80% | ~85% | ~75% (both working) |
| + Priority 1 additions | ~95% | ~93% | ~90% |
| + Priority 2 additions | ~97% | ~95% | ~93% |
| + Kernel 6.12 upgrade | ~98% | ~98% | ~96% |
| Theoretical maximum (all drivers) | ~99% | ~99% | ~98% |

---

## 5. USB Dongle Compatibility

### Most Popular USB WiFi Dongles (2024-2025)

| Adapter | Chipset | Driver | In 6.6? | Status |
|---------|---------|--------|---------|--------|
| TP-Link TL-WN725N | RTL8188EU | rtl8xxxu | ✅ | MISSING CONFIG |
| TP-Link Archer T2U | RTL8821CU | rtw88 | ✅ | ✅ Covered |
| TP-Link Archer T3U | RTL8812BU | rtw88 | ✅ | ✅ Covered |
| Alfa AWUS036ACM | MT7612U | mt76 | ✅ | ✅ Covered |
| Alfa AWUS036ACHM | MT7610U | mt76 | ✅ | ✅ Covered |
| Netgear A6150 | RTL8812BU | rtw88 | ✅ | ✅ Covered |
| Panda PAU09 | RT5572 | rt2800usb | ✅ | ✅ Covered |
| Comfast CF-912AC | RTL8812AU | rtw88 | ❌ 6.14+ | Not on 6.6 |
| Alfa AWUS036ACH | RTL8812AU | rtw88 | ❌ 6.14+ | Not on 6.6 |
| EDUP EP-AX1672 | MT7921AU | mt76 | ✅ 5.18+ | MISSING CONFIG |
| BrosTrend AX1800 | MT7921AU | mt76 | ✅ 5.18+ | MISSING CONFIG |

### Most Popular USB-to-Ethernet Adapters

| Adapter | Chipset | Driver | Status |
|---------|---------|--------|--------|
| Anker USB-C Hub | RTL8153 | r8152 | ✅ Covered |
| Cable Matters USB-C | RTL8153B | r8152 | ✅ Covered |
| TP-Link UE300 | RTL8153 | r8152 | ✅ Covered |
| Apple USB-C Ethernet | ASIX AX88179A | ax88179_178a | ✅ Covered |
| Plugable USB 3.0 | ASIX AX88179 | ax88179_178a | ✅ Covered |
| Dell DA310 dock | RTL8153 | r8152 | ✅ Covered |
| Lenovo USB-C dock | RTL8153 | r8152 | ✅ Covered |
| USB-C 2.5G adapters | RTL8156 | r8152 (cdc_ncm) | ✅ Covered |
| Aquantia USB 5G | AQC111U | aqc111 | ✅ Covered |

**USB Ethernet: 100% coverage** — r8152, ax88179_178a, cdc_ether, and cdc_ncm cover virtually all USB Ethernet adapters.

---

## 6. Kernel Version Considerations

### Kernel 6.6 (current) vs 6.12 LTS — Driver Availability

| Feature/Driver | 6.6 | 6.7 | 6.8 | 6.12 | 6.14+ |
|---------------|-----|-----|-----|------|-------|
| ath12k (Qualcomm WiFi 7) | ✅ Basic | ✅ | ✅ Improved | ✅ Stable | ✅ |
| MT7925 (MediaTek WiFi 7) | ❌ | ✅ Added | ✅ | ✅ MLO support | ✅ |
| rtw88 USB improvements | ⚠️ Buggy | ⚠️ | ✅ | ✅ Dramatic improvement | ✅ |
| rtw89 USB support | ❌ | ❌ | ❌ | ⚠️ Early | ✅ |
| RTL8812AU in-kernel (rtw88) | ❌ | ❌ | ❌ | ❌ | ✅ 6.14 |
| RTL8852BU/8832BU in-kernel | ❌ | ❌ | ❌ | ❌ | ✅ 6.17 |
| RTL8852CU/8832CU in-kernel | ❌ | ❌ | ❌ | ❌ | ✅ 6.19 |
| All ethernet drivers | ✅ | ✅ | ✅ | ✅ | ✅ |

### Upgrade Recommendation

**Kernel 6.12 LTS is strongly recommended** for the following reasons:
1. **MT7925 WiFi 7** — Not available at all on 6.6 (added 6.7)
2. **rtw88 USB stability** — Dramatically improved during 2024, 6.12 is the minimum recommended version
3. **ath12k stability** — Upload bug fixed in 6.11, better WCN7850 support
4. **rtw89 USB** — Early support available
5. **General WiFi improvements** — MLO, better power management, scan improvements

**Kernel 6.12 LTS release date**: November 2024 (supported until ~2028)
**Buildroot 2024.02 support**: May need Buildroot upgrade to 2024.11+ for 6.12 kernel

### Out-of-Tree Drivers (avoid if possible)

| Driver | Reason Out-of-Tree | Alternative |
|--------|-------------------|-------------|
| Realtek r8125 | Realtek's vendor driver | Use r8169 (handles RTL8125 since 5.9) |
| RTL8812AU (aircrack) | Community driver | Use rtw88 backport from lwfinger/rtw88 |
| RTL8814AU | No in-kernel driver yet | No good option on 6.6 |
| Broadcom wl | Broadcom proprietary blob | Use brcmfmac (open source) |

---

## 7. Firmware Requirements

### Firmware Size Estimates for New Drivers

| Driver Family | Firmware Path | Estimated Size | Notes |
|--------------|---------------|----------------|-------|
| **Broadcom tg3** | None | 0 MB | Firmware in hardware ROM |
| **Broadcom bnx2** | `bnx2/bnx2-mips-*.fw` | ~0.5 MB | 4-5 firmware files |
| **Broadcom bnx2x** | `bnx2x/bnx2x-e1*.fw` | ~1 MB | 3 firmware files |
| **Broadcom bnxt_en** | `bnxt/bnxt_en-*.fw` | ~0.5 MB | 2-3 firmware files |
| **Qualcomm ath12k** | `ath12k/WCN7850/hw2.0/` | ~3 MB | board + amss + regdb |
| **MediaTek MT7925** | `mediatek/mt7925/` | ~1 MB | MCU + RAM code |
| **MediaTek MT7921U** | `mediatek/WIFI_MT7961*` | ~1 MB | Already needed for MT7921E |
| **Realtek rtl8xxxu** | `rtlwifi/rtl8188eufw.bin` etc. | ~0.5 MB | Small firmware blobs |
| **Ralink rt2800pci** | `rt2860.bin`, `rt3290.bin` | ~0.1 MB | Very small |
| **Qualcomm alx** | None | 0 MB | No firmware needed |
| **Marvell atlantic** | None | 0 MB | Firmware in hardware ROM |
| **Marvell sky2** | None | 0 MB | No firmware needed |
| **Mellanox mlx4** | `mlx4/*.fw` | ~0.5 MB | |
| **Mellanox mlx5** | `mellanox/*.fw` | ~2 MB | |
| **Broadcom brcmsmac** | None | 0 MB | Open-source SoftMAC |
| **TOTAL NEW FIRMWARE** | | **~10 MB** | |

### Current Firmware in Overlay
- iwlwifi (Intel WiFi): ~22 MB
- rtw88 (Realtek WiFi 5): ~140 KB
- rtl_nic (Realtek Ethernet): ~33 KB
- regulatory.db: ~6 KB
- **Current total: ~22 MB**

### With All New Firmware
- **Projected total: ~32 MB** (~10 MB increase)
- Buildroot's linux-firmware package can selectively install only needed firmware
- Alternatively, copy specific firmware files to overlay

---

## 8. Kernel Config Impact

### New CONFIG Options Needed

#### Ethernet (Priority 1 — add as modules)
```
# Broadcom Ethernet
CONFIG_NET_VENDOR_BROADCOM=y
CONFIG_TIGON3=m                    # tg3 — HP/Dell desktops/servers
CONFIG_BNX2=m                      # NetXtreme II 1GbE
CONFIG_BNX2X=m                     # NetXtreme II 10GbE
CONFIG_BNXT=m                      # NetXtreme-C/E modern

# Qualcomm/Atheros Ethernet
CONFIG_NET_VENDOR_ATHEROS=y
CONFIG_ALX=m                       # AR816x/QCA817x/Killer
CONFIG_ATL1C=m                     # AR8131/AR8151 (older)

# Marvell/Aquantia Ethernet
CONFIG_NET_VENDOR_AQUANTIA=y
CONFIG_AQTION=m                    # AQC107/113 2.5G/5G/10G
CONFIG_NET_VENDOR_MARVELL=y
CONFIG_SKY2=m                      # Marvell Yukon 2 (legacy)
```

#### WiFi (Priority 1 — add as modules)
```
# Qualcomm WiFi 7
CONFIG_ATH12K=m
CONFIG_ATH12K_PCI=m

# Realtek cheap USB dongles
CONFIG_RTL8XXXU=m
CONFIG_RTL8XXXU_UNTESTED=y        # Enables more device IDs

# MediaTek WiFi 6E USB
CONFIG_MT7921U=m                   # Already have MT7921E

# Ralink PCIe
CONFIG_RT2X00_LIB_PCI=m
CONFIG_RT2800PCI=m
```

#### WiFi (Priority 2 — if upgrading kernel)
```
# MediaTek WiFi 7 (requires kernel 6.7+)
CONFIG_MT7925E=m
CONFIG_MT7925U=m

# Broadcom SoftMAC (older)
CONFIG_BRCMSMAC=m
```

### Estimated bzImage/Module Size Impact

| Component | Size Increase |
|-----------|--------------|
| Ethernet modules (tg3, bnx2, bnx2x, bnxt, alx, atl1c, atlantic, sky2) | ~2 MB |
| WiFi modules (ath12k, rtl8xxxu, mt7921u, rt2800pci, brcmsmac) | ~3 MB |
| Firmware files | ~10 MB |
| **Total increase** | **~15 MB** |

Current squashfs is likely 200-300 MB. A 15 MB increase (~5-7%) is negligible.

### Built-in vs Module Strategy

**Keep as built-in (=y)**: Core boot-critical ethernet (e1000e, igc, r8169, r8152, igb) — needed before squashfs pivot
**Use modules (=m)**: Everything else — loaded by init_load_modules() after squashfs pivot

This is the current strategy and works well. New drivers should all be modules since they load after squashfs is available.

---

## 9. Recommendations

### Tier 1: Enable Now on Kernel 6.6 (biggest coverage gain, zero risk)

These drivers are all mature, stable, and fully supported on kernel 6.6:

1. **Broadcom tg3** — Covers HP/Dell desktop/server ethernet
2. **Broadcom bnx2/bnx2x** — Covers HP/Dell server ethernet
3. **Broadcom bnxt_en** — Covers modern Broadcom server ethernet
4. **Qualcomm alx** — Covers Killer/QCA gaming laptop/desktop ethernet
5. **Qualcomm atl1c** — Covers older Atheros ethernet
6. **Marvell atlantic** — Covers Aquantia multi-gig PCIe ethernet
7. **Marvell sky2** — Covers legacy Marvell ethernet
8. **Qualcomm ath12k** — WiFi 7 (basic support on 6.6)
9. **Realtek rtl8xxxu** — Cheap USB WiFi dongles (huge coverage)
10. **MediaTek MT7921U** — WiFi 6E USB dongles
11. **Ralink rt2800pci** — PCIe Ralink WiFi (legacy)
12. **Broadcom brcmsmac** — Older Broadcom SoftMAC WiFi

**Expected coverage gain**: Ethernet 80% → 97%, WiFi 85% → 93%

### Tier 2: Upgrade to Kernel 6.12 LTS

1. **MediaTek MT7925** — WiFi 7 in new laptops (not in 6.6)
2. **rtw88 USB stability** — Major improvements in 6.12
3. **ath12k stability** — Bug fixes through 6.8-6.12
4. **rtw89 USB** — WiFi 6 USB support

**Expected coverage gain**: WiFi 93% → 98%

### Tier 3: Future (kernel 6.14+)

1. RTL8812AU/RTL8821AU in-kernel driver (6.14)
2. RTL8852BU/RTL8832BU WiFi 6 USB (6.17)
3. RTL8852CU/RTL8832CU WiFi 6E USB (6.19)

### Server/Enterprise Priority

For deployments targeting recycled office hardware (HP/Dell/Lenovo), the Broadcom ethernet drivers (tg3, bnx2, bnx2x) are the single highest-impact addition. Most HP ProLiant and Dell PowerEdge servers use Broadcom NICs.

---

## 10. Implementation Plan

### Phase 1: Quick Wins (kernel 6.6, config changes only)

1. Add ethernet driver configs to `linux.config`
2. Add WiFi driver configs to `linux.config`
3. Add firmware files to overlay or enable selective linux-firmware packages
4. Update `init_load_modules()` in `init.cpp` with new module load order
5. Test build, verify modules compile
6. Test with VirtualBox (virtio_net covers VM, but verify module loading)

### Phase 2: Firmware Collection

1. Download firmware from linux-firmware.git for: bnx2, bnx2x, bnxt_en, ath12k, mt7925 (if upgrading), rtl8xxxu, rt2800pci
2. Add to overlay or Buildroot linux-firmware package selections
3. Verify firmware sizes fit in squashfs budget

### Phase 3: Kernel Upgrade (when ready)

1. Evaluate Buildroot 2024.11 or later for kernel 6.12 support
2. Update linux.config for 6.12 kernel
3. Enable MT7925, rtw89 USB, improved rtw88 USB
4. Full regression test

### Module Load Order for init_load_modules()

```cpp
// Add to the dependency-ordered module list:
// Ethernet (no dependencies, load early)
"tg3.ko",
"bnx2.ko",
"bnx2x.ko",
"bnxt_en.ko",
"alx.ko",
"atl1c.ko",
"atlantic.ko",
"sky2.ko",
// WiFi (after mac80211 + cfg80211 which are already loaded)
"ath12k_core.ko", "ath12k_pci.ko",  // ath12k needs ath12k_core first
"rtl8xxxu.ko",
"brcmsmac.ko",
"rt2x00lib.ko", "rt2x00pci.ko", "rt2800lib.ko", "rt2800pci.ko",
"mt7921u.ko",  // mt76 USB variant
```

---

## Sources

- [USB WiFi Chipsets Guide (morrownr/USB-WiFi)](https://github.com/morrownr/USB-WiFi/blob/main/home/USB_WiFi_Chipsets.md)
- [Linux Wireless Drivers Wiki](https://wireless.wiki.kernel.org/en/users/drivers)
- [Intel Wireless Linux Support](https://www.intel.com/content/www/us/en/support/articles/000005511/wireless.html)
- [ath12k Installation Guide](https://wireless.docs.kernel.org/en/latest/en/users/drivers/ath12k/installation.html)
- [Marvell AQtion Driver Documentation](https://docs.kernel.org/networking/device_drivers/ethernet/aquantia/atlantic.html)
- [Linux Kernel Driver Database](https://cateee.net/lkddb/web-lkddb/)
- [Wi-Fi Chipset Market Analysis (Mordor Intelligence)](https://www.mordorintelligence.com/industry-reports/global-wi-fi-chipset-market)
- [lwfinger/rtw88 Backport Repository](https://github.com/lwfinger/rtw88)
- [igc Driver - Kernel Config](https://cateee.net/lkddb/web-lkddb/IGC.html)
- [alx Driver - Linux Foundation Wiki](https://wiki.linuxfoundation.org/networking/alx)
