# Research 21: Power Management, Laptop Support & Thermal Management

> **Context**: Llamaste runs as PID 1 with no systemd, no acpid, no TLP, no upower.
> All power management must be handled directly by the llamaste binary via sysfs,
> procfs, netlink sockets, and the Linux input subsystem. Target hardware includes
> old laptops, desktops, and servers.

---

## Table of Contents

1. [ACPI Events Without systemd/acpid](#1-acpi-events-without-systemdacpid)
2. [Suspend/Resume](#2-suspendresume)
3. [Battery Management](#3-battery-management)
4. [CPU Frequency Scaling](#4-cpu-frequency-scaling)
5. [Thermal Management](#5-thermal-management)
6. [Wake-on-LAN](#6-wake-on-lan)
7. [Power Consumption Profiling](#7-power-consumption-profiling)
8. [UPS / Graceful Shutdown](#8-ups--graceful-shutdown)
9. [Desktop Mode Display Power](#9-desktop-mode-display-power)
10. [Recommendations for Llamaste](#10-recommendations-for-llamaste)

---

## 1. ACPI Events Without systemd/acpid

### How Linux Exposes ACPI Events

The legacy `/proc/acpi/event` interface is deprecated. Modern kernels (3.x+) expose
ACPI events through two mechanisms:

**A) Generic Netlink (ACPI subsystem events)**

The ACPI subsystem registers a Generic Netlink family called `acpi_event`. Userspace
programs receive AC adapter events, thermal events, and other ACPI notifications by:

1. Opening an `AF_NETLINK` / `NETLINK_GENERIC` socket
2. Sending `CTRL_CMD_GETFAMILY` to resolve the `"acpi_event"` family ID and its
   multicast group ID (both are dynamically assigned)
3. Subscribing to the multicast group via `setsockopt()` or bind bitmask
4. Calling `recvmsg()` in a blocking loop to receive `acpi_genl_event` structs
   containing: `device_class`, `bus_id`, `type`, `data`

The acpid2 source code (github.com/tedfelix/acpid2, specifically `netlink.c` and
`acpi_ids.c`) is the definitive C reference for this approach. The code is roughly
400 lines for the netlink portion and has no external dependencies beyond libc.

**B) Linux Input Subsystem (button events)**

Power button, sleep button, and lid switch events are exposed as `/dev/input/event*`
devices using the standard evdev interface:

- **Power button**: `EV_KEY` (type=1), `KEY_POWER` (code=116)
- **Sleep button**: `EV_KEY` (type=1), `KEY_SLEEP` (code=142)
- **Lid switch**: `EV_SW` (type=5), `SW_LID` (code=0), value=1 closed / value=0 open

Reading these events from C++ is straightforward:

```cpp
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>

// Open the appropriate /dev/input/eventN for the power button
int fd = open("/dev/input/event2", O_RDONLY);
struct input_event ev;
while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
    if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
        handle_power_button();
    if (ev.type == EV_SW && ev.code == SW_LID)
        handle_lid(ev.value); // 1=closed, 0=opened
}
```

Device discovery: enumerate `/dev/input/event*`, use `EVIOCGNAME` ioctl to find
devices named "Power Button", "Lid Switch", "Sleep Button". Alternatively, parse
`/proc/bus/input/devices`.

### AC Adapter Plug/Unplug Detection

AC adapter state changes appear in sysfs and can be detected by:

1. **Polling** `/sys/class/power_supply/AC0/online` (or `ADP0`, `ADP1`) -- returns
   "1" when plugged in, "0" when on battery
2. **udev-style uevent monitoring** via `NETLINK_KOBJECT_UEVENT` socket -- the kernel
   sends a uevent when power supply attributes change (SUBSYSTEM=power_supply)
3. **inotify** on the sysfs files (unreliable for sysfs; uevent monitoring preferred)

For Llamaste, the uevent approach is best: open a `NETLINK_KOBJECT_UEVENT` socket
and filter for `SUBSYSTEM=power_supply` events. This gives real-time notification
of AC plug/unplug without polling.

### Recommended Approach for Llamaste

Use the **input subsystem** for power button and lid switch (simpler, more reliable
across hardware), and **NETLINK_KOBJECT_UEVENT** for AC adapter and battery state
changes. Generic Netlink for ACPI is an alternative but adds complexity for events
that are already available through simpler interfaces.

**Implementation estimate**: ~300 lines C++ for a complete ACPI event listener
covering power button, lid switch, and AC adapter state changes.

---

## 2. Suspend/Resume

### Triggering Suspend from Userspace

```bash
echo mem > /sys/power/state     # S3 suspend-to-RAM
echo disk > /sys/power/state    # S4 hibernate (suspend-to-disk)
echo freeze > /sys/power/state  # S0ix / s2idle (modern standby)
```

Available states can be read from `cat /sys/power/state`. On modern hardware (post-2020
Intel/AMD), traditional S3 may not be available -- only `s2idle` (S0ix) is offered.
Older laptops (the Llamaste target) will typically support S3.

### What Happens to llama.cpp During Suspend

**S3 (Suspend-to-RAM):**
- RAM stays powered. All process state -- mmap'd model files, KV cache, thread stacks,
  page tables -- is fully preserved in RAM.
- On resume, the process continues exactly where it left off. No model reload needed.
- Threads frozen by the kernel before suspend are thawed on resume transparently.
- The mmap'd GGUF model file remains in the page cache. Demand paging handles any
  evicted pages transparently on access.

**S4 (Hibernate / Suspend-to-disk):**
- The kernel creates a memory snapshot and writes it to swap space, then powers off.
- On resume, a fresh kernel boots, loads the snapshot, and restores all memory.
- From the process perspective, this is transparent -- mmap, threads, KV cache all
  survive. However, hibernate requires swap space >= RAM size.
- CPU microcode is reapplied on resume (kernel handles this).
- Resume takes longer (10-30 seconds depending on RAM size and disk speed).

**Impact on Running Inference:**
- Any in-progress token generation is simply paused and resumed.
- Network connections (WebSocket, SSE) will break. Clients must reconnect.
- DHCP lease may need renewal after resume (especially after long suspend periods).
- System clock jumps forward; any timeout-based logic needs awareness of time gaps.

### Network Reconnection After Resume

- The kernel re-initializes the NIC driver on resume
- DHCP lease renewal: Llamaste should trigger `dhclient` equivalent after resume
  (send DHCPREQUEST to renew lease via raw socket or use a minimal DHCP client)
- WebSocket/SSE clients see a connection drop and must reconnect
- mDNS/DNS-SD announcements should be re-broadcast

### Should Llamaste Support Suspend?

**Recommendation: Yes, but as an opt-in feature for laptop mode.**

Rationale:
- On laptops, suspend is essential for battery preservation during idle periods
- On servers, suspend is counterproductive (use WoL + shutdown instead)
- On desktops, suspend is a nice-to-have

Default behavior table (see Section 10) defines when to suspend based on mode.

### Hibernate Considerations

Hibernate requires:
- Swap partition or swap file >= RAM size
- `CONFIG_HIBERNATION=y` in kernel config
- Resume device configured in kernel command line (`resume=/dev/sdXN`)

**Recommendation**: Do not support hibernate in Phase 1. It adds complexity (swap
management, kernel cmdline configuration) for minimal benefit over S3. Revisit in
Phase 2 if users request it.

---

## 3. Battery Management

### Reading Battery Status from sysfs

Battery information lives under `/sys/class/power_supply/BAT0/` (or BAT1, etc.).
Some laptops use different naming. Enumerate `/sys/class/power_supply/*/type` to
find entries where type="Battery".

Key attributes:

| Attribute | Unit | Description |
|-----------|------|-------------|
| `status` | string | "Charging", "Discharging", "Not charging", "Full" |
| `capacity` | percent | Current charge percentage (0-100) |
| `energy_now` | uWh | Current remaining energy (microWatt-hours) |
| `energy_full` | uWh | Current full capacity |
| `energy_full_design` | uWh | Original design capacity |
| `power_now` | uW | Current power draw (microWatts) |
| `voltage_now` | uV | Current voltage (microVolts) |
| `charge_now` | uAh | Alternative to energy_now (microAmp-hours) |
| `charge_full` | uAh | Alternative to energy_full |

**Important**: Some systems expose `energy_*` attributes, others expose `charge_*`.
Some expose both. The code must try both paths. On some ThinkPads, which set of
attributes appears depends on whether the system booted on AC or battery.

### Reading in C++

```cpp
int read_sysfs_int(const char* path) {
    std::ifstream f(path);
    int val = -1;
    if (f.is_open()) f >> val;
    return val;
}

std::string read_sysfs_string(const char* path) {
    std::ifstream f(path);
    std::string val;
    if (f.is_open()) std::getline(f, val);
    return val;
}

// Battery percentage
int capacity = read_sysfs_int("/sys/class/power_supply/BAT0/capacity");

// Charging state
std::string status = read_sysfs_string("/sys/class/power_supply/BAT0/status");

// Power draw in watts
long power_uw = read_sysfs_int("/sys/class/power_supply/BAT0/power_now");
double power_w = power_uw / 1e6;
```

### Estimated Runtime Calculation

```
remaining_wh = energy_now / 1e6  (convert uWh to Wh)
current_draw_w = power_now / 1e6  (convert uW to W)
remaining_hours = remaining_wh / current_draw_w
```

This gives a rough estimate. The power_now value fluctuates; a rolling average
over 30-60 seconds provides a smoother estimate.

### Low Battery Action Ladder

| Battery % | Action |
|-----------|--------|
| <= 30% | Inform user via web UI banner: "Battery at X%" |
| <= 20% | Warning: "Low battery. Consider plugging in or saving work." |
| <= 15% | Auto-switch to smaller model if available (e.g., 3B -> 1.5B -> 0.5B) |
| <= 10% | Critical warning. Reduce inference threads. Disable speculative decoding. |
| <= 5% | Initiate graceful shutdown sequence |
| <= 3% | Emergency: sync + immediate power off |

### Model Switching on Battery

When switching from AC to battery, Llamaste can:
1. Continue with current model (default for short battery sessions)
2. Offer to switch to a smaller model (longer battery life)
3. Auto-switch below 15% threshold

A Qwen2.5-0.5B model uses dramatically less compute than 3B or 7B, extending
battery life by 3-5x at the cost of reduced capability. The switch should:
- Save current KV cache state if possible
- Unload current model
- Load smaller model
- Notify user via web UI

### Battery Health Monitoring

```
health_pct = (energy_full / energy_full_design) * 100
```

Display battery health in the web UI system dashboard. Warn if health < 50%
("Battery degraded -- consider replacement for sustained use").

---

## 4. CPU Frequency Scaling (cpufreq)

### Available Governors

| Governor | Behavior | Use Case |
|----------|----------|----------|
| `performance` | Lock to max frequency | Server mode, desktop on AC |
| `powersave` | Lock to min frequency | Extreme battery saving |
| `schedutil` | Kernel scheduler-driven scaling | Best general-purpose on battery |
| `ondemand` | Scale based on CPU load | Legacy alternative to schedutil |
| `conservative` | Gradual scaling (less aggressive) | Quiet/cool operation |

### Setting Governor from C++ via sysfs

```cpp
bool set_governor(int cpu, const std::string& governor) {
    std::string path = "/sys/devices/system/cpu/cpu"
                     + std::to_string(cpu)
                     + "/cpufreq/scaling_governor";
    std::ofstream f(path);
    if (!f.is_open()) return false;
    f << governor;
    return f.good();
}

// Apply to all CPUs
int num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
for (int i = 0; i < num_cpus; i++)
    set_governor(i, "performance");
```

Requires root (Llamaste runs as PID 1, so this is always the case).

### Intel P-State vs Generic cpufreq

Older laptops (pre-Skylake, pre-2015) use `acpi-cpufreq` which supports all
generic governors. Newer Intel CPUs use `intel_pstate` which in active mode only
exposes `performance` and `powersave` -- but these behave differently than the
generic governors (they are hints to the CPU's internal governor).

Check which driver is active:
```
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_driver
```

Possible values: `acpi-cpufreq`, `intel_pstate`, `intel_cpufreq` (passive mode),
`amd-pstate`, `amd-pstate-epp`.

For `intel_pstate` active mode, use the Energy Performance Preference (EPP):
```
# Read available preferences
cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_available_preferences
# balance_performance balance_power default performance power

# Set preference
echo "balance_power" > /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference
```

### AMD CPPC (Collaborative Processor Performance Control)

AMD Zen 2+ CPUs may use `amd-pstate` (kernel 5.17+). Same EPP interface as Intel.
Falls back to `acpi-cpufreq` on older AMD CPUs or if BIOS lacks CPPC support.

### Turbo Boost Control

```bash
# Intel P-State: disable turbo
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo

# Generic cpufreq / AMD: disable turbo
echo 0 > /sys/devices/system/cpu/cpufreq/boost
```

Disabling turbo on battery reduces peak power draw and heat, at the cost of ~20-40%
lower peak single-thread performance. For sustained LLM inference, turbo often
causes thermal throttling anyway on older laptops -- disabling it can paradoxically
improve sustained throughput by maintaining a steady frequency.

### Recommended Power Profiles for Llamaste

| Mode | On AC | On Battery |
|------|-------|------------|
| **Server** | `performance` governor, turbo ON | N/A (servers don't have batteries) |
| **Desktop** | `performance` governor, turbo ON | `schedutil`, turbo OFF |
| **Laptop** | `performance` governor, turbo ON | `schedutil`, turbo OFF, EPP=`balance_power` |

On AC-to-battery transition:
1. Switch governor to `schedutil` (or EPP to `balance_power` for pstate drivers)
2. Disable turbo boost
3. Optionally reduce `scaling_max_freq` to 80% of max

On battery-to-AC transition:
1. Switch governor to `performance` (or EPP to `performance`)
2. Enable turbo boost
3. Restore `scaling_max_freq` to hardware maximum

### Kernel Boot Parameter

For Buildroot, set default governor in kernel config:
```
CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y
```
Or via kernel command line: `cpufreq.default_governor=schedutil`

This sets the default before Llamaste has a chance to configure it, ensuring
reasonable behavior during early boot.

---

## 5. Thermal Management

### Reading Temperatures

Thermal zones are exposed at `/sys/class/thermal/thermal_zone[0-*]/`:

```cpp
int read_cpu_temp() {
    // Returns temperature in millidegrees Celsius
    return read_sysfs_int("/sys/class/thermal/thermal_zone0/temp");
    // Divide by 1000 for degrees C
}
```

Multiple thermal zones may exist (CPU, GPU, SSD, battery, ambient). Identify them:
```
cat /sys/class/thermal/thermal_zone0/type  # e.g., "x86_pkg_temp", "acpitz"
```

### Trip Points

Each thermal zone has trip points defined by firmware/BIOS:

```
/sys/class/thermal/thermal_zone0/trip_point_0_temp   # e.g., 85000 (85C)
/sys/class/thermal/thermal_zone0/trip_point_0_type    # "passive" or "critical"
```

- **passive**: Kernel begins throttling (reduces CPU frequency)
- **active**: Kernel increases fan speed
- **critical**: Kernel initiates emergency shutdown

### Thermal Management State Machine for Llamaste

```
                    +-----------+
                    |   COOL    |  < 60C
                    | (Normal)  |
                    +-----+-----+
                          |
                     temp >= 65C
                          |
                    +-----v-----+
                    |   WARM    |  65-75C
                    | (Monitor) |
                    +-----+-----+
                          |
                     temp >= 75C
                          |
                    +-----v-----+
                    |    HOT    |  75-85C
                    | (Throttle)|
                    +-----+-----+
                          |
                     temp >= 85C
                          |
                    +-----v-----+
                    | CRITICAL  |  85-95C
                    | (Protect) |
                    +-----+-----+
                          |
                     temp >= 95C
                          |
                    +-----v-----+
                    | EMERGENCY |  >= 95C
                    | (Shutdown)|
                    +-----------+
```

State transitions and actions (with 3C hysteresis on downtransitions):

| State | Temp Range | Actions |
|-------|-----------|---------|
| **COOL** | < 60C | Full performance. All threads active. Turbo enabled (if AC). |
| **WARM** | 65-75C | Monitoring mode. Log temperatures. No action needed yet. |
| **HOT** | 75-85C | Reduce inference threads by 25%. Disable turbo. Disable speculative decoding. Warn user in web UI. |
| **CRITICAL** | 85-95C | Reduce inference threads by 50%. Set CPU governor to powersave. Consider model downgrade. Urgent warning in web UI. |
| **EMERGENCY** | >= 95C | Abort inference. Sync filesystems. Graceful shutdown. |

Hysteresis: transitions down require temperature to drop 3C below the threshold
(e.g., HOT -> WARM requires temp < 72C, not just < 75C). This prevents oscillation.

### Thread Reduction Strategy

llama.cpp uses `n_threads` for prompt processing and `n_threads_batch` for token
generation. Llamaste should expose both to the thermal manager:

```cpp
// Normal: use all physical cores
server.set_threads(num_physical_cores);

// HOT: reduce by 25%
server.set_threads(num_physical_cores * 3 / 4);

// CRITICAL: reduce by 50%
server.set_threads(num_physical_cores / 2);
```

Reducing threads lowers CPU utilization and heat output, at the cost of slower
inference. On a 4-core old laptop, going from 4 -> 3 threads reduces throughput
by ~25% but can drop temperature by 10-15C.

### Fan Speed Monitoring

Fans are exposed under `/sys/class/hwmon/hwmonX/`:

```
fan1_input      # Current RPM (read-only on most laptops)
pwm1            # Fan duty cycle, 0-255 (writable if pwm1_enable=1)
pwm1_enable     # 0=disabled, 1=manual, 2=automatic
```

**Caution**: hwmon numbering is unstable across reboots. Identify the correct hwmon
by checking `/sys/class/hwmon/hwmonX/name` (e.g., "coretemp", "thinkpad", "dell_smm").

Most laptops have firmware-controlled fans that work automatically. Llamaste should:
1. Monitor fan RPM as a telemetry signal (report in web UI)
2. NOT attempt to control fan speed on unknown hardware (risk of damage)
3. Only control fans on well-known platforms (ThinkPad, Dell) with explicit user opt-in

### What's Safe for Old Laptops?

Old laptops running sustained LLM inference face:
- **Thermal paste degradation**: 10-year-old thermal paste may have dried out,
  reducing heat transfer. Recommend users re-paste if temperatures exceed 80C at
  reduced thread counts.
- **Clogged vents**: Dust accumulation reduces airflow. Recommend compressed air.
- **Fan bearing wear**: Old fans may not spin at full speed. Monitor RPM.
- **Battery bloat risk**: Heat + old battery = potential swelling. Monitor battery
  temperature if exposed via sysfs.

Safe sustained temperature targets for old laptops:
- Target: keep CPU < 75C for sustained operation
- Maximum: 85C (with thermal throttling reducing performance)
- Shutdown: 95C (protect hardware)

These are conservative thresholds. Modern CPUs can handle 100C+ briefly, but
sustained operation at those temperatures on old hardware risks component failure.

---

## 6. Wake-on-LAN

### How WoL Works

1. NIC stays powered in low-power state when system is off/suspended
2. NIC monitors incoming frames for a "magic packet" containing 6 bytes of 0xFF
   followed by 16 repetitions of the target MAC address (102 bytes total)
3. On receiving the magic packet, NIC signals the motherboard to power on
4. System boots normally through BIOS/GRUB as if the power button was pressed

### Prerequisites

- WoL must be enabled in BIOS/UEFI ("PCI Device Power On" or similar)
- ErP/EuP mode must be disabled in BIOS (otherwise NIC is unpowered when off)
- NIC must support WoL (check with `ethtool <iface>` for "Supports Wake-on: g")

### Enabling WoL from Llamaste

```cpp
// Using ioctl (avoids dependency on ethtool binary)
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>

void enable_wol(const char* iface) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr = {};
    strncpy(ifr.ifr_name, iface, IFNAMSIZ);

    struct ethtool_wolinfo wol = {};
    wol.cmd = ETHTOOL_SWOL;
    wol.wolopts = WAKE_MAGIC;  // Enable magic packet wake

    ifr.ifr_data = (char*)&wol;
    ioctl(fd, SIOCETHTOOL, &ifr);
    close(fd);
}
```

**Important**: WoL settings are reset on every boot by most NIC drivers. Llamaste
must re-enable WoL during its initialization if the user has configured it.

### WoL Use Case for Llamaste Server Mode

1. Server is idle for N minutes (configurable)
2. Llamaste syncs state, writes checkpoint
3. System enters S5 (power off) or S3 (suspend)
4. Remote client sends magic packet to server's MAC address
5. Server boots, GRUB loads Llamaste kernel
6. Llamaste starts, loads model, begins accepting requests
7. Client retries connection until server is ready (~30-60s boot + model load)

This enables a "serverless" experience for home lab deployments while consuming
zero power when idle.

### WoL from Suspended vs Powered Off State

- **From S3 (suspend)**: NIC is powered, WoL works. Resume is fast (2-5s).
- **From S5 (power off)**: NIC is powered by standby rail if BIOS supports it.
  Cold boot takes 30-60s.
- **GRUB behavior**: WoL wake triggers a normal boot sequence. GRUB runs normally,
  default menu entry is selected (or timeout proceeds to default).

---

## 7. Power Consumption Profiling

### Typical Power Draw by Hardware Category

| Hardware | Idle | Light Use | LLM Inference (sustained) |
|----------|------|-----------|---------------------------|
| Old laptop (2012-2016, 15W-35W TDP) | 8-15W | 15-25W | 25-45W |
| Old desktop (2012-2016, 65W-95W TDP) | 40-70W | 60-90W | 80-130W |
| Modern laptop (2020+, 15-28W TDP) | 5-10W | 10-20W | 20-40W |
| Server (dual socket, 2015-era) | 80-120W | 100-150W | 150-250W |

### Power Breakdown During CPU-Only LLM Inference

| Component | Percentage of Total | Typical Draw (old laptop) |
|-----------|--------------------|-|
| CPU | 50-65% | 15-30W |
| RAM (DDR3/DDR4, 2 DIMMs) | 8-15% | 3-8W |
| Display (laptop) | 10-20% | 3-8W |
| SSD/HDD | 2-5% | 1-3W |
| Chipset + peripherals | 10-15% | 3-6W |

### RAM Power: DDR3 vs DDR4

| Memory Type | Voltage | Per-DIMM Active | Per-DIMM Idle |
|------------|---------|-----------------|---------------|
| DDR3 (1.5V) | 1.5V | 3-5W | 1.4-2W |
| DDR3L (1.35V) | 1.35V | 2-4W | 1-1.5W |
| DDR4 (1.2V) | 1.2V | 2-5W | 1.2-2W |

For a laptop with 16GB (2x8GB DDR3), RAM alone draws 3-10W during inference.
With 32GB (4x8GB DDR4 server), RAM draws 5-20W. This is significant for
battery calculations.

### Battery Life Estimates During LLM Inference

| Laptop Battery | Total System Draw | Estimated Runtime |
|---------------|-------------------|-------------------|
| 40Wh (old ThinkPad) | 35W sustained | ~1.1 hours |
| 50Wh (typical old laptop) | 35W sustained | ~1.4 hours |
| 60Wh (large old laptop) | 40W sustained | ~1.5 hours |
| 80Wh (modern, if applicable) | 30W sustained | ~2.7 hours |

With model-downgrade and thread reduction on battery (using 0.5B model, 2 threads):
- Power draw drops to ~20W
- 50Wh battery: ~2.5 hours
- This is a rough 1.8x improvement over running the full model

### Energy per Token (CPU-only, Rough Estimates)

For a 7B Q4_0 model on a 4-core old laptop generating ~5 tokens/second at 35W:
- Energy per token: 35W / 5 tok/s = 7 Joules/token
- For a 0.5B Q4_0 model at ~15 tokens/second at 20W:
- Energy per token: 20W / 15 tok/s = 1.3 Joules/token

The smaller model is ~5x more energy-efficient per token.

### Reading Actual Power Draw

On laptops with battery, `power_now` gives real-time system power consumption:
```cpp
// System power draw in watts
long power_uw = read_sysfs_int("/sys/class/power_supply/BAT0/power_now");
double watts = power_uw / 1e6;
```

On desktops/servers without battery, Intel RAPL provides per-package power data:
```
/sys/class/powercap/intel-rapl/intel-rapl:0/energy_uj  # Cumulative energy in uJ
```
Read twice with a known interval, compute delta for instantaneous power.

---

## 8. UPS / Graceful Shutdown

### Graceful Shutdown Sequence from PID 1

Llamaste as PID 1 must handle shutdown carefully:

```cpp
#include <sys/reboot.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mount.h>

void graceful_shutdown() {
    // 1. Stop accepting new requests
    server.stop_accepting();

    // 2. Wait for in-flight requests to complete (timeout: 5s)
    server.drain(5000);

    // 3. Save state (conversation history, model cache metadata)
    save_state();

    // 4. Send SIGTERM to any child processes (if any)
    kill(-1, SIGTERM);
    sleep(2);
    kill(-1, SIGKILL);

    // 5. Sync all filesystems
    sync();

    // 6. Remount root filesystem read-only
    mount("/", "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);

    // 7. Power off
    reboot(RB_POWER_OFF);
    // Does not return on success
}
```

The `reboot(RB_POWER_OFF)` syscall requires `CAP_SYS_BOOT` -- which PID 1 has
by default. The magic numbers (0xfee1dead, 0x28121969) are handled by the glibc
wrapper; the one-argument form is sufficient.

### UPS Detection

**Full NUT (Network UPS Tools)**: Too heavy for Llamaste. NUT includes multiple
daemons (upsd, upsmon, driver), configuration files, and dependencies.

**Lightweight alternative: Direct USB HID**

Most consumer USB UPSes implement the USB HID Power Device Class. Linux exposes
these as `/dev/hidraw*` devices. Llamaste can:

1. Scan `/sys/class/hidraw/*/device/uevent` for `HID_NAME` containing "UPS" or
   matching known vendor IDs (APC=051d, CyberPower=0764, Eaton=0463)
2. Open the corresponding `/dev/hidrawN`
3. Parse HID reports for battery percentage, AC status, time remaining
4. Trigger shutdown when battery low or AC lost for > N seconds

This is ~200-400 lines of C++ and zero dependencies beyond the kernel HID driver.

**Even simpler: Some USB UPSes appear as `/sys/class/power_supply/` devices**
if the `hid-generic` or `usbhid` driver is loaded with the power supply module.
Check for power_supply entries with `type` = "UPS".

### Shutdown Triggers

| Trigger | Action |
|---------|--------|
| Power button press (short) | Graceful shutdown |
| Power button press (long, 4s) | Kernel handles forced power off via ACPI |
| UPS on battery + UPS battery < 20% | Graceful shutdown |
| UPS on battery > 5 minutes | Graceful shutdown (configurable) |
| System battery < 5% | Graceful shutdown |
| System battery < 3% | Emergency shutdown (sync + power off immediately) |
| Thermal emergency (>95C) | Emergency shutdown |
| Ctrl-Alt-Del (from console) | Graceful shutdown (kernel signals PID 1) |

---

## 9. Desktop Mode Display Power

### DPMS (Display Power Management Signaling)

DPMS defines four states for monitors: Normal, Standby, Suspend, Off. On LCD
monitors, standby/suspend/off all produce the same result (backlight off).

**For the Linux console (no X11/Wayland):**
```cpp
// Blank console after N seconds of inactivity
// Write escape sequence to /dev/console
// setterm --blank N --powerdown N
int fd = open("/dev/console", O_WRONLY);
// ESC [ 9 ; N ] -- set blank time in minutes
char cmd[32];
snprintf(cmd, sizeof(cmd), "\033[9;%d]", minutes);
write(fd, cmd, strlen(cmd));
```

**For Wayland (Cage/Labwc compositor):**
wlroots-based compositors support idle timeout configuration. Labwc supports an
idle timeout that triggers DPMS off. Configure via compositor config or via the
`ext-idle-notify-v1` Wayland protocol.

### Backlight Control

Laptop backlight is controlled via sysfs:

```cpp
// Find backlight device
// /sys/class/backlight/intel_backlight/ or /sys/class/backlight/acpi_video0/

int max_br = read_sysfs_int("/sys/class/backlight/intel_backlight/max_brightness");

void set_brightness(int percent) {
    int val = max_br * percent / 100;
    write_sysfs_int("/sys/class/backlight/intel_backlight/brightness", val);
}

void set_backlight_power(bool on) {
    // 0 = FB_BLANK_UNBLANK (on), 4 = FB_BLANK_POWERDOWN (off)
    write_sysfs_int("/sys/class/backlight/intel_backlight/bl_power", on ? 0 : 4);
}
```

### Should Inference Continue When Screen Is Off?

**Yes.** Display power state should be independent of inference. Use cases:

- User starts a long generation, walks away, screen blanks -> generation continues
- Server mode (headless) has no display at all
- Laptop lid closed -> may or may not suspend, but inference should continue
  until suspend actually occurs

Inference should only stop for thermal/battery/shutdown reasons, never because
the display turned off.

---

## 10. Recommendations for Llamaste

### Phase 1 (Essential)

These must be implemented before first release:

| Feature | Priority | Effort | Description |
|---------|----------|--------|-------------|
| **Power button handling** | P0 | ~100 LOC | Detect via evdev, initiate graceful shutdown |
| **Graceful shutdown** | P0 | ~150 LOC | Signal handling, sync, unmount, reboot() syscall |
| **Thermal monitoring** | P0 | ~200 LOC | Read thermal_zone temps, implement state machine |
| **Thermal throttling** | P0 | ~100 LOC | Reduce threads and disable turbo when hot |
| **CPU governor setup** | P1 | ~100 LOC | Set performance governor on boot (server mode) |
| **Battery status reading** | P1 | ~150 LOC | Read capacity, status, power_now from sysfs |
| **Low battery shutdown** | P1 | ~50 LOC | Shutdown at 5%, warn at 20% |
| **AC adapter detection** | P1 | ~100 LOC | Detect plug/unplug via uevent or polling |

**Estimated total for Phase 1**: ~950 lines of C++

### Phase 2 (Important)

| Feature | Priority | Effort | Description |
|---------|----------|--------|-------------|
| Lid close handling | P2 | ~50 LOC | Detect via evdev, configurable action |
| Suspend/resume support | P2 | ~200 LOC | Write to /sys/power/state, DHCP renewal on resume |
| Battery-aware model switching | P2 | ~300 LOC | Auto-downgrade model on low battery |
| CPU governor switching on AC/battery | P2 | ~100 LOC | Performance on AC, schedutil on battery |
| Turbo boost management | P2 | ~50 LOC | Disable on battery, enable on AC |
| Display backlight control | P2 | ~100 LOC | Brightness control, idle blanking |
| Power stats in web UI | P2 | ~200 LOC | Battery %, temperature, power draw, estimated runtime |
| WoL configuration | P2 | ~150 LOC | Enable WoL on boot, auto-sleep-on-idle for servers |

**Estimated total for Phase 2**: ~1150 lines of C++

### Phase 3 (Nice to Have)

| Feature | Priority | Effort | Description |
|---------|----------|--------|-------------|
| USB UPS detection | P3 | ~300 LOC | USB HID parsing for common UPS brands |
| Fan speed monitoring | P3 | ~100 LOC | Read RPM via hwmon, display in web UI |
| RAPL power monitoring | P3 | ~100 LOC | Read actual CPU package power on Intel/AMD |
| Hibernate support | P3 | ~200 LOC | Swap management, resume configuration |
| Per-core frequency control | P3 | ~100 LOC | Pin inference threads to performance cores |
| Energy-per-token metrics | P3 | ~100 LOC | Track and display Joules/token |

### Default Behavior Table

| Event | Server Mode | Desktop Mode | Laptop Mode (AC) | Laptop Mode (Battery) |
|-------|-------------|--------------|-------------------|----------------------|
| **Power button (short)** | Graceful shutdown | Graceful shutdown | Graceful shutdown | Graceful shutdown |
| **Power button (long, 4s)** | Forced power off (kernel) | Forced power off | Forced power off | Forced power off |
| **Lid close** | N/A | N/A | Continue running | Suspend (S3) |
| **Lid open** | N/A | N/A | No action | Resume |
| **AC unplugged** | N/A | N/A | Switch to battery profile | Already on battery |
| **AC plugged in** | N/A | N/A | Switch to AC profile | Switch to AC profile |
| **Battery <= 20%** | N/A | N/A | N/A | Warning in web UI |
| **Battery <= 15%** | N/A | N/A | N/A | Downgrade model |
| **Battery <= 5%** | N/A | N/A | N/A | Graceful shutdown |
| **Battery <= 3%** | N/A | N/A | N/A | Emergency shutdown |
| **Temp >= 75C** | Reduce threads, disable turbo | Same | Same | Same + reduce further |
| **Temp >= 85C** | Throttle hard, warn user | Same | Same | Same |
| **Temp >= 95C** | Emergency shutdown | Same | Same | Same |
| **Idle > 30 min** | No action | Screen blank | Screen blank | Suspend (S3) |
| **Idle > 2 hours** | No action | No action | No action | Shutdown |
| **UPS on battery** | Start countdown to shutdown | N/A | N/A | N/A |

### Power Profile Summary

```
SERVER PROFILE:
  Governor: performance
  Turbo: enabled
  Threads: all physical cores
  Thermal: monitor, throttle at 85C
  Idle: stay running (WoL available for sleep-on-idle)

DESKTOP PROFILE:
  Governor: performance (AC), schedutil (no battery, but for future)
  Turbo: enabled
  Threads: all physical cores
  Thermal: monitor, throttle at 80C (lower threshold for quiet operation)
  Idle: screen blank after 10 min

LAPTOP-AC PROFILE:
  Governor: performance (or EPP=performance)
  Turbo: enabled
  Threads: all physical cores
  Thermal: monitor, throttle at 75C (laptops run hotter)
  Idle: screen blank after 5 min

LAPTOP-BATTERY PROFILE:
  Governor: schedutil (or EPP=balance_power)
  Turbo: disabled
  Threads: 75% of physical cores
  Thermal: monitor, throttle at 70C (more aggressive to save battery)
  Idle: screen blank after 2 min, suspend after 15 min idle
  Model: consider switching to smaller model below 15%
```

### Kernel Configuration Requirements

These kernel options must be enabled in the Buildroot kernel config:

```
# ACPI
CONFIG_ACPI=y
CONFIG_ACPI_AC=y
CONFIG_ACPI_BATTERY=y
CONFIG_ACPI_BUTTON=y        # Power/sleep/lid buttons via input
CONFIG_ACPI_FAN=y
CONFIG_ACPI_THERMAL=y
CONFIG_ACPI_PROCESSOR=y

# Power Management
CONFIG_PM=y
CONFIG_SUSPEND=y
CONFIG_PM_SLEEP=y
# CONFIG_HIBERNATION is not set  (Phase 1: skip)

# CPU Frequency
CONFIG_CPU_FREQ=y
CONFIG_CPU_FREQ_STAT=y
CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL=y
CONFIG_CPU_FREQ_GOV_PERFORMANCE=y
CONFIG_CPU_FREQ_GOV_POWERSAVE=y
CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y
CONFIG_CPU_FREQ_GOV_ONDEMAND=y
CONFIG_X86_ACPI_CPUFREQ=y
CONFIG_X86_INTEL_PSTATE=y

# Thermal
CONFIG_THERMAL=y
CONFIG_THERMAL_HWMON=y
CONFIG_THERMAL_DEFAULT_GOV_STEP_WISE=y
CONFIG_THERMAL_GOV_USER_SPACE=y

# Input (for power button, lid switch)
CONFIG_INPUT=y
CONFIG_INPUT_EVDEV=y

# Backlight
CONFIG_BACKLIGHT_CLASS_DEVICE=y

# HID (for USB UPS in Phase 3)
CONFIG_HID=y
CONFIG_USB_HID=y
CONFIG_HIDRAW=y

# Power supply
CONFIG_POWER_SUPPLY=y

# WoL (NIC drivers handle this, ensure relevant drivers are built)
```

### Implementation Architecture

The power management subsystem in Llamaste should be structured as:

```
PowerManager (singleton, runs in its own thread)
  |
  +-- EventListener
  |     +-- InputDeviceMonitor  (evdev: power button, lid switch)
  |     +-- UeventMonitor      (netlink: AC adapter, battery changes)
  |     +-- ThermalMonitor     (poll thermal_zone temps every 5s)
  |     +-- BatteryMonitor     (poll battery sysfs every 30s)
  |
  +-- PolicyEngine
  |     +-- ThermalPolicy      (state machine: COOL/WARM/HOT/CRITICAL/EMERGENCY)
  |     +-- BatteryPolicy      (action ladder: warn/downgrade/shutdown)
  |     +-- IdlePolicy         (screen blank, suspend timers)
  |     +-- PowerProfile       (server/desktop/laptop-ac/laptop-battery)
  |
  +-- Actuators
        +-- CpuFreqController  (governor, turbo, EPP, max_freq)
        +-- ThreadController   (adjust inference thread count)
        +-- BacklightController(brightness, power)
        +-- SuspendController  (echo mem > /sys/power/state)
        +-- ShutdownController (sync, unmount, reboot syscall)
        +-- ModelController    (request model swap from inference engine)
```

The `EventListener` thread uses `epoll` to multiplex across:
- evdev file descriptors (power button, lid switch)
- netlink socket (uevent for AC adapter)
- timerfd for periodic polling (thermal, battery)

This design ensures all power management runs in a single thread with no busy-waiting,
using ~0% CPU when idle and responding within milliseconds to events.

---

## References

### ACPI & Events
- [acpid2 source (netlink implementation)](https://github.com/tedfelix/acpid2)
- [Linux Kernel Input Event Codes](https://www.kernel.org/doc/html/v4.14/input/event-codes.html)
- [Linux Kernel Netlink Introduction](https://docs.kernel.org/userspace-api/netlink/intro.html)
- [Understanding Linux Power Button Events](https://www.baeldung.com/linux/power-button-behavior)

### Sleep States & Suspend
- [System Sleep States - Kernel Documentation](https://docs.kernel.org/admin-guide/pm/sleep-states.html)
- [Arch Wiki - Suspend and Hibernate](https://wiki.archlinux.org/title/Power_management/Suspend_and_hibernate)

### Battery & Power Supply
- [Kernel sysfs-class-power Documentation](https://www.kernel.org/doc/Documentation/ABI/testing/sysfs-class-power)
- [Linux Power Supply Class](https://www.kernel.org/doc/Documentation/power/power_supply_class.txt)

### CPU Frequency
- [CPU Performance Scaling - Kernel Documentation](https://docs.kernel.org/admin-guide/pm/cpufreq.html)
- [Intel P-State Documentation](https://www.kernel.org/doc/html/v5.0/admin-guide/pm/intel_pstate.html)
- [Arch Wiki - CPU Frequency Scaling](https://wiki.archlinux.org/title/CPU_frequency_scaling)

### Thermal
- [Generic Thermal Sysfs Driver - Kernel Documentation](https://docs.kernel.org/driver-api/thermal/sysfs-api.html)
- [Arch Wiki - Fan Speed Control](https://wiki.archlinux.org/title/Fan_speed_control)

### Display & Backlight
- [Kernel Backlight Documentation](https://docs.kernel.org/gpu/backlight.html)
- [Kernel sysfs-class-backlight](https://www.kernel.org/doc/Documentation/ABI/stable/sysfs-class-backlight)
- [Arch Wiki - DPMS](https://wiki.archlinux.org/title/Display_Power_Management_Signaling)
- [Arch Wiki - Backlight](https://wiki.archlinux.org/title/Backlight)

### Wake-on-LAN
- [Arch Wiki - Wake-on-LAN](https://wiki.archlinux.org/title/Wake-on-LAN)

### Power Consumption & Efficiency
- [TokenPowerBench: LLM Inference Power Benchmarking](https://arxiv.org/html/2512.03024v1)
- [LLM Energy Efficiency Analysis](https://jacquesmattheij.com/llama-energy-efficiency/)
- [DDR4 DIMM Power Testing](https://www.servethehome.com/ddr4-dimms-system-power-consumption-tested/)
- [RAM Power Consumption (Crucial)](https://www.crucial.com/support/articles-faq-memory/how-much-power-does-memory-use)

### Shutdown & UPS
- [reboot(2) - Linux Manual Page](https://man7.org/linux/man-pages/man2/reboot.2.html)
- [Reverse Engineering USB HID (UPS)](https://popovicu.com/posts/how-to-reverse-engineer-usb-hid-on-linux/)
- [Arch Wiki - Power Management](https://wiki.archlinux.org/title/Power_management)
