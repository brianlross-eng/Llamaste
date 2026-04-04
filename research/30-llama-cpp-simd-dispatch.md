# Research: llama.cpp SIMD Dispatch (AVX2, AVX-512, AMX)

**Date**: 2026-04-03
**Purpose**: Determine how to build a single Llamaste binary/image that runs optimally on both AVX2-only (i5-1035G1 Ice Lake) and AVX-512/AMX (Core Ultra 9 275HX Arrow Lake) machines.

---

## 1. What Does GGML_NATIVE=ON Actually Do?

`GGML_NATIVE=ON` passes `-march=native` to GCC/Clang. This tells the compiler to detect the **build machine's** CPU features and enable ALL instruction sets it supports.

- If built on an AVX2-only machine: enables up to AVX2, FMA, F16C, BMI2
- If built on an AVX-512 machine: enables AVX-512F/CD/VL/DQ/BW and potentially VNNI/VBMI/BF16
- If built on an AMX machine: may enable AMX_TILE/AMX_INT8/AMX_BF16

**Critical**: The resulting binary will **SIGILL (illegal instruction) crash** on machines that lack the features used at compile time. A binary built with `-march=native` on an AVX-512 machine CANNOT run on an AVX2-only machine.

**Also critical**: `GGML_NATIVE` is **incompatible with `GGML_BACKEND_DL`** (dynamic backend loading). CMake explicitly errors: "GGML_NATIVE is not compatible with GGML_BACKEND_DL, consider using GGML_CPU_ALL_VARIANTS".

---

## 2. Explicit CMake Feature Flags

When `GGML_NATIVE=OFF` (the default), you can manually select features:

### SIMD Instruction Sets
| Flag | Compiler Flags (GCC) | Description |
|------|---------------------|-------------|
| `GGML_SSE42` | `-msse4.2` | SSE 4.2 (baseline x86-64-v2) |
| `GGML_AVX` | `-mavx` | AVX (Sandy Bridge+) |
| `GGML_F16C` | `-mf16c` | Half-precision float conversion |
| `GGML_FMA` | `-mfma` | Fused multiply-add |
| `GGML_BMI2` | `-mbmi2` | Bit manipulation |
| `GGML_AVX2` | `-mavx2` | 256-bit integer SIMD (Haswell+) |
| `GGML_AVX_VNNI` | `-mavxvnni` | AVX-VNNI (Alder Lake+, no AVX-512 needed) |
| `GGML_AVX512` | `-mavx512f -mavx512cd -mavx512vl -mavx512dq -mavx512bw` | AVX-512 foundation (Skylake-X+) |
| `GGML_AVX512_VBMI` | `-mavx512vbmi` | Variable byte/bit manipulation (Ice Lake+) |
| `GGML_AVX512_VNNI` | `-mavx512vnni` | Vector neural network instructions (Ice Lake+) |
| `GGML_AVX512_BF16` | `-mavx512bf16` | BFloat16 support (Cooper Lake+) |

### Intel AMX (Advanced Matrix Extensions)
| Flag | Compiler Flags (GCC) | Description |
|------|---------------------|-------------|
| `GGML_AMX_TILE` | `-mamx-tile` | AMX tile configuration |
| `GGML_AMX_INT8` | `-mamx-int8` | AMX INT8 matrix multiply |
| `GGML_AMX_BF16` | `-mamx-bf16` | AMX BF16 matrix multiply |

