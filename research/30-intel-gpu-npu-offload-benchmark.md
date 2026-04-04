# Research 30: Intel GPU/NPU Offload & Benchmark Endpoint Design

**Date**: 2026-04-03
**Status**: Research complete — recommendations for Llamaste integration

---

## 1. Intel Arc iGPU on Arrow Lake-HX (275HX)

### Hardware Facts
- **275HX has an iGPU**: Intel Graphics based on Xe-LPG architecture
- **4 Xe cores, 64 Execution Units (EUs)**, clocked 300–1900 MHz
- **Manufactured on TSMC N5P** (separate tile from CPU's N3B)
- **Shared system memory** — no dedicated VRAM; uses DDR5-6400 dual-channel
- **13 TOPS (Int8) from NPU alone**, 36 TOPS total (CPU+GPU+NPU combined)

### llama.cpp SYCL Backend Support
- **YES — officially supported**. The SYCL docs explicitly list "built-in Arc GPU in Arrow Lake" as verified.
- Benchmarks show ARL-H (Arrow Lake-H, same iGPU tier) at **14→17 tok/s** on llama-2-7b Q4_0 after MUL_MAT optimization (2025.2 update). This is comparable to CPU-only AVX2 performance on the 275HX.
- **< 80 EUs warning**: The docs note iGPUs with fewer than 80 EUs may be "too slow for practical use." The 275HX has only 64 EUs — right at the edge.

### Build Complexity for SYCL
**HIGH — not suitable for Llamaste's single-binary approach.**

Dependencies required:
- Intel oneAPI Base Toolkit OR Intel Deep Learning Essentials (~2–5 GB installed)
- Intel compute runtime (NEO) — OpenCL + Level Zero drivers
- oneMKL, oneDNN, oneDPL libraries
- DPC++ compiler (icpx) — separate from GCC/Clang
- OpenCL ICD loader + headers

Build steps:
```
source /opt/intel/oneapi/setvars.sh
cmake -B build -DGGML_SYCL=ON -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx
```

**Impact on Llamaste**: Would require Buildroot to cross-compile with Intel's DPC++ compiler, add ~500MB of runtime libraries to the image, and break the single-compiler-toolchain simplicity. The oneAPI stack is designed for full Linux distributions, not minimal embedded systems.

### Verdict: NOT WORTH IT for Llamaste
- 64 EUs is below the recommended threshold
- Performance gain over AVX2 CPU would be marginal (14→17 tok/s ≈ +21%)
- Build complexity is enormous for a Buildroot system
- Runtime dependencies (Level Zero, OpenCL, NEO driver) add significant image size
- Shared memory means no bandwidth advantage over CPU access

---

## 2. Intel NPU (Neural Processing Unit)

### Hardware Facts
- **YES — Arrow Lake has an NPU** called "AI Boost"
- **13 TOPS (Int8)** on the 275HX
- This is the same NPU architecture as Meteor Lake / Lunar Lake, but HX variants typically have a smaller NPU than the U/H variants

### llama.cpp OpenVINO Backend — NPU Support
- **YES — officially supported** via the OpenVINO backend (`GGML_OPENVINO=ON`)
- Device selection via `GGML_OPENVINO_DEVICE=NPU` environment variable
- Works with existing GGUF models — the backend translates GGML compute graphs to OpenVINO IR at runtime

### Quantization on NPU
- **Primary supported format: Q4_0 only**
- Q6_K tensors are auto-requantized to Q4_0_128 (lossy)
- Embedding weights get Q8_0_C treatment
- **NOT compatible with Q4_K_M** (Llamaste's current default) — would need Q4_0 models

### NPU Limitations (Current)
- Context size must be small (`-c 1024` recommended) — large contexts cause failures
- No model caching on NPU
- Only single chat session supported with stateful execution
- No parallel sequences (`-np > 1`)
- No context-shift support
- No encoder models (embedding, reranking)
- **"Work in progress"** — performance and accuracy validation ongoing

### Build Requirements
- OpenVINO Runtime 2026.0+ (~200–400 MB)
- OpenCL ICD + headers
- TBB (Threading Building Blocks)
- cmake + ninja

### Verdict: NOT PRACTICAL YET for Llamaste
- NPU backend is explicitly marked "work in progress"
- Q4_0-only quantization (Llamaste uses Q4_K_M for quality)
- 13 TOPS is low for LLM inference — NPUs are optimized for small vision/audio models
- Context size limitation (1024 tokens) is crippling for an OS assistant
- Single-session limitation conflicts with Llamaste's multi-slot design
- OpenVINO runtime adds ~300MB to image
- Would only help with prompt evaluation (prefill), not token generation

---

## 3. Vulkan Backend

### Status in llama.cpp
- **YES — Vulkan compute backend exists** (`GGML_VULKAN=ON`)
- Cross-platform GPU compute via Vulkan API
- Simpler than SYCL — just needs Vulkan SDK + driver

### Intel iGPU + Vulkan
- Intel Xe-LPG GPUs do support Vulkan 1.3
- **BUT**: Reports of crashes and incorrect output on Intel GPUs with Vulkan backend (integer dot product operations issue, reported late 2025)
- Intel GPU Vulkan compute support in Mesa (ANV driver) is less mature than NVIDIA/AMD
- Performance is inconsistent across drivers and platforms

### Build Complexity
**MODERATE — much simpler than SYCL.**

Dependencies:
- Vulkan SDK headers (vulkan-headers package)
- Vulkan ICD loader (libvulkan)
- GPU driver with Vulkan support (Mesa ANV for Intel)

```
cmake -B build -DGGML_VULKAN=ON
```

No special compiler needed. Works with GCC/Clang.

### Performance vs CPU-Only
- On Intel iGPUs: **likely WORSE than AVX2 CPU** for the 275HX
  - 64 EUs with shared memory bandwidth = bottleneck
  - AVX2 on 24 cores with DDR5-6400 is very competitive
  - Vulkan overhead (command buffer submission, synchronization) hurts small batches
- On discrete GPUs (Arc A770): ~42–55 tok/s on 7B Q4_0 (competitive with CUDA on mid-range)
- Vulkan shines on AMD/mobile GPUs where no native compute stack exists

### Verdict: POSSIBLE BUT LOW PRIORITY for Llamaste
- Build complexity is manageable (just vulkan-headers + libvulkan)
- But Intel iGPU performance would likely not exceed CPU-only
- Known stability issues on Intel GPUs
- Would benefit future users with discrete GPUs (Arc A770/B580) if they use Llamaste
- **Best as a compile-time option, not default**

---

## 4. Benchmark Endpoint Design

### What `/llamaste/benchmark` Should Measure

#### Standard Test Protocol
1. **Prompt evaluation** (prefill) — measure tokens/second for processing input
2. **Token generation** (decode) — measure tokens/second for output
3. **Time to first token** (TTFT) — latency from request to first generated token
4. **Total completion time** — wall clock for the benchmark prompt

#### Standard Benchmark Prompt
```
Explain how a CPU processes a single instruction, from fetch to execute,
in exactly 200 words. Be precise and technical.
```

Why this prompt:
- Fixed-length output request (~200 tokens generated)
- Technical content exercises the model's reasoning
- Reproducible across model sizes
- Short enough to not disrupt active inference (< 30 seconds on 3B)

#### Metadata to Report
```json
{
  "version": "0.2.5",
  "model": "qwen2.5-3b-instruct-q4_k_m.gguf",
  "model_size_bytes": 2045014016,
  "quantization": "Q4_K_M",
  "cpu_model": "Intel Core Ultra 9 275HX",
  "cpu_features": ["avx2", "avx512f", "f16c", "fma"],
  "cpu_threads_used": 8,
  "cpu_threads_available": 24,
  "ram_total_mb": 36864,
  "ram_available_mb": 28672,
  "gpu_offload": "none",
  "benchmark": {
    "prompt_tokens": 42,
    "prompt_eval_tok_s": 185.3,
    "generated_tokens": 217,
    "generation_tok_s": 14.2,
    "time_to_first_token_ms": 227,
    "total_time_ms": 15507
  }
}
```

### Existing llama-server Performance Data

llama-server already exposes timing data in several places:

1. **`/health`** — Returns server status (ok/loading/error), no timing data
2. **`/metrics`** (requires `--metrics` flag) — Prometheus-format gauges:
   - `prompt_tokens_seconds` — average prompt throughput
   - `predicted_tokens_seconds` — average generation throughput (from docs/issues)
3. **`/slots`** (requires `--slots` flag) — Per-slot state with timing
4. **`/completion` response** — Each completion includes `timings` object:
   - `prompt_n`, `prompt_ms`, `prompt_per_token_ms`, `prompt_per_second`
   - `predicted_n`, `predicted_ms`, `predicted_per_token_ms`, `predicted_per_second`

### Implementation Approach for `/llamaste/benchmark`

**Run without disrupting active inference:**

1. Check if inference is currently active (query `/health` or slot state)
2. If active: return cached last benchmark result with `"cached": true`
3. If idle: run benchmark prompt through `/completion` with a reserved slot
4. Parse `timings` from completion response
5. Combine with system metadata (CPU model from `/proc/cpuinfo`, RAM from `/proc/meminfo`)
6. Cache result in memory (don't re-run unless explicitly requested)
7. Add `?force=true` parameter to force re-benchmark

**Auth**: Require auth (same as other `/llamaste/*` endpoints).

**Timeout**: 120 seconds max (covers 14B model at 2 tok/s × 200 tokens).

---

## 5. Backward Compatibility & Graceful Fallback

### Compile-Time Backend Selection

The cleanest approach for Llamaste:

```
# Default build (CPU-only, current):
cmake -DGGML_NATIVE=ON

# Optional Vulkan build:
cmake -DGGML_NATIVE=ON -DGGML_VULKAN=ON

# Optional SYCL build (requires oneAPI):
cmake -DGGML_NATIVE=ON -DGGML_SYCL=ON -DCMAKE_CXX_COMPILER=icpx

# Optional OpenVINO build:
cmake -DGGML_NATIVE=ON -DGGML_OPENVINO=ON
```

llama.cpp's backend system handles this at runtime:
- Multiple backends can be compiled in simultaneously
- `ggml_backend_reg` auto-discovers available backends at startup
- Model layers are assigned to backends based on `-ngl` (GPU layers) parameter
- If GPU backend fails to initialize: falls back to CPU automatically

### Runtime Detection (No Recompile)

If Vulkan is compiled in:
1. At boot, check `/dev/dri/renderD128` exists (GPU present)
2. Try `vkEnumeratePhysicalDevices()` — if it returns 0 devices, fall back to CPU
3. If Vulkan device found but inference fails: catch error, retry on CPU
4. Log: `[gpu] Vulkan device found: Intel Xe-LPG (64 EU) — offloading N layers`
5. Or: `[gpu] No GPU detected — using CPU-only inference`

### Headless Servers (No iGPU)

- Xeon / headless systems: no `/dev/dri/` devices
- Vulkan `vkEnumeratePhysicalDevices()` returns 0 → CPU path
- SYCL `sycl::device::get_devices()` returns empty → CPU path
- OpenVINO `GGML_OPENVINO_DEVICE=CPU` works without any GPU driver
- **Zero impact on current behavior** — GPU offload is purely additive

### Recommended Architecture for Llamaste

```
Build time:    GGML_VULKAN=ON (optional, controlled by defconfig flag)
               GGML_NATIVE=ON (always, for AVX2/AVX512)

Runtime:       1. Probe /dev/dri/ for GPU
               2. If found AND Vulkan compiled in: try GPU offload
               3. If GPU offload fails OR not compiled: CPU-only (current behavior)
               4. Log the decision for diagnostics
               5. /llamaste/system/info reports "gpu_offload": "vulkan|sycl|openvino|none"
```

---

## Summary Recommendations

| Option | Build Complexity | Performance Gain | Image Size Impact | Recommendation |
|--------|-----------------|------------------|-------------------|----------------|
| **SYCL (iGPU)** | Very High | +21% (marginal, 64 EU) | +500MB | **SKIP** — too complex, too little gain |
| **NPU (OpenVINO)** | High | Unknown (WIP) | +300MB | **SKIP** — not ready, Q4_0 only, 1024 ctx limit |
| **Vulkan (iGPU)** | Low-Medium | Likely negative on iGPU | +5MB | **DEFER** — good for dGPU users later |
| **CPU AVX2/512** | Zero (current) | Baseline (14 tok/s) | 0 | **KEEP** — best bang for buck |
| **Benchmark endpoint** | Low | N/A (diagnostic) | 0 | **DO NEXT** — high value, low effort |

### Priority Order
1. **Implement `/llamaste/benchmark`** — uses existing `/completion` timings, ~100 LOC
2. **Add Vulkan as optional build flag** — future-proofing for dGPU users, no default change
3. **Revisit NPU when OpenVINO backend matures** — check quarterly for Q4_K_M support + larger context
4. **Skip SYCL entirely** — Vulkan or OpenVINO cover the same hardware with less complexity

---

## Sources
- [llama.cpp SYCL backend docs](https://github.com/ggml-org/llama.cpp/blob/master/docs/backend/SYCL.md)
- [llama.cpp OpenVINO backend docs](https://github.com/ggml-org/llama.cpp/blob/master/docs/backend/OPENVINO.md)
- [Intel 275HX specs — NotebookCheck](https://www.notebookcheck.net/Intel-Core-Ultra-9-275HX-Processor-Benchmarks-and-Specs.943094.0.html)
- [Arrow Lake Wikipedia](https://en.wikipedia.org/wiki/Arrow_Lake_(microprocessor))
- [llama.cpp Vulkan performance discussion](https://github.com/ggml-org/llama.cpp/discussions/10879)
- [llama-server README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
- [llama.cpp server health metrics](https://leeroopedia.com/index.php/Implementation:Ggml_org_Llama_cpp_Server_Health_Metrics)
- [llama.cpp GPU acceleration guide](https://www.ywian.com/blog/llama-cpp-gpu-acceleration-complete-guide)
