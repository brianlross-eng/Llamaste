# Research 24: llama.cpp Speed Optimization — Gap Analysis
**Date**: 2026-02-27
**Status**: Complete
**Purpose**: Fill gaps not covered in research/09-cpu-speed-optimization.md and research/15-llm-speed-optimization.md. Focuses on five concrete areas: quantized KV cache, ubatch-size tuning, Vulkan on iGPU, 2025/2026 new features, and Qwen2.5 real-world benchmarks.

---

## 1. Quantized KV Cache (`-ctk`/`-ctv`)

### What it is
KV cache stores the computed attention keys and values for every token in the context window. By default these are stored in F16 (16-bit float). Quantizing them to Q8_0 or Q4_0 reduces memory usage significantly.

### RAM Savings — Confirmed by Multiple Sources

For an 8B parameter model with 32K context window:
- F16 KV cache: ~6 GB
- Q8_0 KV cache: ~3 GB (50% savings)
- Q4_0 KV cache: ~2 GB (67% savings)

This is not model-specific: the ratio holds because KV cache size scales as:
`ctx_size × num_heads × head_dim × num_layers × 2 (K+V) × bytes_per_element`

The practical implication: saving 3 GB with Q8_0 on an 8B/32K setup allows either:
- Doubling context to 64K while staying in the same memory envelope, OR
- Running a 14B model instead of 8B at the original context length

