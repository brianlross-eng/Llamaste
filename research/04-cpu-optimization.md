# CPU Feature Detection and Optimization for LLM Inference

## Detection Mechanisms

### /proc/cpuinfo
```bash
# Detect x86 SIMD features
grep -m1 'flags' /proc/cpuinfo | grep -o 'avx\|avx2\|avx512f\|avx512_vnni\|avx512_bf16\|amx_tile\|amx_int8\|fma' | sort -u

# ARM features
grep -m1 'Features' /proc/cpuinfo | grep -o 'neon\|asimd\|sve\|sve2\|bf16\|i8mm' | sort -u

# Physical vs logical cores (SMT detection)
PHYS=$(grep 'core id' /proc/cpuinfo | sort -u | wc -l)
LOGICAL=$(grep -c '^processor' /proc/cpuinfo)
```

### lscpu
```bash
lscpu                    # Structured overview
lscpu -C                 # Cache topology
lscpu -p=CPU,CORE,NODE   # CPU-to-NUMA mapping
```

## x86-64 SIMD Hierarchy for Inference

| Feature | Width | Year | Impact |
|---------|-------|------|--------|
| SSE4.2 | 128-bit | 2008 | Baseline |
| AVX | 256-bit | 2011 | 1.3x float throughput |
| AVX2+FMA | 256-bit | 2013 | 2-2.5x (FMA is key for dot products) |
| AVX-512 | 512-bit | 2017 | 2.5-4x (with frequency penalty on older Intel) |
| AVX-512 VNNI | 512-bit | 2019 | Huge INT8 throughput for quantized models |
| AMX | Tile-based | 2023 | 4-6x for batch INT8/BF16 |

### ARM SIMD
- **NEON/ASIMD** (128-bit): Mandatory on all ARMv8-A
- **Dot product (sdot/udot)**: Cortex-A76+, all Apple Silicon
- **SVE**: Variable length (128-2048 bit), Graviton3+
- **I8MM**: ARMv8.6, int8 matrix multiply

## Huge Pages for Model Loading

### Why It Matters
7B Q4_0 model (~3.8 GB) = ~1M page table entries with 4KB pages. TLB can only cache ~1-2K entries. Every miss costs 20-100ns.

With 2MB huge pages: only ~1,900 entries. With 1GB pages: 4 entries.

### Transparent Huge Pages (THP)
```bash
# Set mode
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
echo defer+madvise > /sys/kernel/mm/transparent_hugepage/defrag
```

`madvise` mode preferred over `always` to avoid khugepaged latency spikes.

### Explicit Huge Pages
```bash
# Allocate 2MB huge pages
echo 4096 > /proc/sys/vm/nr_hugepages   # 4096 x 2MB = 8GB

# 1GB pages (must be at boot time)
# Kernel cmdline: hugepagesz=1G hugepages=40 default_hugepagesz=2M

# Mount hugetlbfs
mount -t hugetlbfs nodev /mnt/hugepages
```

## CPU Governor Settings

```bash
# Set performance governor (recommended for dedicated inference)
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$gov"
done

# Enable turbo boost
echo 0 > /sys/devices/system/cpu/intel_pstate/no_turbo    # Intel
echo 1 > /sys/devices/system/cpu/cpufreq/boost             # AMD
```

| Governor | Latency | Best For |
|----------|---------|----------|
| performance | Zero | Dedicated servers |
| schedutil | 1-4ms | Shared/laptop systems |
| ondemand | 10-30ms | Not recommended |
| powersave | N/A | Never for inference |

### C-State Management
```bash
# Disable deep C-states (reduce wake latency)
for state_dir in /sys/devices/system/cpu/cpu*/cpuidle/state*/; do
    state=$(basename "$state_dir")
    [ "$state" != "state0" ] && echo 1 > "${state_dir}disable"
done
```

### Laptop Thermal Management
```bash
# Cap power to avoid throttling
echo 25000000 > /sys/class/powercap/intel-rapl:0/constraint_0_power_limit_uw  # 25W

# Or cap frequency
cpupower frequency-set -u 3000000  # 3.0 GHz

# Use fewer cores at higher frequency (often faster for inference)
HALF_CORES=$((PHYS_CORES / 2))
./llama-cli -m model.gguf -t $HALF_CORES
```

## NUMA Topology Awareness

```bash
# Check topology
numactl --hardware

# Single-node binding (model fits in one node)
numactl --cpunodebind=0 --membind=0 ./llama-cli -m model.gguf -t 8

# Interleaved allocation (model spans nodes)
numactl --interleave=all ./llama-cli -m model.gguf -t 32

# llama.cpp built-in NUMA
./llama-cli -m model.gguf --numa distribute

# Disable automatic NUMA balancing
echo 0 > /proc/sys/kernel/numa_balancing
```

## Memory Bandwidth: THE Bottleneck

Token generation speed ≈ `memory_bandwidth / model_size`

DDR5-4800 dual-channel (~76 GB/s): 76 / 3.8 ≈ 20 tok/s theoretical ceiling for 7B Q4_0

### Maximizing Bandwidth
1. **Populate all memory channels** (single vs dual = 2x difference)
2. **Use fastest supported memory frequency**
3. **Use physical cores only, disable SMT** (`echo off > /sys/devices/system/cpu/smt/control`)
4. **Use mlock** to prevent swapping
5. **Lower quantization = fewer bytes to read = faster generation**

### Kernel Tuning
```bash
sysctl -w vm.swappiness=1
echo 4096 > /sys/block/nvme0n1/queue/read_ahead_kb
sysctl -w vm.zone_reclaim_mode=0
echo 0 > /proc/sys/kernel/nmi_watchdog
```

## Hardware Profiling Script

```bash
#!/bin/sh
# Detect CPU features, RAM, recommend model/quantization
FLAGS=$(grep -m1 'flags' /proc/cpuinfo)
PHYS_CORES=$(lscpu -p=CORE | grep -v '#' | sort -u | wc -l)
TOTAL_RAM_KB=$(grep MemTotal /proc/meminfo | awk '{print $2}')
TOTAL_RAM_GB=$((TOTAL_RAM_KB / 1024 / 1024))

# Recommend model based on RAM
if [ "$TOTAL_RAM_GB" -ge 28 ]; then
    MODEL="32B"; QUANT="Q4_K_M"
elif [ "$TOTAL_RAM_GB" -ge 12 ]; then
    MODEL="14B"; QUANT="Q4_K_M"
elif [ "$TOTAL_RAM_GB" -ge 6 ]; then
    MODEL="7B"; QUANT="Q4_K_M"
elif [ "$TOTAL_RAM_GB" -ge 3 ]; then
    MODEL="3B"; QUANT="Q4_K_M"
else
    MODEL="1.5B"; QUANT="Q4_K_M"
fi

echo "RAM: ${TOTAL_RAM_GB}GB | Cores: ${PHYS_CORES} | Recommended: ${MODEL} ${QUANT}"
echo "Threads: $((PHYS_CORES - 1)) generation, ${PHYS_CORES} batch"
```

## Intel vs AMD Differences

| Aspect | Intel | AMD |
|--------|-------|-----|
| AVX-512 | Native 512-bit units | Double-pumped 256-bit (Zen 4) |
| AMX | Sapphire Rapids+ | Not available |
| Memory channels | 8 (Xeon 6) | 12 (EPYC 9005) |
| Consumer AVX-512 | Disabled (12th-14th gen) | Available (Zen 4) |
| Best for bandwidth | Less bandwidth per socket | 50% more bandwidth per socket |

**For CPU inference of quantized models, AMD EPYC has the best price-to-performance due to superior memory bandwidth.**
