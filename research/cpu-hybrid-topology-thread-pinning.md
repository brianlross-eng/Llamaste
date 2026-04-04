# CPU Hybrid Topology Detection & Thread Pinning for LLM Inference

**Date**: 2026-04-03
**Context**: Llamaste runs as PID 1 on Linux (no systemd). Targets Intel hybrid (Arrow Lake HX, Alder Lake, Raptor Lake), homogeneous Intel (Ice Lake i5-1035G1), and AMD (EVO-X2).

---

## 1. How Linux Detects P-cores vs E-cores

### 1.1 CPUID Leaf 0x1A (Native Detection)

The primary hardware mechanism. On each logical processor:
- **CPUID EAX=0x07, ECX=0**: Bit 15 of EDX = `Hybrid` flag. If set, CPU has mixed core types.
- **CPUID EAX=0x1A, ECX=0**: Bits 31:24 of EAX = Core Type.
  - `0x20` = Intel Atom (E-core / Gracemont / Skymont / Crestmont)
  - `0x40` = Intel Core (P-core / Golden Cove / Lion Cove / Raptor Cove)
  - `0x00` = Not hybrid (homogeneous CPU)

**Critical**: CPUID 0x1A returns the type of the core *currently executing*. Must pin thread to target CPU before calling CPUID. On homogeneous CPUs, leaf 0x1A returns 0 or is unsupported.

### 1.2 Sysfs Interfaces (Kernel 6.2+)

**`/sys/devices/system/cpu/types/`** (kernel 6.12+ patches, June 2024):
- `/sys/devices/system/cpu/types/intel_core/cpulist` -- lists P-core logical CPUs (e.g., `0-15`)
- `/sys/devices/system/cpu/types/intel_atom/cpulist` -- lists E-core logical CPUs (e.g., `16-31`)
- If this directory doesn't exist, CPU is homogeneous.

**Status**: These patches were submitted by Intel in June 2024 for vulnerability mitigation (RFDS). They may or may not be in kernel 6.12.78 LTS depending on backport status. Must check at runtime.

**`/sys/devices/system/cpu/cpuX/topology/`** (always available):
- `core_id` -- physical core ID
- `core_cpus_list` -- sibling threads on same core (HT pairs)
- `package_cpus_list` -- all CPUs in same physical package
- `cluster_id` -- cluster grouping (P-cores and E-cores are different clusters on hybrid)

**`/sys/devices/system/cpu/cpuX/cpufreq/`** (heuristic fallback):
- `cpuinfo_max_freq` -- P-cores have higher max freq than E-cores
- `scaling_max_freq` -- current scaling limit
- On i7-12700H: P-cores ~4700 MHz, E-cores ~3500 MHz
- **Caveat**: Not 100% reliable (turbo boost, thermal limits), but works as heuristic when cpu_type sysfs unavailable.

### 1.3 Intel Thread Director (ITD / HFI)

Hardware Feature Interface (HFI) provides real-time per-core performance and efficiency hints to the kernel scheduler. The kernel exposes this via `/sys/class/thermal/thermal_zone*/` but it's primarily for the scheduler's internal use, not directly useful for userspace thread pinning. Requires `CONFIG_INTEL_HFI_THERMAL=y` in kernel config.

---

## 2. Thread Pinning in C++ (Linux)

### 2.1 sched_setaffinity (Process-wide)

```c
#include <sched.h>

cpu_set_t mask;
CPU_ZERO(&mask);
CPU_SET(0, &mask);  // pin to CPU 0
CPU_SET(1, &mask);  // also allow CPU 1
sched_setaffinity(0, sizeof(mask), &mask);  // 0 = calling thread
```

### 2.2 pthread_setaffinity_np (Per-thread)

```c
#include <pthread.h>

cpu_set_t mask;
CPU_ZERO(&mask);
for (int i = 0; i < 8; i++) CPU_SET(i, &mask);  // P-cores 0-7
pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);
```

### 2.3 At thread creation (pthread_attr)

