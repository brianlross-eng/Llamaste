# Research 24: LLM Speed Benchmarks & Advanced Optimization

**Created**: 2026-02-27
**Purpose**: Fill gaps in existing speed research (research/09, research/15) with measured benchmarks, quantized KV cache, prompt chunking, Vulkan iGPU, and latest llama.cpp improvements.

---

## 1. Quantized KV Cache (`-ctk`/`-ctv`)

**Confirmed by multiple sources.**

### RAM Savings (8B model at 32K context)
| KV Type | RAM Usage | Savings |
|---------|-----------|---------|
| F16 (default) | ~6 GB | baseline |
| Q8_0 | ~3 GB | 50% |
| Q4_0 | ~2 GB | 67% |

### Speed Impact
Negligible net effect at Q8_0. K-cache quantization slightly *improves* speed (smaller reads); V-cache quantization slightly *degrades* it (dequantization overhead). The two roughly cancel.

### Quality Impact (tested on Qwen2.5 Coder 7B)
- Q8_0: +0.0043 perplexity — negligible
- Q4_0: +0.21-0.25 perplexity — noticeable

### Critical Dependency
`-ctv q8_0` **requires** `--flash-attn` to be active. The server will refuse to start without it.

### CLI Flags
```
--cache-type-k q8_0   # or -ctk q8_0
--cache-type-v q8_0   # or -ctv q8_0
--flash-attn          # required for ctv
```

### Llamaste Recommendation
Enable Q8_0 KV cache as default for all models. On a 16 GB system running Qwen2.5 14B:
- Saves ~750 MB at 4K context
- Saves ~6 GB at 64K context
- No measurable quality loss
- Enables larger context windows or concurrent users

Sources: smcleod.net (Dec 2024), llama.cpp #5932

---

## 2. Prompt Chunking / `--ubatch-size`

**Well-established behavior from llama.cpp docs.**

### What It Controls
`--ubatch-size` (default 512) controls how many tokens are fed to the model per compute kernel call during the **prefill/prompt-processing phase only**. Token generation is unaffected.

### Impact on Time-to-First-Token (TTFT)
For a 2000-token system prompt at default settings (ubatch=512), prefill is split into 4 chunks of 512 tokens. Increasing to `--ubatch-size 2048` processes it in one chunk.

### Measurement
```bash
llama-bench -m model.gguf -p 2000 -n 1 -ub 512    # baseline
llama-bench -m model.gguf -p 2000 -n 1 -ub 2048   # larger chunk
# avg_ns / 1,000,000 = TTFT in ms
```

### Concrete Example
- 100-token prompt on CPU = ~163 ms TTFT
- 2000-token system prompt scales to ~1200-1500 ms (first request only)
- `cache_prompt: true` eliminates this for subsequent requests

### Recommended Values by System
| System | RAM | ubatch-size |
|--------|-----|-------------|
| Weak CPU / 8 GB | Low | 256–512 |
| Mid-range / 16 GB | Medium | 512–1024 |
| High-end / 32 GB+ | High | 2048 |

### Flash Attention Interaction
CPU flash attention helps PP modestly but has minimal effect on TG. On CPU, users report "can't measure any difference" for TG speed.

Sources: llama.cpp #14115, steelph0enix.dev guide

---

## 3. Vulkan Backend for CPU + iGPU

**Multiple sources, hardware-dependent.**

### AMD RDNA2/RDNA3 iGPUs — Substantial Benefit

From blog.linux-ng.de (September 2025), AMD Ryzen 7 7735HS with Radeon 680M:

| Model | CPU-only PP | Vulkan PP | CPU-only TG | Vulkan TG |
|-------|-------------|-----------|-------------|-----------|
| Gemma 3 12B | ~9 t/s | ~37 t/s | ~5 t/s | ~8 t/s |

PP improved **4x**; TG improved **1.6x**.

### Intel Iris Xe — Mixed Results

| Metric | CPU-only | Vulkan | Change |
|--------|----------|--------|--------|
| PP | 30.51 t/s | 42.02 t/s | +38% |
| TG | 9.87 t/s | 7.28 t/s | **-26%** |

PP improves, but TG gets **worse**. For interactive chat the user feels TG speed, not PP — net-negative on Intel.

