# Hardware Optimization Design — Llamaste v0.3.0

**Date**: 2026-04-03
**Goal**: Maximize inference speed on all x86-64 hardware without breaking backward compatibility
**Trigger**: 72B Q4_K_M at 1.7 tok/s on Core Ultra 9 275HX (24 cores, 64GB DDR5) — unacceptable

## The Hard Truth: Memory Bandwidth is the Ceiling

For token generation (TG), the bottleneck is **memory bandwidth, not compute**:

```
tok/s = memory_bandwidth_GB_s / model_size_GB
```

| Hardware | RAM | Bandwidth | 72B Q4_K_M (43GB) | 14B Q4_K_M (8.7GB) | 3B Q4_K_M (2GB) |
|----------|-----|-----------|-------------------|---------------------|-----------------|
| 275HX (DDR5-6400 dual) | 64GB | ~102 GB/s | ~2.4 tok/s | ~11.7 tok/s | ~51 tok/s |
| VivoBook i5 (DDR4-3200 dual) | 36GB | ~51 GB/s | N/A (won't fit) | ~5.9 tok/s | ~25 tok/s |
| VivoBook i5 (DDR4 single!) | 36GB | ~25 GB/s | N/A | ~2.9 tok/s | ~12 tok/s |

**Key insight**: 1.7 tok/s on 275HX for 72B is actually ~71% of the theoretical max (2.4). The remaining gap is likely:
- E-cores dragging down barrier sync (biggest factor)
- Thread count misconfigured
- Missing AVX-512 (if Ice Lake variant is loaded instead)
- Suboptimal KV cache / batch settings

**VivoBook anomaly**: 14B at 2.0-2.2 tok/s when theoretical dual-channel is 5.9 suggests **single-channel RAM**. Need `dmidecode -t 17` to confirm.

## Three Changes, Maximum Impact (Backward Compatible)

### Change 1: Dynamic SIMD Backend Loading (BIGGEST WIN)

**Current**: `GGML_NATIVE=ON` compiles with `-march=native` on the Buildroot VM. The VM passes through host AVX2 but NOT AVX-512. Result: every machine runs AVX2 code, even 275HX which has AVX-512.

**Fix**: Switch to `GGML_BACKEND_DL=ON` + `GGML_CPU_ALL_VARIANTS=ON`

This builds 7 separate `.so` backend libraries, each compiled for a different CPU microarchitecture:

| Variant | ISA | Machines |
|---------|-----|----------|
| x64 | SSE2 baseline | Ancient CPUs, fallback |
| sse42 | SSE4.2 | Old Core 2 / early Core i |
| sandybridge | AVX | Sandy/Ivy Bridge |
| haswell | AVX2 + FMA | **VivoBook i5-1035G1**, most 2014+ CPUs |
| skylakex | AVX-512F | Xeon Skylake, some HEDT |
| icelake | AVX-512 + VNNI | **Ice Lake** (but VivoBook is mobile, no AVX-512 in hybrid mode) |
| alderlake | AVX2 + VNNI (no AVX-512) | Alder/Raptor/Arrow Lake hybrid |

At runtime, ggml loads CPUID, scores each variant, picks the best one. Zero user configuration.

**Impact**: On machines where AVX-512 is actually available (non-hybrid Ice Lake desktop, Xeon), this gives 1.3-1.8x speedup. On hybrid Arrow Lake (275HX), it correctly selects alderlake variant (AVX2 + VNNI) avoiding E-core SIGILL crashes.

**Backward compat**: Perfect. Old CPUs get haswell or sandybridge variant automatically. No binary crashes.

**Build change**: In `llama-server.mk`:
```makefile
# Remove:
LLAMA_SERVER_CONF_OPTS += -DGGML_NATIVE=ON

# Add:
LLAMA_SERVER_CONF_OPTS += -DGGML_NATIVE=OFF
LLAMA_SERVER_CONF_OPTS += -DGGML_BACKEND_DL=ON
LLAMA_SERVER_CONF_OPTS += -DGGML_CPU_ALL_VARIANTS=ON
```

Post-install copies `.so` files to `/opt/llamaste/backends/`.
At startup, set `GGML_BACKEND_DIR=/opt/llamaste/backends/` env var before execv.

### Change 2: P-Core Detection and Thread Pinning (BIG WIN on Hybrid CPUs)

**Problem**: Arrow Lake-HX 275HX has 8 P-cores (16 threads) + 16 E-cores. Default `hardware_concurrency()` = 40. LLM inference uses all 40 threads, but E-cores are 2-3x slower → barrier sync means every batch waits for the slowest E-core thread. Community benchmarks show **2-3x slowdown** from E-core inclusion.

**Fix**: Detect hybrid topology at boot, pass `--threads <P_core_count>` and `--cpu-range <P_core_list>` to llama-server.

Detection strategy (3-tier fallback):
1. **sysfs** (kernel 6.12+): `/sys/devices/system/cpu/types/intel_core/cpulist` → P-core list
2. **CPUID leaf 0x1A**: Query each CPU's hybrid type (0x40 = P-core, 0x20 = E-core)
3. **cpufreq heuristic**: Cluster by `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` — highest cluster = P-cores

For non-hybrid CPUs (AMD, Ice Lake): all cores are equal, use all physical cores (no HT).

**llama-server already supports this**: `--cpu-range 0-15` and `--cpu-strict 1` are existing flags. No llama.cpp source changes needed.

**Impact on 275HX**: From 40 threads (with E-core drag) to 8 P-core threads (16 with HT, but HT hurts TG). Expected: threads alone could give 1.5-2x improvement on TG.

**Impact on VivoBook**: No change — homogeneous CPU, all 4 cores used (8 threads for batch, 4 for TG).

**Impact on AMD (EVO-X2)**: No change — homogeneous CPU, all cores used.

**Implementation**: ~100 LOC in supervisor.cpp or a new `cpu_topology.h` header. Runs once at boot, results passed to llama-server args.

### Change 3: Optimal Thread Count and Server Flags

**Current**: llama-server likely uses `hardware_concurrency()` default for both `--threads` and `--threads-batch`.

**Fix**: Set threads based on detected topology:

| Parameter | Purpose | Optimal Setting |
|-----------|---------|-----------------|
| `--threads` | Token generation (memory-bound) | Physical P-cores only (no HT) |
| `--threads-batch` | Prompt processing (compute-bound) | All P-core threads (with HT) |
| `--flash-attn` | Flash attention | Always enable (free perf) |
| `--ctx-size` | Context window | Auto-size based on RAM |
| `--batch-size` | Prompt batch | 2048 (default is fine) |

**Expected settings per machine**:

| Machine | --threads | --threads-batch | --cpu-range |
|---------|-----------|-----------------|-------------|
| 275HX (8P+16E) | 8 | 16 | 0-15 (P-cores) |
| VivoBook i5 (4C/8T) | 4 | 8 | (all) |
| EVO-X2 AMD (varies) | N_physical | N_logical | (all) |

## What NOT To Do (Complexity Not Worth It)

### Skip: Intel iGPU SYCL Offload
- 64 EUs on 275HX is below recommended 80 EU minimum
- oneAPI build adds +500MB runtime, DPC++ compiler, massive build complexity
- Only ~21% improvement over CPU (benchmarked on similar iGPU)
- Breaks the "single simple binary" philosophy

### Skip: Intel NPU Offload
- OpenVINO NPU backend is "work in progress"
- Only supports Q4_0 (not Q4_K_M), 1024 token limit, single session
- 13 TOPS NPU is irrelevant for 72B model

### Skip: Vulkan GPU Backend (for now)
- Intel iGPU Vulkan compute has known crash bugs
- Good future option when users have discrete Arc GPUs
- Lower priority than CPU optimization

### Skip: AMX (for now)
- Arrow Lake P-cores support AMX, but llama.cpp's ALL_VARIANTS doesn't include an AMX variant yet
- Would need custom variant added to ggml build
- AVX-512 VNNI on icelake variant already captures most of the benefit
- Revisit when llama.cpp adds AMX to standard variants
- NOTE: Arrow Lake HYBRID disables AVX-512 at platform level, so AMX may be the only advanced ISA available on P-cores. Worth monitoring.

## Benchmark Endpoint

Add `/llamaste/benchmark` API endpoint:
- Runs a standard 200-token prompt on idle server
- Reports: tok/s (TG), tok/s (PP), CPU model, detected features, thread config, RAM info, model loaded
- Caches result (re-run with `?force=true`)
- Uses llama-server's existing `timings` from `/completion` response
- ~100 LOC in a new `tools_benchmark.cpp`

## Implementation Order

1. **Benchmark endpoint** (measure before optimizing — know the baseline)
2. **CPU topology detection** (P-core/E-core, thread count, CPUID features)
3. **Thread tuning** (pass optimal --threads, --threads-batch, --cpu-range to llama-server)
4. **Dynamic SIMD backends** (GGML_CPU_ALL_VARIANTS build change + backend dir)
5. **Memory diagnostics** (detect single vs dual channel, warn user)

Steps 1-3 are pure C++ changes in Llamaste code.
Step 4 is a Buildroot build configuration change.
Step 5 is informational (boot log + benchmark output).

## Backward Compatibility Guarantee

| Feature | Old CPU (Haswell) | VivoBook (Ice Lake) | EVO-X2 (AMD) | 275HX (Arrow Lake) |
|---------|-------------------|---------------------|---------------|---------------------|
| SIMD variant | haswell.so | haswell.so* | haswell.so | alderlake.so |
| Thread pinning | All cores | All 4 cores | All cores | P-cores only |
| Thread count | N_physical | 4 / 8 | N_physical / N_logical | 8 / 16 |
| Flash attn | Yes | Yes | Yes | Yes |
| Crash risk | None | None | None | None |

*Ice Lake mobile (VivoBook) may get icelake variant if AVX-512 is exposed. Needs testing.

## Expected Performance After Optimization

| Machine | Model | Current | After Changes | Bottleneck |
|---------|-------|---------|---------------|------------|
| 275HX | 72B Q4_K_M | 1.7 tok/s | ~2.0-2.4 tok/s | DDR5 bandwidth ceiling |
| 275HX | 14B Q4_K_M | ? | ~10-12 tok/s | DDR5 bandwidth |
| 275HX | 3B Q4_K_M | ? | ~30-50 tok/s | Compute (will benefit from VNNI) |
| VivoBook | 14B Q4_K_M | 2.2 tok/s | ~3-6 tok/s* | DDR4 bandwidth (*if dual-channel) |
| VivoBook | 3B Q4_K_M | 14 tok/s | ~20-25 tok/s | DDR4 bandwidth |

The 72B on 275HX has a hard ceiling around 2.4 tok/s due to memory bandwidth. The real wins are:
- **Smaller models get much faster** (3B: potentially 30-50 tok/s on 275HX)
- **14B becomes interactive** (~10-12 tok/s on 275HX)
- **E-core drag eliminated** on all hybrid CPUs
- **Automatic best-SIMD** on every machine with zero configuration