```c
pthread_attr_t attr;
pthread_attr_init(&attr);
pthread_attr_setaffinity_np(&attr, sizeof(mask), &mask);
pthread_create(&thread, &attr, worker_func, arg);
```

**Note**: `cpu_set_t` supports up to 1024 CPUs by default. For >1024, use `CPU_ALLOC()`.

---

## 3. Performance Impact: E-cores vs P-cores for LLM Inference

### 3.1 Measured Results (llama.cpp community)

From llama.cpp Discussion #572, Intel i7-12700H (6P + 8E):
- **7B Q4_0, all 20 threads (P+E)**: 481.99 ms/token
- **7B Q4_0, 6 threads on P-cores only**: 201.83 ms/token -- **2.4x faster**
- **65B Q4_0, all threads**: 4068.66 ms/token
- **65B Q4_0, P-cores only**: 1357.09 ms/token -- **3x faster**

### 3.2 Why E-cores Hurt LLM Performance

1. **Barrier synchronization**: GGML uses barrier-based parallelism. All threads must complete before the next layer. The slowest thread (E-core) dictates overall speed. E-cores run at ~60-70% of P-core speed, so all P-core threads idle waiting.

2. **SIMD width**: P-cores (Golden Cove/Lion Cove) have wider execution units. E-cores (Gracemont) have narrower SIMD pipelines. Even when both support AVX2, P-cores execute AVX2 faster.

3. **Cache hierarchy**: P-cores typically have larger L2 caches (1.25-2MB each). E-cores share smaller L2 (2-4MB per 4-core cluster). LLM inference is memory-bandwidth-bound; more cache per thread helps.

4. **Scheduler thrashing**: Without explicit pinning, the Linux CFS scheduler moves threads between P and E cores based on load, causing cache cold starts and TLB flushes.

---

## 4. Arrow Lake HX and AVX-512

### 4.1 The Critical Finding

**Arrow Lake (all SKUs including HX) does NOT expose AVX-512 to software.**

- Lion Cove P-cores: AVX-512 capable in hardware
- Skymont E-cores: NO AVX-512 support
- **Intel's decision**: AVX-512 is disabled at the platform level to maintain ISA parity between P and E cores. CPUID will NOT report AVX-512 support on any Arrow Lake processor.

This means:
- `GGML_NATIVE=ON` with `-march=native` on Arrow Lake will NOT generate AVX-512 instructions
- No SIGILL risk from AVX-512 on Arrow Lake -- the compiler won't emit it because CPUID doesn't advertise it
- AVX2 + VNNI is the highest SIMD level available on Arrow Lake

### 4.2 Alder Lake / Raptor Lake (12th/13th/14th Gen)

