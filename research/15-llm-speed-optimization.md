# Making the LLM Faster — Speed Optimization for LLM-as-OS

## The Problem

For an LLM that IS the operating system, every interaction waits on inference. Kernel operations take microseconds; LLM decisions take 100-2000ms. The goal: sub-500ms for most OS tool calls.

---

## Priority-Ordered Optimization Techniques

### 1. KV Cache Reuse for System Prompt (Highest Impact, Zero Cost)

**What**: Pre-compute the system prompt + tool definitions KV cache once at boot. Reuse it for every request.

**How**: llama.cpp's `cache_prompt=true` (default in server mode) automatically reuses KV cache for identical prompt prefixes. The system prompt + tool definitions are the same for every request, so they're computed once.

**Impact**: Skips re-processing the entire system prompt. Can achieve sub-100ms for queries where the cached prefix covers most of the input.

**Implementation**: Already built into llama.cpp server. Just ensure the system prompt is a stable prefix across all requests.

---

### 2. Grammar-Constrained Decoding for Tool Calls (100x Faster JSON)

**What**: When the LLM outputs a tool call, constrain the token search space to valid JSON only.

**How**: Define a JSON schema for tool calls. llama.cpp converts this to a GBNF grammar and prunes invalid tokens at each generation step. The LLM never generates invalid JSON.

**Impact**: XGrammar achieves 100x speedup over unconstrained generation. Even basic grammar constraints in llama.cpp significantly reduce generation time for structured output.

**Why it matters for Llamaste**: Most OS operations produce a tool call JSON, not free text. Constraining the output to valid JSON with known tool names drastically reduces the search space.

---

### 3. Speculative Decoding Without a Draft Model (1.8-2.5x Speedup)

**What**: Use n-gram patterns from the generated text itself to predict future tokens. No separate draft model needed — no extra RAM.

**How**: llama.cpp supports multiple n-gram speculative modes:
- `--spec-ngram-size-n 12` (lookup window)
- `--spec-ngram-size-m 48` (draft length)
- Works by searching your own token history for matching patterns

**Impact**: 1.8-2.5x speedup for generation. Especially effective for tool calls because tool invocations repeat patterns (same tool names, similar parameter structures).

**Alternative**: If RAM allows, a tiny draft model (Qwen2.5-0.5B, ~400 MB) paired with the main model gives even better acceptance rates.

---

### 4. Smaller Specialized Model for Tool Calling

**What**: Instead of a large general-purpose model, use a small model specifically fine-tuned for function calling.

**Candidates**:
| Model | Size | Tool Call Accuracy | RAM |
|-------|------|-------------------|-----|
| FunctionGemma | 270 MB | 85% (fine-tuned) | <1 GB |
| Qwen2.5-0.5B-Instruct | ~400 MB | 68% single-turn | ~1 GB |
| Hammer 2.1 0.5B | ~400 MB | 68% single-turn | ~1 GB |
| Qwen2.5-1.5B-Instruct | ~1.2 GB | Good | ~2.5 GB |
| Hermes 2 Pro 7B | ~4.5 GB | 91% | ~6.5 GB |

**Dual-model strategy for Llamaste**:
- **Fast model** (0.5-1.5B): Handles simple tool calls ("list files", "check disk", "what's my IP"). Sub-200ms response.
- **Smart model** (7-14B): Handles complex reasoning, conversation, multi-step tasks. 1-3 second response.
- Agent routes to the appropriate model based on query complexity.

**Impact**: A 0.5B model generates tokens 5-10x faster than a 7B model on the same hardware. For simple tool dispatch, this is transformative.

---

### 5. Semantic Caching / Pattern Matching Before LLM

**What**: For common queries, skip the LLM entirely. Pattern-match the input and return a cached tool call.

**How**:
```
User: "what time is it" → CACHE HIT → tool_call(system.time) → skip LLM
User: "show disk space" → CACHE HIT → tool_call(fs.disk_usage) → skip LLM
User: "explain quantum physics" → CACHE MISS → send to LLM
```

**Implementation**: A simple prefix/keyword matching table for the most common OS operations. ~50 patterns cover 80% of system management queries. The LLM only handles novel or complex requests.