### AMD Ryzen Z1 Extreme (RDNA3) — Best iGPU Result
- Vulkan PP: 199.36 t/s
- Vulkan TG: 18.77 t/s

### Build Flags
```bash
sudo apt-get install libvulkan-dev glslc
cmake -B build -DGGML_VULKAN=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Runtime
llama-server -m model.gguf -ngl 99
```

### Critical Warning
`--flash-attn` with Vulkan on non-Nvidia hardware causes CPU fallback for attention computation — significantly *slower* than not using flash attention.

### Llamaste Recommendation
- **Phase 1**: CPU-only. Don't ship Vulkan. The TG regression on Intel is unacceptable for an appliance.
- **Phase 2**: Optional Vulkan support. Auto-detect AMD RDNA2+ iGPU and enable if present. Never enable on Intel iGPU.
- For the static binary, Vulkan would require bundling `libvulkan.so` — breaks the single-binary constraint.

Sources: blog.linux-ng.de (Sep 2025), llama.cpp #10879

---

## 4. Latest llama.cpp Speed Improvements (2025-2026)

### CPU Flash Attention Chunking (Jan 2026)
Parallelization of CPU flash attention across threads. Likely 10-30% PP improvement on 8+ core systems. Community benchmarks pending.

### Self-Speculative Decoding Without Draft Model
New draftless option using rolling hash n-gram pool (~16 MB shared across all slots).

```
--spec-type ngram-mod
--spec-ngram-size-n 12    # lookup n-gram length
--spec-ngram-size-m 48    # draft m-gram length
--draft-max 16
```

Observed acceptance rates: 57-76%. Speedup depends on content:
- Code editing, summarization, repetitive content: **1.5-2.5x TG speedup**
- General chat: minimal benefit (<1.1x)

### New CPU SIMD Support
AMX (Intel), ARM SVE/SVE2/SME now fully supported. BF16 dot products via `GGML_AVX512_BF16`.

### Hybrid mmap + DirectIO Loading
Model weights streamed more efficiently from disk — relevant for Llamaste where the model file is on a separate ext4 DATA partition.

### MoE Gate Weight Merging
`--fuse_gate_up_exps` for DeepSeek-family MoE models only. Not relevant for Qwen2.5.

Sources: Buttondown Jan 2026 weekly, llama.cpp GitHub, speculative.md

---

## 5. Qwen2.5 Benchmarks on Consumer x86 Hardware

### Important Note
Official Qwen2.5 benchmarks are GPU/vLLM only. Community x86 CPU data is sparse.

### Theoretical Ceiling (Memory-Bandwidth-Bound TG)
```
TG ≈ memory_bandwidth / model_size_bytes

DDR4-3200 dual channel (~50 GB/s):
  50 / 4.5 GB ≈ 11 t/s  for 7B Q4_K_M
  50 / 8.9 GB ≈ 5.6 t/s for 14B Q4_K_M

DDR5-4800 dual channel (~75 GB/s):
  75 / 4.5 GB ≈ 17 t/s  for 7B Q4_K_M
  75 / 8.9 GB ≈ 8.4 t/s for 14B Q4_K_M

Real efficiency is typically 70-85% of theoretical ceiling.
```

### Practical Estimates
| Model | Quant | Size | Expected TG (DDR4 Ryzen) |
|-------|-------|------|--------------------------|
| Qwen2.5 0.5B | Q8_0 | 0.5 GB | 60-100+ t/s |
| Qwen2.5 1.5B | Q8_0 | 1.5 GB | 30-60 t/s |
| Qwen2.5 7B | Q4_K_M | 4.5 GB | 8-20 t/s |
| Qwen2.5 14B | Q4_K_M | 8.9 GB | 4-8 t/s |

### Comparison
Qwen2.5 7B is speed-equivalent to Llama3 8B at the same quantization. GQA architecture provides slightly less KV cache pressure but no measurable core-inference speed difference.

Apple Silicon reference: Qwen2.5-7B on M1 Max via llama.cpp = 40.75 t/s (3-5x faster than x86 due to 8x higher memory bandwidth).

Sources: singhajit.com benchmarks, llm-tracker.info cheat sheet

---

## 6. Bonus: ik_llama.cpp Fork

On Ryzen 7950X (Zen4/AVX512), LLaMA-3.1-8B prompt processing:

| Quant | llama.cpp | ik_llama.cpp | Speedup |
|-------|-----------|--------------|---------|
| Q4_0 | 146 t/s | 260 t/s | 1.78x |
| Q8_0 | 138 t/s | 258 t/s | 1.87x |
| IQ3_S | 29 t/s | 150 t/s | 5.18x |

The fork exploits AVX512 more aggressively. **Not recommended for Phase 1** (fork maintenance risk, no upstreaming), but viable Phase 2 swap if TTFT complaints arise.

Source: ik_llama.cpp wiki (Jan 2025)

---

## 7. YouTube Video Research

### Videos Searched
- "llama.cpp speed optimization 2025"
- "fastest llama inference cpu"
- "llama cpp benchmark tutorial"
- "quantized kv cache llama"

### Key Findings from Video Content

**"How to Make llama.cpp FAST" (various creators, 2025)**
Common optimization checklist cited across multiple videos:
1. Use Q4_K_M quantization (best quality/speed tradeoff)
2. Enable flash attention (`-fa`)
3. Set threads to physical cores only (`-t` = physical, not hyperthreaded)
4. Use `--mlock` to prevent swapping
5. Enable `cache_prompt: true` in API requests
6. For speculative decoding: n-gram mode is free (no draft model needed)

**"Building a Local AI Server" (multiple, 2025-2026)**
Key takeaways relevant to Llamaste:
- Memory bandwidth is the bottleneck, not compute — DDR5 gives ~50% more t/s than DDR4
- First request is always slow (system prompt processing) — subsequent requests benefit from KV cache reuse
- For OS-type workloads (short tool calls), the 0.5B-1.5B models are the sweet spot: 30-100 t/s
- Grammar-constrained output (JSON mode) doesn't slow down generation — it can actually speed it up by reducing token count

---

## 8. Recommended Llamaste Server Configuration (Phase 1)

Based on all findings, the recommended startup flags for Llamaste:

```bash
llama-server \
  --model /data/models/current.gguf \
  --ctx-size 8192 \
  --batch-size 2048 \
  --ubatch-size 512 \
  --flash-attn \
  --cache-type-k q8_0 \
  --cache-type-v q8_0 \
  --threads $(nproc) \
  --mlock \
  --port 80 \
  --host 0.0.0.0
```

### Per-Request Settings (API)
```json
{
  "cache_prompt": true,
  "temperature": 0.7,
  "top_p": 0.9,
  "min_p": 0.05
}
```

### Expected Performance with These Settings

| System | Model | TG Speed | TTFT (first) | TTFT (cached) |
|--------|-------|----------|-------------|---------------|
| 8 GB DDR4 | Qwen2.5 7B Q4_K_M | 8-15 t/s | ~1.5s | <100ms |
| 16 GB DDR4 | Qwen2.5 14B Q4_K_M | 4-8 t/s | ~2.5s | <100ms |
| 16 GB DDR5 | Qwen2.5 14B Q4_K_M | 6-10 t/s | ~2.0s | <100ms |
| 32 GB DDR5 | Qwen2.5 32B Q4_K_M | 3-5 t/s | ~4.0s | <100ms |

### Key Insight for Llamaste
The biggest speed win is **KV cache reuse** (`cache_prompt: true`). After the first request, the 2000-token system prompt is cached, and TTFT drops from 1-4 seconds to <100ms. This is by far the most impactful optimization and it's free.

---

## 9. Updates to Previous Research

### Changes from research/15
- **KV cache quantization**: Not covered before. Q8_0 saves 50% KV RAM at zero quality cost. Should be default.
- **ubatch-size**: Not covered before. Default 512 is fine for most cases.
- **Vulkan iGPU**: Not covered before. Skip for Phase 1, AMD-only for Phase 2.

### Changes from research/09
- **ik_llama.cpp fork**: New finding. 1.8-5x PP speedup on AVX512. Monitor but don't adopt yet.
- **Self-speculative without draft model**: Updated — `--spec-type ngram-mod` is the current flag name.
- **CPU flash attention chunking**: New (Jan 2026). Will benefit PP on multi-core.

### Changes to MEMORY.md Build Notes
Add: `--flash-attn` is required when using `-ctv q8_0`. Without it, server refuses to start.
Add: KV cache Q8_0 saves 50% KV RAM at negligible quality cost — use as default.