Same situation: AVX-512 disabled at platform level when E-cores present. Some users report re-enabling AVX-512 by disabling E-cores in BIOS, but this is not a viable strategy for Llamaste (we don't control BIOS).

### 4.3 Implications for Llamaste

- **`GGML_NATIVE=ON` is safe** on all Intel hybrid CPUs -- the compiler queries CPUID and won't use instructions the CPU doesn't advertise
- **Thread pinning to P-cores is a pure performance optimization**, not a correctness requirement
- The AVX-512 SIGILL scenario only applies if we manually force AVX-512 codegen via compiler flags (which we don't)

---

## 5. llama.cpp Thread Affinity Support

### 5.1 Built-in CPU Control (Current as of 2025)

llama.cpp has native thread affinity support via these parameters:

| Parameter | Description |
|-----------|-------------|
| `--cpu-mask <hex>` / `-C` | CPU affinity mask in hex notation |
| `--cpu-range <lo-hi>` / `-Cr` | CPU range for affinity (e.g., `0-7`) |
| `--cpu-strict <0\|1>` | Strict placement: 1 = one thread per core |
| `--cpu-mask-batch` / `-Cb` | Separate mask for batch processing |
| `--cpu-range-batch` / `-Crb` | Separate range for batch processing |
| `--cpu-strict-batch` | Strict placement for batch threads |

**Example**: `llama-server --cpu-range 0-7 --cpu-strict 1` pins inference to P-cores 0-7.

### 5.2 NUMA Support

- `ggml_numa_init()` reads `/sys/devices/system/node/` to enumerate NUMA nodes
- Uses `pthread_getaffinity_np()` / `sched_setaffinity()` for per-thread pinning
- `--numa distribute` distributes threads across NUMA nodes
- **Note**: `--numa` and `--cpu-mask/range/strict` are currently NOT compatible (as per llama.cpp developer comment in Discussion #9996). This will be fixed in future threading updates.

### 5.3 Internal Implementation

In `ggml-cpu.c`:
- Thread pool uses `cpu_set_t` on Linux for affinity masks
- `ggml_get_numa_affinity()` reads current thread's affinity via `pthread_getaffinity_np()`
- Hybrid CPU detection is NOT built into ggml -- it relies on user-supplied masks
- The threadpool does exclude E-cores from auto-detected thread count on hybrid CPUs (to prevent lockstep barrier slowdown), but explicit mask control is preferred

### 5.4 What Llamaste Should Do

Since Llamaste wraps llama-server via `fork()/execv()`, we can:
1. Detect hybrid topology at boot in supervisor
2. Build the `--cpu-range` or `--cpu-mask` argument dynamically
3. Pass it to llama-server on the execv command line
4. No llama.cpp source modification needed

---

## 6. AMD CPUs (EVO-X2) -- No Hybrid Topology

AMD desktop/mobile CPUs (Zen 3, Zen 4, Zen 5) use homogeneous core designs:
- All cores are identical (no P/E split)
- All cores support the same ISA (AVX2, AVX-512 on Zen 4+)
- CPUID leaf 0x1A returns 0 or is unsupported (no hybrid flag)

**Handling**: When hybrid detection returns "not hybrid," use all cores. No special treatment needed. The `--cpu-range` is simply `0-(N-1)` where N = total logical CPUs (or physical cores if HT disabled).

AMD Zen 4+ supports AVX-512 on ALL cores, so `GGML_NATIVE=ON` will use AVX-512 on AMD. This is safe and provides a significant speedup.

---

## 7. Homogeneous Intel (i5-1035G1 Ice Lake) -- No Hybrid

- Ice Lake (10th gen) predates hybrid architecture
- All 4 cores are identical (Sunny Cove microarchitecture)
- Supports AVX-512 on all cores (Ice Lake was the last client chip with AVX-512)
- CPUID leaf 0x1A: not supported or returns 0
- `/sys/devices/system/cpu/types/`: directory does not exist

**Handling**: Same as AMD -- use all cores. AVX-512 available on all cores (and `GGML_NATIVE=ON` will use it).

---

## 8. Runtime Detection Algorithm (C++)

### 8.1 Recommended Approach (3-tier fallback)

```
1. Try sysfs: read /sys/devices/system/cpu/types/intel_core/cpulist
   - If exists: parse CPU list -> these are P-cores
   - Read /sys/devices/system/cpu/types/intel_atom/cpulist -> E-cores
   - Result: hybrid=true, p_cores={list}, e_cores={list}

2. Try CPUID 0x1A (fallback if sysfs not available):
   - Check CPUID(0x07).EDX bit 15 (hybrid flag)
   - If hybrid: iterate each CPU, pin thread, call CPUID(0x1A)
   - EAX[31:24] == 0x40 -> P-core, == 0x20 -> E-core
   - Result: hybrid=true, p_cores={list}, e_cores={list}

3. Frequency heuristic (last resort):
   - Read /sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq
   - Cluster CPUs by max frequency
   - Highest cluster = P-cores, lowest = E-cores
   - Only use if there are exactly 2 distinct frequency clusters
   - Result: hybrid=maybe, p_cores={list}, e_cores={list}

4. Default (non-hybrid):
   - All cores treated equally
   - Use all physical cores (exclude HT siblings for llama.cpp)
   - Result: hybrid=false, all_cores={list}
```

### 8.2 HT Sibling Detection

Even on hybrid CPUs, P-cores have HT (2 threads per core). E-cores do NOT have HT (1 thread per core). For LLM inference, use one thread per physical core:
- Read `/sys/devices/system/cpu/cpuX/topology/core_cpus_list`
- For each unique `core_id`, pick only the first logical CPU

### 8.3 Arrow Lake HX Specifics (Core Ultra 9 275HX)

- 8 P-cores (Lion Cove) with HT = 16 logical CPUs
- 16 E-cores (Skymont) without HT = 16 logical CPUs
- Total: 32 logical CPUs, but optimal for LLM = 8 threads on P-cores only
- With HT on P-cores: maybe 10-12 threads (8 physical + a few HT) depending on workload

---

## 9. Safe Backward-Compatible Strategy for Llamaste

### Decision Matrix

| CPU Type | Detection | Thread Strategy | SIMD |
|----------|-----------|----------------|------|
| Intel hybrid (Alder/Raptor/Arrow Lake) | sysfs cpu_types or CPUID 0x1A | Pin to P-cores only, physical cores, no HT | AVX2 (AVX-512 disabled by platform) |
| Intel homogeneous (Ice Lake, older) | No hybrid flag, no cpu_types | All physical cores | AVX-512 or AVX2 depending on gen |
| AMD (Zen 3/4/5) | No hybrid flag | All physical cores | AVX2 or AVX-512 (Zen 4+) |
| Unknown/VM | Detection fails | All logical cores, n_threads = physical cores | Whatever CPUID reports |

### Implementation Plan

1. **Boot-time detection** in `supervisor.cpp` or `init.cpp`:
   - Run detection algorithm (Section 8.1) once at startup
   - Store results in a struct: `{bool hybrid, vector<int> p_cores, vector<int> e_cores, int recommended_threads}`

2. **Pass to llama-server** via command-line args:
   - Hybrid: `--cpu-range 0-7 --cpu-strict 1 -t 8` (P-cores only)
   - Homogeneous: `-t <physical_cores>` (default, no mask)

3. **Expose via API** (`/llamaste/system/info`):
   - Add `cpu_topology` field: `{type: "hybrid"|"homogeneous", p_cores: N, e_cores: N, threads_used: N}`
   - Allows web UI to show topology info in dashboard

4. **No GGML source changes needed**:
   - llama.cpp already supports `--cpu-range` and `--cpu-strict`
   - We just need to compute the right values and pass them

### Performance Expectations

- **Arrow Lake HX (8P+16E, pinned to 8P)**: ~2-3x faster than using all 32 threads
- **i5-1035G1 (4 cores, no hybrid)**: No change, already optimal
- **AMD EVO-X2**: No change, all cores identical

---

## 10. References

- Intel 12th Gen Gamedev Guide: https://www.intel.com/content/www/us/en/developer/articles/guide/12th-gen-intel-core-processor-gamedev-guide.html
- Intel HybridDetect (reference C++ implementation): https://github.com/GameTechDev/HybridDetect
- llama.cpp Discussion #572 (3x perf with P-cores only): https://github.com/ggml-org/llama.cpp/discussions/572
- llama.cpp Discussion #9996 (--cpu-range usage): https://github.com/ggml-org/llama.cpp/discussions/9996
- Linux kernel cputopology sysfs docs: https://www.kernel.org/doc/html/latest/admin-guide/cputopology.html
- Linux hybrid topology patches (Phoronix, June 2024): https://www.phoronix.com/news/Intel-Hybrid-Topology-Mitigate
- Arrow Lake lacks AVX-512 (Tom's Hardware): https://www.tomshardware.com/pc-components/cpus/core-ultra-k-prototype-appears-on-cpu-z-uses-intel-4-process-node-and-hits-5-ghz-clocks-lacks-avx-512-support
- Arrow Lake HX core architecture (Phoronix): https://www.phoronix.com/news/Intel-ARL-H-3-Core-Types
- Intel Skymont E-core details (Chips and Cheese): https://chipsandcheese.com/p/intel-details-skymont
- LWN.net hybrid scheduling: https://lwn.net/Articles/909611/
