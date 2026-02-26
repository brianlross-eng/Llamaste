# Open-Source LLM Model Selection for CPU Inference

## Best Models by RAM Tier

### 4 GB RAM (1-3B parameters)
| Model | Quant | Size | License | Notes |
|-------|-------|------|---------|-------|
| **Qwen2.5 1.5B-Instruct** | Q4_K_M | ~1.2 GB | Apache 2.0 | **Top pick** |
| TinyLlama 1.1B | Q5_K_M | ~0.9 GB | Apache 2.0 | 3T training tokens |
| Phi-2 (2.7B) | Q4_K_M | ~1.8 GB | MIT | Strong reasoning |
| SmolLM2 1.7B | Q4_K_M | ~1.3 GB | Apache 2.0 | HuggingFace |
| DeepSeek-R1-Distill-Qwen-1.5B | Q4_K_M | ~1.2 GB | MIT | Reasoning focus |

### 8 GB RAM (3-7B parameters)
| Model | Quant | Size | License | Notes |
|-------|-------|------|---------|-------|
| **Qwen2.5 7B-Instruct** | Q4_K_M | ~4.5 GB | Apache 2.0 | **Top pick** |
| Mistral 7B v0.3 | Q4_K_M | ~4.4 GB | Apache 2.0 | Classic workhorse |
| Llama 3.1 8B | Q3_K_M | ~4.1 GB | Llama License | Meta |
| Phi-3.5 Mini (3.8B) | Q5_K_M | ~3.0 GB | MIT | 128K native context |
| Gemma 2 9B | Q3_K_M | ~4.8 GB | Gemma Terms | Tight fit |

### 16 GB RAM (7-14B parameters)
| Model | Quant | Size | License | Notes |
|-------|-------|------|---------|-------|
| **Qwen2.5 14B-Instruct** | Q4_K_M | ~8.7 GB | Apache 2.0 | **Top pick** |
| Gemma 2 9B | Q5_K_M | ~7.0 GB | Gemma Terms | Excellent quality |
| Llama 3.1 8B | Q8_0 | ~8.5 GB | Llama License | Near-lossless |
| Mistral Nemo 12B | Q4_K_M | ~7.5 GB | Apache 2.0 | Tool use/function calling |
| DeepSeek-R1-Distill-Qwen-14B | Q4_K_M | ~8.7 GB | MIT | Best reasoning |

### 32 GB RAM (14-32B+ parameters)
| Model | Quant | Size | License | Notes |
|-------|-------|------|---------|-------|
| **Qwen2.5 32B-Instruct** | Q4_K_M | ~19.5 GB | Apache 2.0 | **Top pick** |
| Mistral Small 22B | Q5_K_M | ~16 GB | Apache 2.0 | Tool use |
| Mixtral 8x7B (46.7B MoE) | Q4_K_M | ~26 GB | Apache 2.0 | Fast MoE |
| DeepSeek-R1-Distill-Qwen-32B | Q4_K_M | ~19.5 GB | MIT | Best reasoning |

## Memory Consumption per Billion Parameters

| Quantization | Bits/Weight | GB per 1B | 7B Size | 14B Size | 32B Size |
|-------------|------------|-----------|---------|----------|----------|
| F16 | 16.0 | ~2.00 | ~14.0 | ~28.0 | ~64.0 |
| Q8_0 | 8.5 | ~1.07 | ~7.5 | ~15.0 | ~34.0 |
| Q6_K | 6.6 | ~0.83 | ~5.8 | ~11.6 | ~26.6 |
| Q5_K_M | 5.7 | ~0.72 | ~5.0 | ~10.1 | ~23.0 |
| **Q4_K_M** | **4.8** | **~0.60** | **~4.2** | **~8.4** | **~19.2** |
| Q3_K_M | 3.9 | ~0.49 | ~3.4 | ~6.9 | ~15.7 |
| Q2_K | 3.4 | ~0.42 | ~2.9 | ~5.9 | ~13.4 |

## CPU Performance (Tokens/Second)

### Modern Desktop (Ryzen 7 / i7, DDR5)
| Model + Quant | Prompt (tok/s) | Generation (tok/s) |
|---------------|---------------|-------------------|
| Qwen2.5 1.5B Q4_K_M | 200-400 | 30-60 |
| Qwen2.5 7B Q4_K_M | 30-60 | 8-15 |
| Qwen2.5 14B Q4_K_M | 15-30 | 4-8 |
| Qwen2.5 32B Q4_K_M | 6-12 | 2-5 |