**Impact**: Instant response (<10ms) for cached patterns. Research shows 31% of LLM queries exhibit semantic similarity to previous requests.

**Library**: GPTCache provides production-grade semantic caching with embedding-based similarity matching.

---

### 6. Quantization Sweet Spots

**Current best options**:
| Format | Bits/Weight | Speed vs FP16 | Quality | Best For |
|--------|-------------|---------------|---------|----------|
| Q4_K_M | 4.5 | ~4x faster | 96-97% | Default, good balance |
| IQ3_S | ~3.4 | ~5x faster | 93-95% | RAM-constrained |
| Q2_K | 2.6 | ~6x faster | 85-90% | Extreme compression |
| BitNet 1.58 | 1.58 | multiply→add | ~90% | Future (needs trained models) |

**Recommendation**: Q4_K_M for the main model, Q8_0 for the fast tool-call model (small model, can afford higher quant).

---

### 7. Hardware-Specific Acceleration

**Intel AMX** (Xeon Sapphire Rapids+):
- 8x performance vs AVX-512 for INT8 operations
- LLaMA 3.2 3B INT8: 57 tokens/sec with AMX vs 28 without
- Available in server-class hardware

**AVX-512 VNNI** (Ice Lake+, Zen 4+):
- Significant boost for quantized inference
- Available on modern consumer CPUs

**ARM KleidiAI**:
- 20-40% improvement over NEON on newer ARM cores
- I8MM, SVE, SME instruction support

**For Llamaste**: `GGML_CPU_ALL_VARIANTS=ON` handles all of this automatically at build time. Runtime dispatch selects the best code path.

---

### 8. Context Length vs Speed

- Each doubling of context costs ~10-20% speed
- For OS tool calls, context is small (system prompt + recent conversation)
- Keep context window modest: 2K-4K for tool calls, 8K for longer conversations
- Pre-cached system prompt amortizes the context overhead

---

### 9. Thread Optimization for Tool-Call Latency

**Key insight**: For single-user OS interactions, optimize for latency not throughput.

```bash
# Fewer threads for generation (memory-bound, diminishing returns)
# More threads for prompt processing (compute-bound)
./llamaste -t 4 -tb 8  # 4 gen threads, 8 batch threads
```

- Use physical cores only (hyperthreading hurts memory-bound work)
- Pin threads to cores (avoid migration overhead)
- Leave 1-2 cores for the HTTP server and tool execution

---

## Expected Performance (Realistic Targets)

### With All Optimizations Applied

| Query Type | Technique | Expected Latency |
|-----------|-----------|-----------------|
| Cached OS command | Pattern match | <10 ms |
| Simple tool call (0.5B model) | Small model + grammar | 50-200 ms |
| Simple tool call (7B model) | KV cache + grammar + speculative | 200-500 ms |
| Complex reasoning (14B model) | Full inference | 1-3 seconds |
| Multi-step task (14B, 3+ tools) | Multiple inference rounds | 3-10 seconds |

### Without Optimizations (Baseline)

| Query Type | Expected Latency |
|-----------|-----------------|
| Any query (7B, no cache) | 1-3 seconds |
| Any query (14B, no cache) | 2-5 seconds |
| Multi-step task | 10-30 seconds |

**The optimization stack provides 5-10x improvement for common operations.**

---

## Implementation Priority for Llamaste

1. **KV cache reuse** — free, just structure the system prompt correctly
2. **Grammar-constrained tool output** — already in llama.cpp, just enable
3. **Semantic cache for common queries** — ~200 lines of code, huge impact
4. **Speculative decoding (n-gram)** — llama.cpp flag, no extra RAM
5. **Dual-model strategy** — Phase 2, needs model management logic
6. **Thread tuning** — boot-time auto-configuration
7. **Hardware-specific dispatch** — build flag, already planned

---

## Sources

- llama.cpp speculative decoding docs
- Meta's Efficient Speculative Decoding at Scale
- XGrammar: 100x structured output speedup
- Anthropic prompt caching (90% cost reduction)
- FunctionGemma (Google, 270M tool-calling model)
- GPTCache semantic caching
- Intel AMX benchmarks (57 t/s on 3B model)
- MLX on Apple M5 (4x TTFT improvement)
- On-Device LLMs 2026 state of the art