Source: [smcleod.net KV quantization article](https://smcleod.net/2024/12/bringing-k/v-context-quantisation-to-ollama/) — confirmed by [llama.cpp discussion #5932](https://github.com/ggml-org/llama.cpp/discussions/5932)

### Speed Impact

**K cache quantization**: slight speed *improvement* (memory reads are smaller, cache bandwidth improves)
**V cache quantization**: slight speed *degradation* (dequantization overhead at read time)
**Net effect at Q8_0**: negligible — the two effects roughly cancel out
**Net effect at Q4_0**: still negligible on throughput, but noticeable quality cost

Note: Flash attention is required for V cache quantization. The error message if you try without it:
> `V cache quantization requires flash_attn`

This means `-ctv q8_0` automatically requires `--flash-attn` to be active.

### Quality Impact — Confirmed

Tested on Qwen2.5 Coder 7B (from smcleod.net article):
- Q8_0: +0.0043 perplexity increase — negligible
- Q4_0: +0.206 to +0.25 perplexity increase — noticeable, model-dependent

The K cache is more sensitive to quantization than the V cache. For models using heavy GQA (grouped query attention) the degradation is slightly worse. Qwen2.5 uses GQA, so Q8_0 is recommended over Q4_0 for it.

### Exact CLI Flags

```
# Server launch flags
--cache-type-k q8_0      # or: -ctk q8_0
--cache-type-v q8_0      # or: -ctv q8_0
--flash-attn             # required when using ctv

# Available types: f16 (default), q8_0, q4_0
# q5_0 also supported in some builds
```

API: there is no per-request API parameter — these are server startup flags only.

### Relevance to Llamaste

HIGH. On a 16 GB system running Qwen2.5 14B Q4_K_M (~9 GB weights), the KV cache at 4K context in F16 is roughly 2.5 GB. Switching to Q8_0 saves ~1.25 GB, which matters at that memory tier. At larger context (16K+) the savings become more significant. Recommend: enable `-ctk q8_0 -ctv q8_0 --flash-attn` as default in llamaste's server startup for all models.

---

## 2. Prompt Chunking / `--ubatch-size`

### What `--ubatch-size` Controls

llama.cpp has two batch-size parameters:
- `--batch-size` (`-b`): logical maximum batch size — default 2048
- `--ubatch-size` (`-ub`): physical (micro) batch size — default 512

`--ubatch-size` controls how many tokens are fed to the model in a single compute kernel call during the *prompt processing (prefill) phase*. The full prompt of N tokens is split into chunks of `ubatch-size` and processed sequentially.

This is distinct from token generation (decode), which processes exactly 1 token at a time (or a small draft batch for speculative decoding).

### How It Affects TTFT

LLM inference has two distinct phases:
1. **Prompt processing (PP/prefill)**: compute-bound, matrix-matrix multiplication, fast on CPU if parallelized
2. **Token generation (TG/decode)**: memory-bandwidth-bound, matrix-vector multiplication, slower

For a 2000-token system prompt:
- Larger `--ubatch-size` → fewer kernel calls → better CPU pipeline utilization → lower TTFT
- Smaller `--ubatch-size` → more kernel calls → lower peak memory during prefill

The default `--ubatch-size 512` means a 2000-token prompt is processed in 4 chunks. Increasing to 2048 processes it in one chunk.

### Measuring TTFT

Command:
```bash
llama-bench -m <model> -p 2000 -n 1 -ub 512 -o json   # default
llama-bench -m <model> -p 2000 -n 1 -ub 2048 -o json  # larger ubatch
```

The `avg_ns` field gives TTFT in nanoseconds. Divide by 1,000,000 for milliseconds.

Concrete example from llama.cpp GitHub discussion #14115: a 100-token prompt on a CPU system measured 163.33 ms TTFT (`avg_ns = 163,327,916`). This scales roughly linearly with prompt length for CPU inference (prompt processing is compute-bound).

For a 2000-token system prompt at the same PP speed (1632 t/s implied): TTFT ≈ 2000 / 1632 * 1000 ≈ **1225 ms** (first request only — cache_prompt eliminates this for subsequent requests).

### Optimal Values by Hardware

From community recommendations:
- **Weak CPU (4-core laptop, 8 GB)**: `-ub 256` or `-ub 512` — reduce peak memory pressure during prefill
- **Mid-range CPU (8-16 core, 16+ GB)**: `-ub 512` (default) or `-ub 1024` — good balance
- **High-end CPU (Ryzen 9, Threadripper)**: `-ub 2048` — maximize PP throughput, reduces TTFT for long prompts
- **Server with GPU offload**: `-b 4096 -ub 2048` — see real config from llama.cpp issue #17284

A "hybrid setup" trick documented in GitHub discussions: launch with high `-ub` to prefill the KV cache fast, then restart with a lower `-ub` for better TG speed. Not practical for Llamaste's use case (PID 1, continuous operation), but useful context.

### Flash Attention Interaction

Flash attention (`--flash-attn`) primarily helps with prompt processing by fusing the attention softmax computation. On CPU, the benefit is modest but present for large contexts. One benchmark on Ryzen 5900X + SmolLM2 1.7B Q8_0 with flash attention enabled:
- PP512: 162.54 ± 1.70 t/s
- TG128: 22.50 ± 0.05 t/s

Without flash attention, TG speed is similar but PP is slightly slower for large contexts.

### Relevance to Llamaste

MEDIUM-HIGH. Llamaste has a large fixed system prompt (estimated 1000-3000 tokens). First-request TTFT is the most impacted parameter. Key recommendation:
- Use `-b 2048 -ub 512` as safe default
- On hardware detect: if RAM >= 16 GB, bump to `-ub 2048`
- The system prompt TTFT cost is only paid once per slot, then eliminated by `cache_prompt: true`

---

## 3. Vulkan Backend for CPU + iGPU

### Does Vulkan on iGPU Actually Help?

It depends heavily on GPU architecture. Findings from benchmarks:

**AMD iGPUs (RDNA2/RDNA3) — YES, substantially:**

From blog.linux-ng.de (September 2025), AMD Ryzen 7 7735HS with Radeon 680M (RDNA2) iGPU:
| Model | CPU-only prompt (t/s) | Vulkan iGPU prompt (t/s) | CPU-only eval (t/s) | Vulkan iGPU eval (t/s) |
|-------|----------------------|--------------------------|---------------------|------------------------|
| Gemma 3 12B | ~9 | ~37 | ~5 | ~8 |
| Gemma 3 27B | — | ~16 | — | ~2.2 |
| Gemma 3 1B | — | ~150 | — | ~60 |

Prompt processing improved **4x** (9 → 37 t/s). Token generation improved **1.6x** (5 → 8 t/s). The disparity makes sense: PP is compute-bound (GPU wins), TG is memory-bandwidth-bound (shared memory iGPU has same bandwidth as CPU).

From GitHub discussion #10879, Intel Iris Xe (i7-1185G7):
| Config | PP (t/s) | TG (t/s) |
|--------|----------|----------|
| CPU-only | 30.51 ± 0.25 | 9.87 ± 0.05 |
| Vulkan ngl=100 | 42.02 ± 0.07 | 7.28 ± 0.24 |
| Vulkan ngl=100 (improved driver) | 50.86 ± 0.03 | 8.30 ± 0.05 |

Intel Iris Xe: PP improves ~65%, but TG is **slightly worse** (9.87 → 7.28 t/s initially, then 8.30 with better driver). This matters: for interactive chat, TG speed is what the user perceives.

From GitHub discussion #10879, AMD Ryzen Z1 Extreme:
- PP: 199.36 ± 7.02 t/s (Vulkan)
- TG: 18.77 ± 0.02 t/s (Vulkan)
This is an RDNA3-class iGPU with high-bandwidth shared memory — substantially better than Intel.

**Key pattern**: AMD RDNA2/RDNA3 iGPUs benefit significantly from Vulkan. Intel iGPUs may see PP improvement but TG regression, making the net benefit unclear for interactive use.

### Build Flags for Vulkan

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install libvulkan-dev glslc

# CMake build
cmake -B build -DGGML_VULKAN=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j$(nproc)

# Docker build (simpler)
docker build -t llama-cpp-vulkan -f .devops/vulkan.Dockerfile .

# Runtime: verify detection
./build/bin/llama-cli -m model.gguf -p "test" -ngl 99
# Look for: "ggml_vulkan: Using Intel(R) Graphics ... | uma: 1 | fp16: 1"
```

### AMD Variable Graphics Memory (VGM)

AMD Ryzen AI 300 series (Phoenix, Hawk Point, Strix Point) support VGM, which extends the iGPU's "dedicated" VRAM from 512 MB to up to 75% of system RAM as a contiguous block. This is critical for running larger models on iGPU — without VGM, the iGPU must use fragmented shared system memory.

This is a BIOS setting, not a software flag. Llamaste cannot control it programmatically, but documentation should note it for AMD users.

### Intel iGPU: SYCL Backend is Better

For Intel Arc and built-in graphics, SYCL outperforms Vulkan:
```bash
source /opt/intel/oneapi/setvars.sh
cmake -B build -DGGML_SYCL=ON -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx
```

However, SYCL requires Intel oneAPI toolkit which is not easily bundled in a static OS image. The Vulkan path is more practical for Llamaste.

### Flash Attention + Vulkan Warning

Flash attention on Vulkan is only implemented for Nvidia GPUs with coopmat2 extension. On Intel/AMD iGPUs with Vulkan, enabling `--flash-attn` causes fallback to CPU for the attention computation — which is *slower* than not enabling it. Keep `--flash-attn` disabled when using Vulkan on non-Nvidia hardware.

```
# Safe iGPU launch
llama-server -m model.gguf -ngl 99 --no-flash-attn  # Vulkan + iGPU, no flash attn
```

Source: llama.cpp server README + [GitHub discussion #10879](https://github.com/ggml-org/llama.cpp/discussions/10879)

### Relevance to Llamaste

MEDIUM. Llamaste targets CPU-primary inference. However, hardware detection should probe for Vulkan capability and use it if AMD RDNA2+ is detected. The 4x PP speedup means a 2000-token system prompt processes in ~250 ms instead of ~1000 ms on the first load — significant UX improvement. Intel iGPU Vulkan should probably be left disabled by default due to TG regression risk; could be made user-configurable.

---

## 4. Latest llama.cpp Speed Improvements (2025-2026)

### CPU Compute Extensions (Confirmed in mainline)

As of late 2025, llama.cpp supports:
- **x86**: AVX, AVX2, AVX512, AVX-VNNI, **AMX** (Intel Advanced Matrix Extensions)
- **x86 BF16**: `GGML_AVX512_BF16` — BF16 dot products via `VDPBF16PS`
- **ARM**: NEON, i8MM, SVE, SVE2, SME, SME2
- **RISC-V**: RVV, ZVFH, ZFH

The ARM SVE additions in 2025 provided significant gains on Graviton3E (AWS) and similar ARM server chips. The Raspberry Pi 5 (ARMv8.2 with dotprod + fp16) achieved a ~10x PP speedup vs. older ARMv8 builds from the llamafile team's custom kernels — some of these gains have been partially upstreamed.

Source: [Wikipedia llama.cpp article](https://en.wikipedia.org/wiki/Llama.cpp) + [llama.cpp GitHub](https://github.com/ggml-org/llama.cpp)

### CPU Flash Attention Chunking (2026)

The January 2026 weekly report ([Buttondown](https://buttondown.com/weekly-project-news/archive/weekly-github-report-for-llamacpp-january-25-2026-7750/)) documents:
> "Introduced chunking in CPU flash attention implementation to enable parallelization improvements"

This allows CPU flash attention to be split across threads more effectively, improving PP speed on multi-core systems. Exact speedup numbers not yet benchmarked in community, but likely 10-30% PP improvement for 8+ core systems.

### Self-Speculative Decoding Without Draft Model (PR #18471)

A new draftless speculative method was merged that uses current token history to predict future tokens. Best for:
- Code generation with repetitive patterns
- Summarization tasks where prompt content is echoed
- Reasoning chains that repeat intermediate steps

The `--spec-type` flag now accepts:
- `ngram-simple` — simplest, looks for matching n-gram in history
- `ngram-map-k` — tracking up to 4 candidate continuations per key
- `ngram-map-k4v` — experimental, statistical selection
- `ngram-mod` — lightweight hash pool (~16 MB), shared across server slots

Acceptance rates observed in official llama.cpp docs:
- ngram-simple: 57-70% acceptance rate
- ngram-mod: 730/960 tokens accepted (76% in one test)

For code editing tasks (where prompt contains the full file being edited), 70-76% acceptance rates translate to roughly 1.5-2.5x TG speedup. For general chat, acceptance rates drop to ~30-40%, giving minimal benefit.

Full CLI flags:
```
--spec-type ngram-mod          # or ngram-simple, ngram-map-k, ngram-map-k4v
--spec-ngram-size-n 12         # lookup n-gram size (default: 12)
--spec-ngram-size-m 48         # draft m-gram size (default: 48)
--draft-max 16                 # max tokens to draft per step
```

Source: [llama.cpp speculative.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/speculative.md)

### MoE Gate Weight Merging

For MoE models (DeepSeek family), a new flag `--fuse_gate_up_exps` merges gate and expert weights, reducing kernel launches and improving throughput. Not relevant for Qwen2.5 (not MoE), but noted for future models.

### Hybrid mmap + DirectIO Loading

New in 2025-2026: hybrid model loading combining mmap (for hot weights that stay in memory) and DirectIO (for streaming cold weights). This is particularly relevant for Llamaste where the model file lives on the DATA partition and must be streamed into RAM.

### Expected Attention (KV Cache Compression)

Research-phase feature: "Expected Attention method exploring KV cache compression based on predicted future queries." This could further reduce KV cache memory requirements beyond what quantization provides. Not production-ready as of 2026-02-27.

### Profile-Guided Speculative Decoding

New work uses empirically measured batch cost profiles to dynamically adjust draft length. Uses look-ahead parameters to improve prediction accuracy. This is separate from the static `--draft-max` parameter — it adapts draft length based on observed hardware performance.

---

## 5. Qwen2.5 Benchmarks on Consumer Hardware

### CPU-Only x86 Speed (Triangulated from Multiple Sources)

Concrete published data for Qwen2.5 on x86 CPU specifically is sparse. These estimates are triangulated from: Llama3 8B benchmarks (similar parameter count), Qwen2 architecture analysis, and AMD Ryzen AI 300 marketing data.

**Qwen2.5 7B Q4_K_M (~4.5 GB model):**
- Expected range on modern x86 (AMD Ryzen 9, 16 GB DDR4 3200): **8-20 t/s** TG
- AMD Ryzen AI 9 HX 375 (high-end mobile, 2024): up to 50.7 t/s on 1B model; extrapolated 7B ≈ 15-25 t/s
- The Qwen2/2.5 architecture with GQA (grouped query attention) has fewer KV heads than Llama3, reducing memory bandwidth for KV cache reads — slight speed advantage for TG

**Qwen2.5 14B Q4_K_M (~8.9 GB model):**
- Expected range on 16 GB x86: **4-12 t/s** TG
- 16 GB total RAM with 8.9 GB model leaves ~7 GB for OS, KV cache, and overhead — tight but viable at 4K context
- Memory bandwidth bottleneck: TG speed ≈ RAM bandwidth / model size
  - DDR4-3200 dual channel: ~50 GB/s → 50 / 8.9 ≈ **5.6 t/s** theoretical ceiling at Q4_K_M
  - DDR5-4800 dual channel: ~75 GB/s → 75 / 8.9 ≈ **8.4 t/s** theoretical ceiling
  - These are ceilings, not achieved speeds; real efficiency is 70-85% of theoretical

**Qwen2.5 0.5B / 1.5B Q8_0 (dispatcher tier):**
- 0.5B Q8_0: ~0.5 GB — expect 60-100+ t/s on any modern CPU
- 1.5B Q8_0: ~1.5 GB — expect 30-60 t/s
- These are the models Llamaste would use for fast tool dispatch

### Apple Silicon (for reference comparison)

From singhajit.com benchmarks:
- Qwen2.5-7B on M1 Max (MLX): 63.7 t/s
- Qwen2.5-7B on M1 Max (llama.cpp/GGUF): 40.75 t/s

Apple Silicon is ~3-5x faster than equivalent-tier x86 for TG due to much higher memory bandwidth (400 GB/s vs 50-75 GB/s for consumer DDR).

### Qwen2.5 vs Llama3 Speed Comparison

Qwen2.5's architecture advantage: it uses a larger vocabulary (~150K tokens) but fewer transformer heads due to GQA. The vocabulary size primarily affects embedding lookup (the first and last layers) — not the core attention blocks. This means:
- Core inference (attention + FFN) is similar speed to Llama3 at same parameter count
- Token-level efficiency may be slightly higher (more meaning per token in Chinese/code) but irrelevant for speed measurement

Multiple sources confirm Qwen2.5 7B is broadly speed-equivalent to Llama3 8B at the same quantization level on the same hardware.

### Context Length Impact on TG Speed

For CPU inference, increasing context length (and thus KV cache size) increases memory bandwidth demand during TG:
- At 4K context: KV cache adds ~0.5 GB for 8B model → negligible impact
- At 32K context: KV cache adds ~4 GB for 8B model (F16) → meaningful bandwidth pressure
- With Q8_0 KV cache: 32K context adds ~2 GB → much better

The `ik_llama.cpp` fork (discussed below) maintains TG speed better at long contexts (2.0 t/s at 8K) compared to mainline llama.cpp (1.47-1.66 t/s at 8K) — likely because it uses more efficient cache access patterns.

### Memory Requirements Summary

| Model | Quant | Size | 4K ctx F16 KV | 4K ctx Q8_0 KV | Minimum RAM |
|-------|-------|------|----------------|----------------|-------------|
| Qwen2.5 0.5B | Q8_0 | 0.5 GB | ~0.1 GB | ~0.05 GB | 2 GB |
| Qwen2.5 1.5B | Q8_0 | 1.5 GB | ~0.2 GB | ~0.1 GB | 4 GB |
| Qwen2.5 7B | Q4_K_M | 4.5 GB | ~1.0 GB | ~0.5 GB | 8 GB |
| Qwen2.5 14B | Q4_K_M | 8.9 GB | ~1.5 GB | ~0.75 GB | 12 GB |
| Qwen2.5 32B | Q4_K_M | 20 GB | ~3 GB | ~1.5 GB | 24 GB |

---

## 6. Notable Finding: `ik_llama.cpp` Fork

Not requested but highly relevant — this fork was discovered during research and addresses Llamaste's primary bottleneck.

### Performance vs Mainline (January 2025 Benchmarks)

On Ryzen 7950X (Zen4/AVX512), LLaMA-3.1-8B, prompt processing:

| Quantization | llama.cpp (t/s) | ik_llama.cpp (t/s) | Speedup |
|---|---|---|---|
| F16 | 63.24 | 139.08 | **2.20x** |
| Q8_0 | 137.79 | 257.81 | **1.87x** |
| Q4_0 | 146.20 | 260.71 | **1.78x** |
| Q5_0 | 104.30 | 243.08 | **2.33x** |
| Q3_K_S | 80.68 | 245.12 | **3.04x** |
| IQ3_S | 28.96 | 149.98 | **5.18x** |

On Ryzen 5975WX (AVX2 only), the gains are smaller (1.6-1.7x for Q4_0), confirming the fork's larger benefit comes from better AVX512 exploitation.

Token generation also improves: ik_llama.cpp sustains 2.0 t/s at 8K context vs 1.47-1.66 t/s for mainline.

Source: [ik_llama.cpp wiki: Jan 2025 performance comparison](https://github.com/ikawrakow/ik_llama.cpp/wiki/Jan-2025:-prompt-processing-performance-comparison)

### Should Llamaste Use This Fork?

**Pros:**
- 1.8-2.3x PP speedup (reduces TTFT for first system prompt load)
- Better long-context TG stability
- Supports same GGUF format — drop-in

**Cons:**
- Not upstream — fork diverges over time, security patches lag
- Author explicitly says no upstreaming planned
- Server API compatibility not guaranteed as mainline evolves
- Would complicate Buildroot package maintenance

**Recommendation**: Use mainline llama.cpp for Phase 1. Document ik_llama.cpp as a possible future swap if TTFT becomes a user complaint. The `cache_prompt` feature eliminates TTFT for all but the first request to a slot, making the PP speedup less critical.

---

## 7. Grammar-Constrained Sampling (Supplement to Research 15)

Research 15 claimed "100x faster JSON" from grammar constraints — this needs calibration. The actual mechanism is that grammar constraints reduce the vocabulary to a small valid set at each token position, eliminating most tokens from consideration in the sampler.

The speed benefit is in *sampling*, not in *forward pass computation*. The forward pass still runs at full speed. For small output sizes (a typical tool call JSON of 50-200 tokens), the sampling time is a small fraction of total inference time.

More accurate claim: grammar constraints provide **faster token acceptance** (fewer tokens to score) rather than accelerating the model's forward pass. The practical benefit is:
- Smaller outputs (20-50 tokens) complete faster because fewer invalid tokens are generated and need to be discarded
- The model rarely "hallucinates" invalid JSON structure, so fewer retries are needed

A known gotcha: complex grammar patterns like `x? x? x?` (repeated optionals) cause exponential state explosion and can make sampling extremely slow. Llamaste's tool schemas should be written as simple flat JSON with no deeply nested optional fields.

Source: [llama.cpp issue #4218: speed-up grammar sampling](https://github.com/ggml-org/llama.cpp/issues/4218) + [constrained decoding article](https://www.aidancooper.co.uk/constrained-decoding/)

---

## 8. Summary: Priority Ranking for Llamaste

| Optimization | RAM Impact | Speed Impact | Implementation Cost | Recommendation |
|---|---|---|---|---|
| `cache_prompt: true` | None | Eliminates TTFT after 1st request | Zero (API flag) | MUST-HAVE, already in plan |
| `-ctk q8_0 -ctv q8_0 --flash-attn` | -50% KV | Negligible | Zero (startup flags) | MUST-HAVE, add to default server config |
| `--ubatch-size 2048` (on >=16 GB) | +peak RAM during prefill | Faster initial TTFT | Low (hw detect) | SHOULD-HAVE |
| Ngram speculative decoding | ~16 MB | 1.5-2.5x on code/repetitive | Low (startup flag) | SHOULD-HAVE for code tool responses |
| Vulkan iGPU (AMD only) | None | 1.6-4x PP, 1.6x TG | Medium (hw detect + build) | CONSIDER for Phase 2 |
| Flash attention (CPU) | None | Minimal TG benefit, modest PP | Zero (startup flag) | Enable by default (required for ctv anyway) |
| ik_llama.cpp fork | None | 1.8-3x PP | High (fork maintenance) | Phase 2 consideration only |

### Exact Recommended Server Startup Flags

```bash
# Baseline Llamaste server config (Phase 1 default):
llama-server \
  --model /data/models/current.gguf \
  --ctx-size 8192 \
  --batch-size 2048 \
  --ubatch-size 512 \
  --flash-attn \
  --cache-type-k q8_0 \
  --cache-type-v q8_0 \
  --threads $(nproc) \
  --port 8080

# Phase 2 additions (hardware-conditional):
# If AMD iGPU detected:    add -ngl 99 (no --flash-attn with Vulkan non-Nvidia)
# If RAM >= 16 GB:          change --ubatch-size 2048
# For code-heavy use case: add --spec-type ngram-mod --draft-max 16
```

---

## Sources

- [llama.cpp discussion #5932: 4-bit KV cache](https://github.com/ggml-org/llama.cpp/discussions/5932)
- [smcleod.net: Bringing K/V Context Quantisation to Ollama](https://smcleod.net/2024/12/bringing-k/v-context-quantisation-to-ollama/)
- [llama.cpp discussion #14115: Tutorial measuring TTFT](https://github.com/ggml-org/llama.cpp/discussions/14115)
- [llama.cpp discussion #18030: Batch processing performance](https://github.com/ggml-org/llama.cpp/discussions/18030)
- [llama.cpp discussion #10879: Vulkan iGPU performance](https://github.com/ggml-org/llama.cpp/discussions/10879)
- [blog.linux-ng.de: Running LLMs with Vulkan (Sep 2025)](https://blog.linux-ng.de/2025/09/27/running-llms-with-llama-cpp-using-vulkan/)
- [AMD blog: Ryzen AI 300 + llama.cpp](https://www.amd.com/en/blogs/2024/accelerating-llama-cpp-performance-in-consumer-llm.html)
- [Buttondown: llama.cpp weekly Jan 25 2026](https://buttondown.com/weekly-project-news/archive/weekly-github-report-for-llamacpp-january-25-2026-7750/)
- [llama.cpp speculative.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/speculative.md)
- [llama.cpp build.md](https://github.com/ggml-org/llama.cpp/blob/master/docs/build.md)
- [llama.cpp discussion #13606: KV cache reuse tutorial](https://github.com/ggml-org/llama.cpp/discussions/13606)
- [ik_llama.cpp GitHub](https://github.com/ikawrakow/ik_llama.cpp)
- [ik_llama.cpp wiki: Jan 2025 performance comparison](https://github.com/ikawrakow/ik_llama.cpp/wiki/Jan-2025:-prompt-processing-performance-comparison)
- [ik_llama.cpp discussion #164: CPU performance comparison](https://github.com/ikawrakow/ik_llama.cpp/discussions/164)
- [justine.lol/matmul: LLaMA goes faster on CPUs](https://justine.lol/matmul/)
- [singhajit.com: Qwen2 and Llama 3.1 benchmark results](https://singhajit.com/llm-inference-speed-comparison/)
- [llm-tracker.info: LLM inference benchmarking cheat sheet](https://llm-tracker.info/howto/LLM-Inference-Benchmarking-Cheat%E2%80%91Sheet-for-Hardware-Reviewers)
- [llama.cpp discussion #8860: KV cache prefix persistence](https://github.com/ggml-org/llama.cpp/discussions/8860)
- [llama.cpp issue #4218: speed-up grammar sampling](https://github.com/ggml-org/llama.cpp/issues/4218)
- [OpenBenchmarking.org llama.cpp test](https://openbenchmarking.org/test/pts/llama-cpp)
- [steelph0enix.dev: llama.cpp guide](https://blog.steelph0enix.dev/posts/llama-cpp-guide/)