Laptop: ~50-70% of desktop. Older DDR4 systems: ~40-60% of DDR5 desktop.

**Key insight:** Generation speed ≈ memory_bandwidth / model_size. DDR5 gives ~1.7x over DDR4.

## RAM Calculation Formula

```
Total RAM = Model_Weights + KV_Cache + Buffers + OS_Overhead

Model_Weights = Parameters(B) x Bits_per_weight / 8
KV_Cache = 2 x layers x kv_heads x head_dim x context_len x 2 bytes
Buffers = ~200-300 MB
OS_Overhead = 0.5-1.5 GB (Linux headless)
```

### KV Cache per 1K Context (FP16)
| Model | KV per 1K Context |
|-------|-------------------|
| Qwen2.5 7B (4 KV heads) | ~14 MB |
| Llama 3.1 8B (8 KV heads) | ~32 MB |
| Qwen2.5 14B (8 KV heads) | ~40 MB |
| Qwen2.5 32B (8 KV heads) | ~64 MB |

KV cache quantization (-ctk q8_0) can reduce by ~50%.

## Context Length Guidelines

| RAM | Safe Context | Max Context |
|-----|-------------|-------------|
| 4 GB | 2048 | 4096 |
| 8 GB | 4096-8192 | 16K (small KV models) |
| 16 GB | 8192-16384 | 32K (with KV quant) |
| 32 GB | 16K-32K | 64K+ (with KV quant) |

## License Quick Reference

### Fully Permissive (Apache 2.0 / MIT)
- **Qwen2.5** (all sizes) -- Apache 2.0
- **Mistral** (7B, Nemo, Small, Mixtral) -- Apache 2.0
- **Phi-2/3/3.5** -- MIT
- **TinyLlama** -- Apache 2.0
- **SmolLM2** -- Apache 2.0
- **DeepSeek-R1-Distill (Qwen-based)** -- MIT

### Commercial with Conditions
- **Llama 3.1/3.2** -- Llama License (700M MAU threshold)
- **Gemma 2** -- Gemma Terms (no competing model training)

### Non-Commercial
- **Codestral 22B** -- MNPL (research only)
- **Command R 35B** -- CC-BY-NC-4.0

**Recommendation for Llamaste:** Ship with Qwen2.5 family (Apache 2.0, no restrictions).

## Where to Download GGUF Models

### HuggingFace (Primary)
- **bartowski** -- Most active GGUF quantizer (2024-2025)
- **TheBloke** -- Huge back catalog
- **QuantFactory** -- Automated, broad coverage
- Search: `{model name} GGUF` on huggingface.co

### Quantization Recommendations

| Use Case | Quantization | Why |
|----------|-------------|-----|
| Default | **Q4_K_M** | Best quality-per-byte, community standard |
| Quality sensitive | Q5_K_M or Q6_K | ~2% better quality |
| Max quality | Q8_0 | Near-lossless |
| Extreme low RAM | IQ3_M or IQ2_XS | imatrix calibrated, better than Q2/Q3 K-quants |
| Speed priority | Q3_K_M or lower | Fewer bytes = faster generation |

**Rule:** Prefer smaller model at higher quant over larger model at very low quant. 7B Q5_K_M > 13B Q2_K.

## Specialization Picks

### Coding
- 4GB: Qwen2.5-Coder 1.5B
- 8GB: Qwen2.5-Coder 7B
- 16GB: Qwen2.5-Coder 14B
- 32GB: Qwen2.5-Coder 32B

### Reasoning
- All tiers: DeepSeek-R1-Distill-Qwen series (MIT license)

### Multilingual
- Qwen2.5 leads all sizes (CJK + European + SE Asian)

## Quick Decision Table

| RAM | Model | Quant | Speed | File Size | License |
|-----|-------|-------|-------|-----------|---------|
| 4 GB | Qwen2.5 1.5B | Q4_K_M | 25-50 tok/s | ~1.2 GB | Apache 2.0 |
| 8 GB | Qwen2.5 7B | Q4_K_M | 8-15 tok/s | ~4.5 GB | Apache 2.0 |
| 16 GB | Qwen2.5 14B | Q4_K_M | 4-8 tok/s | ~8.7 GB | Apache 2.0 |
| 32 GB | Qwen2.5 32B | Q4_K_M | 2-5 tok/s | ~19.5 GB | Apache 2.0 |