**AMX note**: MSVC does NOT support AMX compilation. Must use GCC or Clang. AMX is integrated into the CPU backend (was separate ggml-amx, moved to ggml-cpu in PR #10570).

---

## 3. Runtime CPU Feature Detection and Dispatch

llama.cpp has **two dispatch mechanisms**:

### A. Compile-Time Only (Default, Single Binary)
With a standard build (no `GGML_BACKEND_DL`), feature selection is **compile-time only**. The binary uses whatever instruction set it was compiled for. No runtime detection. No fallback.

### B. Runtime Multi-Variant Dispatch (`GGML_BACKEND_DL` + `GGML_CPU_ALL_VARIANTS`)
This is the **key mechanism for universal binaries**. When enabled:

1. Multiple CPU backend shared libraries are built, each compiled for a different ISA level
2. Each library contains a **score function** (`ggml_backend_cpu_x86_score()`) that uses CPUID to check if the host CPU supports all required features
3. At runtime, the backend registry loads all variants and selects the one with the **highest score**
4. If a required feature is missing, score returns 0 (variant rejected)

**The pre-defined x86 variants** (from `ggml/src/CMakeLists.txt`):

```cmake
ggml_add_cpu_backend_variant(x64)                                                    # baseline x86-64
ggml_add_cpu_backend_variant(sse42        SSE42)                                     # + SSE4.2
ggml_add_cpu_backend_variant(sandybridge  SSE42 AVX)                                 # + AVX
ggml_add_cpu_backend_variant(haswell      SSE42 AVX F16C AVX2 BMI2 FMA)              # + AVX2
ggml_add_cpu_backend_variant(skylakex     SSE42 AVX F16C AVX2 BMI2 FMA AVX512)       # + AVX-512
ggml_add_cpu_backend_variant(icelake      SSE42 AVX F16C AVX2 BMI2 FMA AVX512 AVX512_VBMI AVX512_VNNI) # + VNNI/VBMI
ggml_add_cpu_backend_variant(alderlake    SSE42 AVX F16C AVX2 BMI2 FMA AVX_VNNI)     # AVX2 + VNNI (no 512)
```

**Score calculation** (from `arch/x86/cpu-feats.cpp`):
- Each feature adds a power-of-2 to the score (FMA=1, F16C=2, SSE42=4, BMI2=8, AVX=16, AVX2=32, AVX_VNNI=64, AVX512=128, AVX512_VBMI=256, AVX512_BF16=512, AVX512_VNNI=1024, AMX_INT8=2048)
- Missing required feature -> score = 0 (variant rejected)
- Highest score wins

**Feature detection code** (`cpu-feats.cpp`) uses CPUID:
- Compiled with `-fno-lto` to prevent cross-module optimization from inlining arch-specific instructions
- The score function itself contains NO arch-specific instructions (safe to load on any CPU)
- Actual arch-specific code is in the backend .so, only loaded if score > 0

---

## 4. Performance: AVX2 vs AVX-512 vs AMX

### AVX2 vs AVX-512
Approximate performance differences for LLM inference:

| Source | Metric | AVX2 | AVX-512 | Improvement |
|--------|--------|------|---------|-------------|
| Cortensor/OpenMetal | LLaMA-3 3.2B INT8 | ~28 t/s | ~57 t/s (w/ AMX) | ~2x |
| AMD EPYC Zen4 | General inference | baseline | "significant uplift" | est. 1.3-1.8x |
| Justine Tunney (llamafile) | Matrix multiply | baseline | 1.5-2.8x faster | varies by op |

**Key points**:
- AVX-512 benefits are most visible in **prompt processing** (batch matrix multiply), less in token generation
- VNNI (Vector Neural Network Instructions) specifically accelerates INT8 quantized inference
- AVX-512 BF16 enables native bfloat16 operations (significant for certain quant types)
- On desktop CPUs (non-Xeon), AVX-512 may cause **thermal throttling** (downclocking), reducing effective gains

### AMX Performance
- **AMX INT8 is the big win**: Dedicated matrix multiply tiles (1024-bit) vs AVX-512 (512-bit)
- OpenMetal benchmarks: AMX on = ~57 t/s vs AMX off = ~28 t/s on Xeon (LLaMA-3 3.2B)
- Presidio/AWS: ~100 t/s with AMX vs ~25 t/s baseline on generic prompts
- AMX is ~40% faster than llamafile sgemm at pp512 (prompt processing)
- **Supported quantization types for AMX**: Q4_1, Q8_0, Q4_K, Q5_K, Q6_K, IQ4_XS

---

## 5. Building a Universal Binary for Llamaste

### The Problem
Llamaste currently uses `GGML_NATIVE=ON`, producing a binary optimized for the BUILD machine. Since Buildroot builds in WSL2 (which passes through the host CPU flags via VirtualBox), the binary works on the build host but may crash on different hardware.

### Solution: `GGML_BACKEND_DL=ON` + `GGML_CPU_ALL_VARIANTS=ON`

This builds **multiple .so backend libraries**, one per CPU tier:
- `ggml-cpu-x64.so` — works on any x86-64
- `ggml-cpu-haswell.so` — uses AVX2 (i5-1035G1 would use this)
- `ggml-cpu-icelake.so` — uses AVX-512 + VNNI/VBMI (if supported)
- `ggml-cpu-alderlake.so` — uses AVX2 + AVX-VNNI (hybrid cores)
- etc.

At runtime, the highest-scoring variant is selected automatically.

### Implications for Llamaste
1. **Cannot use static linking for the CPU backend** — `GGML_BACKEND_DL` requires shared libraries (.so files)
2. The main binary links against ggml core; CPU backends are loaded via `dlopen()` at runtime
3. Need to ship all variant .so files in the squashfs
4. The backend .so files go in a search path (default: alongside the binary, or `GGML_BACKEND_DIR` env var)
5. Total binary size increases (each variant is a separate .so, ~2-5MB each, ~7 variants = ~20-35MB total)

### Alternative: Fat Binary Without Dynamic Loading
If dynamic loading is undesirable (PID 1, minimal system):
- Build with explicit flags: `-DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON` (covers both machines)
- Do NOT enable AVX-512 or AMX (would crash on VivoBook)
- Accept losing AVX-512/AMX performance on the 275HX
- This is what `GGML_NATIVE=ON` effectively does when built on the VivoBook

### Recommended Approach for Llamaste
Option A (Simple, current approach with guard rails):
- Keep `GGML_NATIVE=ON` but always build on the **lowest-common-denominator** machine
- i5-1035G1 supports: SSE4.2, AVX, AVX2, FMA, F16C, BMI2 (but NOT AVX-512)
- Binary works on both machines; 275HX just doesn't use AVX-512/AMX

Option B (Optimal, more complex):
- Switch to `GGML_BACKEND_DL=ON` + `GGML_CPU_ALL_VARIANTS=ON`
- Ship all variant .so files in squashfs
- Runtime auto-selects best variant per machine
- Requires changes to Buildroot packaging (install .so files, set `GGML_BACKEND_DIR`)
- Need to verify `dlopen()` works correctly in PID 1 context (should be fine with musl)

---

## 6. Intel AMX on Arrow Lake (Core Ultra 9 275HX)

### Does Arrow Lake Have AMX?
Arrow Lake (desktop/mobile) **does support AMX** (AMX-INT8, AMX-BF16, AMX-TILE). This is confirmed by CPUID feature flags. However:
- Arrow Lake **P-cores** have AMX
- Arrow Lake **E-cores** do NOT have AMX
- The OS/kernel must handle tile configuration via XSAVE/XRSTOR
- Linux kernel 5.16+ has AMX support (`CONFIG_X86_INTEL_AMX=y` or auto via XSAVE)

### llama.cpp AMX Support Status
- AMX is fully integrated into the CPU backend (PR #10570 merged AMX into ggml-cpu)
- Supported quant types: Q4_1, Q8_0, Q4_K, Q5_K, Q6_K, IQ4_XS
- Score function checks AMX_INT8 via CPUID (`f_7_edx[25]`)
- **MSVC cannot compile AMX code** — must use GCC or Clang (fine for Llamaste/Buildroot)
- `GGML_CPU_ALL_VARIANTS` does NOT currently include an AMX-specific variant, but the `icelake` variant could be extended, or a custom variant added

### Adding AMX to Llamaste
To add an AMX variant, you would add to the variant list:
```cmake
ggml_add_cpu_backend_variant(arrowlake SSE42 AVX F16C AVX2 BMI2 FMA AVX512 AVX512_VBMI AVX512_VNNI AMX_TILE AMX_INT8 AMX_BF16)
```
This requires Arrow Lake to have AVX-512 (it does, via APX/AVX10 compatibility). The score function already handles AMX_INT8 scoring (bit 11, value 2048).

---

## 7. CPUID-Based Runtime Dispatch in ggml

Yes, ggml has CPUID-based runtime dispatch, implemented in `ggml/src/ggml-cpu/arch/x86/cpu-feats.cpp`.

### How It Works
1. `cpuid_x86` struct uses `__cpuid` / `__cpuidex` (MSVC) or inline asm `cpuid` (GCC/Clang)
2. Checks EAX/EBX/ECX/EDX for specific feature bits per Intel SDM Vol. 2 (doc 325383)
3. Detected features: SSE3, SSE4.2, AVX, AVX2, FMA, F16C, BMI2, AVX-512 (F/CD/VL/DQ/BW/VBMI/VNNI/BF16), AMX (TILE/INT8/FP16/BF16)
4. `ggml_backend_cpu_x86_score()` returns a bitmask score; highest score among loadable variants wins
5. Feature check code is compiled with `-fno-lto` to prevent LTO from inlining arch-specific instructions into the safe score function

### Safety Mechanism
- The score function itself uses **no SIMD instructions** — only CPUID and integer math
- LTO is disabled for the feature check compilation unit specifically to prevent the linker from pulling in AVX-512 instructions from other TUs
- If a variant's .so is loaded but the score function returns 0, the variant is never initialized (no SIGILL risk)

---

## 8. Summary & Recommendation for Llamaste

### Current State
- `GGML_NATIVE=ON` in `llama-server.mk` — binary uses whatever the build machine supports
- Built in WSL2 on host with i5-1035G1 → AVX2 max → works on both VivoBook and 275HX
- 275HX doesn't get AVX-512 or AMX acceleration

### Short-Term (Safe)
Keep `GGML_NATIVE=ON`, ensure builds always happen on AVX2-level machine. No AVX-512/AMX acceleration, but universal compatibility.

### Medium-Term (Optimal)
Switch to `GGML_BACKEND_DL=ON` + `GGML_CPU_ALL_VARIANTS=ON`:
1. Modify `llama-server.mk` to set `GGML_NATIVE=OFF`, `GGML_BACKEND_DL=ON`, `GGML_CPU_ALL_VARIANTS=ON`
2. Install all `ggml-cpu-*.so` files to `/opt/llamaste/backends/`
3. Set `GGML_BACKEND_DIR=/opt/llamaste/backends` in the environment
4. ~20-35MB additional squashfs size for all variants
5. Runtime auto-selects optimal variant per CPU

### Long-Term (Maximum Performance)
Add custom AMX variant for Arrow Lake/Sapphire Rapids:
```cmake
ggml_add_cpu_backend_variant(arrowlake SSE42 AVX F16C AVX2 BMI2 FMA AVX512 AVX512_VBMI AVX512_VNNI AMX_TILE AMX_INT8 AMX_BF16)
```

---

## Sources
- [llama.cpp CMakeLists.txt (ggml-cpu)](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-cpu/CMakeLists.txt)
- [llama.cpp build docs](https://github.com/ggml-org/llama.cpp/blob/master/docs/build.md)
- [x86 cpu-feats.cpp](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-cpu/arch/x86/cpu-feats.cpp)
- [ggml/src/CMakeLists.txt (variant definitions)](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/CMakeLists.txt)
- [GitHub Issue #17966 - CPU ALL_VARIANTS](https://github.com/ggml-org/llama.cpp/issues/17966)
- [GitHub Discussion #12166 - AMX build](https://github.com/ggml-org/llama.cpp/discussions/12166)
- [GitHub PR #10570 - AMX moved to CPU backend](https://github.com/ggml-org/llama.cpp/pull/10570)
- [Cortensor CPU Instruction Sets for LLM Inference](https://docs.cortensor.network/technical-architecture/ai-inference/cpu-instruction-sets-for-llm-inference-avx-amx-sme-vs-gpus)
- [DeepWiki llama.cpp Building](https://deepwiki.com/ggml-org/llama.cpp/2.1-installation-and-building)
- [Cloud Run SIGILL blog post](https://haitmg.pl/blog/cloud-run-sigill-avx512-llama-cpp/)
