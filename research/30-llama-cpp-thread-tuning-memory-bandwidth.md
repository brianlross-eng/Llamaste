# Research 30: llama.cpp Thread Tuning & Memory Bandwidth for CPU Inference

**Date**: 2026-04-03
**Purpose**: Optimal thread configuration, memory bandwidth analysis, and expected performance for Llamaste hardware targets.

---

## 1. Thread Count Tuning

### 1.1 n_threads vs n_threads_batch

llama-server exposes two separate thread parameters:

| Flag | Purpose | Phase |
|------|---------|-------|
| `-t, --threads N` | CPU threads during **token generation** (TG) | Autoregressive decode — one token at a time |
| `-tb, --threads-batch N` | CPU threads during **prompt processing** (PP) | Batch prefill — entire prompt in one pass |

**Why two parameters?** These are fundamentally different workloads:
- **Token generation (TG)** is **memory-bandwidth-bound**. Each token requires a full forward pass reading the entire model from RAM. Speed = `model_size / memory_bandwidth`.
- **Prompt processing (PP)** is **compute-bound** at large batch sizes. Multiple tokens processed simultaneously, so FLOPS matter more.

**Default**: If `--threads-batch` is not specified, it inherits the value of `--threads`. If `--threads` is not specified, it defaults to `std::thread::hardware_concurrency()` (all logical cores, which is often suboptimal).

### 1.2 Optimal Thread Count

**The universal rule: set --threads to the number of physical cores, NOT logical cores.**

Evidence from multiple sources:
- AMD Ryzen 7 3700X (8P/16T): performance peaks at exactly 8 threads. Beyond 8, performance degrades.
- Community benchmarks show a clear performance cliff when exceeding physical core count.
- As few as 5 threads can saturate dual-channel DDR4 memory bandwidth for TG workloads.

**For token generation (TG)**:
- Memory-bandwidth-bound — adding threads beyond what saturates your memory bandwidth provides zero benefit and adds synchronization overhead.
- On dual-channel DDR4: ~4-8 threads is usually enough.
- On dual-channel DDR5: ~6-10 threads.
- More threads = more cache thrashing, more barrier overhead.

**For prompt processing (PP)**:
- Compute-bound — can benefit from more threads, even HT/SMT threads.
- Set `--threads-batch` to physical core count or slightly higher (test with HT).
- Larger batch sizes help amortize overhead.

**Practical tuning method**:
1. Start with `--threads 1`, benchmark with `llama-bench`.
2. Double until tok/s stops improving.
3. That plateau is your sweet spot. Usually = physical cores.

### 1.3 Hyperthreading (Intel HT / SMT)

**Consensus: HT hurts token generation, may slightly help prompt processing.**

- **TG (generation)**: HT threads share the same physical core and its memory path. Since TG is memory-bound, HT adds contention without providing more bandwidth. Benchmarks consistently show performance drops of 5-15% when using HT threads for TG.
- **PP (prompt processing)**: Being compute-bound, HT can provide 5-20% improvement on PP since the second logical core can execute during stalls. Worth testing with `--threads-batch` set to logical core count.

**Recommendation for Llamaste**:
- `--threads` = physical P-cores only (no E-cores, no HT)
- `--threads-batch` = physical P-cores (test with +HT)

### 1.4 Intel Core Ultra 9 275HX Specifics (Arrow Lake-HX)

- **Architecture**: 8 P-cores (HT) + 16 E-cores = 24 cores / 24 threads (P-cores have HT but Arrow Lake HX reports 24T total)
- **Note**: Arrow Lake desktop dropped HT on P-cores. Arrow Lake-HX (mobile) may vary — check `lscpu` output.
- **Recommended starting point**: `--threads 8` (P-cores only for TG), `--threads-batch 16-24` (test).
- P-cores have significantly higher IPC and AVX2/AVX-512 support than E-cores. E-cores lack AVX-512 entirely. Mixing P+E cores for SIMD-heavy inference can cause performance cliffs (kernel has to use the lowest common denominator instruction set, or suffer core migration penalties).
- **Best approach**: Pin llama-server to P-cores only using `taskset` or cgroup cpuset.

### 1.5 i5-1035G1 Specifics (Ice Lake)

- **Architecture**: 4 P-cores / 8 threads (HT), no E-cores
- **Recommended**: `--threads 4` (physical cores), `--threads-batch 4`
- **Already confirmed**: ~14 tok/s on 3B Q4_K_M with GGML_NATIVE=ON (AVX2)
- **HT test**: Try `--threads 4` vs `--threads 8` — expect 4 to win for TG.

---

## 2. Memory Bandwidth

### 2.1 Why Memory Bandwidth Dominates

For autoregressive token generation, each token requires reading the **entire model weights** from RAM. The formula:

```
theoretical_max_tok_s = memory_bandwidth_GB_s / model_size_GB
```

This is the hard ceiling. No amount of CPU optimization can exceed it. AVX2/AVX-512 helps with the compute side (dequantization, matmul) but the data must still flow through the memory bus.

### 2.2 72B Q4_K_M: Memory-Bound Analysis

| Metric | Value |
|--------|-------|
| 72B parameter model at Q4_K_M | ~42-44 GB on disk/RAM |
| KV cache overhead (2048 ctx) | ~1-2 GB |
| Total RAM needed | ~48-50 GB minimum (with OS overhead) |
| Theoretical max tok/s (102.4 GB/s BW) | 102.4 / 43 ≈ **2.4 tok/s** |
| Theoretical max tok/s (38.4 GB/s DDR4) | 38.4 / 43 ≈ **0.9 tok/s** |

**72B Q4_K_M is definitively memory-bandwidth-bound for TG**. Compute requirements are easily met by modern CPUs with AVX2. The bottleneck is purely how fast you can stream 43GB of weights per token.

### 2.3 Memory Bandwidth by Configuration

| Config | Theoretical Peak | Realistic (~70-80%) |
|--------|-----------------|---------------------|
| DDR4-3200 single-channel | 25.6 GB/s | ~18-20 GB/s |
| DDR4-3200 dual-channel | 51.2 GB/s | ~36-40 GB/s |
| DDR5-4800 single-channel | 38.4 GB/s | ~27-30 GB/s |
| DDR5-4800 dual-channel | 76.8 GB/s | ~54-60 GB/s |
| DDR5-5600 dual-channel | 89.6 GB/s | ~63-70 GB/s |
| DDR5-6400 dual-channel | 102.4 GB/s | ~72-80 GB/s |
| LPDDR5x-7500 (laptop) | 120 GB/s | ~84-96 GB/s |

**Critical insight**: Dual-channel vs single-channel is a 2x difference. A single DIMM in a dual-channel board runs at HALF bandwidth. This is the single biggest performance variable for CPU inference.

### 2.4 Detecting Memory Configuration on Linux

```bash
# 1. dmidecode — shows DIMM slots, speed, type (requires root)
dmidecode -t memory
# Look for: "Type: DDR5", "Speed: 6400 MT/s", "Locator:" fields
# Count populated slots — 2 DIMMs in alternating channels = dual-channel

# 2. Infer channel config from populated DIMM slots
dmidecode -t 17 | grep -E "Locator:|Speed:|Type:"
# If DIMMs in both Channel A and Channel B → dual-channel

# 3. lshw (alternative)
lshw -class memory

# 4. Practical bandwidth test — use mbw or STREAM benchmark
apt install mbw
mbw -n 5 256  # Copy 256MB, 5 iterations — shows actual bandwidth

# 5. /proc/meminfo — total RAM only (no channel info)
cat /proc/meminfo | head -5

# 6. dmesg — may show memory controller info at boot
dmesg | grep -i "memory\|ddr\|channel"
```

**Note**: Linux has no direct `/proc` or `/sys` file that reports "dual-channel" vs "single-channel". This is a memory controller configuration, not an OS concept. `dmidecode` + slot population analysis is the most reliable method. The STREAM or `mbw` benchmark gives you the actual achieved bandwidth, which is what matters.

### 2.5 NUMA Considerations

**Consumer desktops: NUMA does NOT apply.** All Intel consumer chips (including Core Ultra 9 275HX) and AMD consumer chips (Ryzen series) use a single memory controller with uniform memory access. NUMA only applies to:
- Server platforms (Xeon, EPYC) with multiple sockets
- AMD Threadripper (multiple CCDs with separate memory controllers)
- Very large HEDT configurations

**For Llamaste**: Ignore NUMA entirely. The `--numa` flag in llama-server is irrelevant for consumer hardware. Do NOT set it.

---

## 3. Expected Performance

### 3.1 Core Ultra 9 275HX (Arrow Lake-HX)

| Model | Quant | Expected TG tok/s | Notes |
|-------|-------|--------------------|-------|
| 3B (Qwen2.5) | Q4_K_M | ~25-35 | Model fits in cache, compute-limited |
| 7B | Q4_K_M | ~15-20 | Near cache boundary |
| 14B | Q4_K_M | ~8-12 | Memory-bound starts dominating |
| 72B | Q4_K_M | ~1.5-2.3 | Hard memory-bound: 102.4 GB/s / 43 GB ≈ 2.4 theoretical max |

**Assumptions**: DDR5-6400 dual-channel (102.4 GB/s peak), GGML_NATIVE=ON (AVX2/AVX-512), `--threads 8` (P-cores), 64+ GB RAM.

**No published 275HX benchmarks found** as of 2026-04. These are estimates based on the memory bandwidth formula and comparable Arrow Lake results. The 275HX should perform very similarly to any other dual-channel DDR5-6400 platform for bandwidth-bound models.

### 3.2 i5-1035G1 (Ice Lake, 4C/8T, DDR4)

| Model | Quant | Measured tok/s | Notes |
|-------|-------|----------------|-------|
| 1.5B (Qwen2.5) | Q4_K_M | ~14 | Confirmed on VivoBook (GGML_NATIVE=ON) |
| 3B (Qwen2.5) | Q4_K_M | ~14 | Confirmed on VivoBook |
| 14B (Qwen2.5) | Q4_K_M | ~2.0-2.2 | Confirmed on VivoBook |
| 72B | Q4_K_M | N/A | Requires 48+ GB RAM — VivoBook has 36 GB, cannot run |

**Memory bandwidth**: DDR4-3200 dual-channel = 51.2 GB/s theoretical, ~36-40 GB/s real. The 14B (~8 GB) theoretical max is ~51.2/8 ≈ 6.4 tok/s, but the measured 2.0-2.2 suggests either single-channel DDR4, lower clock, or significant overhead. Worth verifying DIMM configuration with `dmidecode`.

### 3.3 Community Benchmark Reference Points

From llama.cpp discussions and community benchmarks:
- **EPYC 7502P 32C** (8-channel DDR4-3200 = 204.8 GB/s): 13B Q4_0 → 7.85 tok/s TG
- **Ryzen 7 3700X 8C** (dual DDR4-3200): 7B Q4_0 → ~14 tok/s TG
- **M1 Pro** (200 GB/s unified): 7B Q4_0 → 35 tok/s TG — shows bandwidth dominance
- **M1 Max** (400 GB/s unified): 7B Q4_0 → 54 tok/s TG
- **Threadripper** with 4-channel: 34B Q4_0 → 1.5 tok/s (1 DIMM) → 4 tok/s (2 DIMMs) — channel count directly scales performance

---

## 4. llama-server Configuration for Speed

### 4.1 Flags That Affect Inference Speed

| Flag | Effect | Recommendation |
|------|--------|----------------|
| `-t, --threads N` | TG threads | Physical P-cores only |
| `-tb, --threads-batch N` | PP threads | Physical P-cores (test with HT) |
| `-c, --ctx-size N` | Context window | Smaller = less KV cache RAM, marginal speed gain. 2048-4096 for single-user |
| `-fa, --flash-attn` | Flash Attention | Reduces KV cache memory ~50%. Marginal speed improvement on CPU. Enable it. |
| `-b, --batch-size N` | Prompt processing batch | Default 2048. Lower for less RAM. Higher doesn't help much on CPU. |
| `-ub, --ubatch-size N` | Micro-batch for PP | Default 512. Controls granularity within batch. Leave default. |
| `--cache-type-k, --cache-type-v` | KV cache quantization | `q8_0` or `q4_0` saves RAM, tiny quality impact. Useful for large contexts. |
| `-ngl, --n-gpu-layers N` | GPU offload | CPU-only: set to 0 or omit |
| `--mlock` | Lock model in RAM | Prevents swapping. Use if RAM is tight. |
| `-np, --parallel N` | Concurrent slots | 1 for single-user (Llamaste default). More slots = more KV cache RAM. |
| `--no-mmap` | Disable memory mapping | Sometimes faster on some systems. Test both. |

### 4.2 Does Context Size Affect tok/s?

**For TG: minimal impact.** Each generated token still reads the full model weights regardless of context length. The KV cache lookup adds some overhead that scales with context length (quadratic attention), but this is tiny compared to the model weight read.

**For PP: yes, significantly.** Larger contexts during prompt processing take proportionally longer (and superlinearly due to attention scaling). Flash attention (`--flash-attn`) mitigates the quadratic scaling.

**For RAM**: Context size directly determines KV cache size. For 72B at fp16 KV:
- 2048 ctx ≈ 1.3 GB KV cache
- 4096 ctx ≈ 2.6 GB KV cache
- 8192 ctx ≈ 5.2 GB KV cache

With `--cache-type-k q8_0 --cache-type-v q8_0`, halve these numbers.

### 4.3 Recommended Llamaste Server Config

**For Core Ultra 9 275HX (72B Q4_K_M, 64 GB RAM)**:
```
--threads 8 --threads-batch 8 --ctx-size 2048 --flash-attn --cache-type-k q8_0 --cache-type-v q8_0 --parallel 1
```

**For i5-1035G1 (3B Q4_K_M, 36 GB RAM)**:
```
--threads 4 --threads-batch 4 --ctx-size 4096 --flash-attn --parallel 1
```

---

## 5. Key Takeaways for Llamaste

1. **Thread auto-detection is wrong**: `hardware_concurrency()` returns logical cores (including HT and E-cores). Llamaste should detect physical P-core count and set threads explicitly.

2. **How to detect physical cores on Linux**:
   ```bash
   # Physical cores only (no HT)
   grep -c '^processor' /proc/cpuinfo  # logical cores
   grep '^cpu cores' /proc/cpuinfo | head -1  # physical per socket
   lscpu | grep "Core(s) per socket"

   # P-cores vs E-cores (Intel hybrid)
   # Check /sys/devices/system/cpu/cpu*/topology/cluster_id
   # or parse lscpu for different MHz tiers
   ```

3. **Memory bandwidth is king**: For models > 7B, the CPU is almost irrelevant. What matters is RAM bandwidth. Dual-channel DDR5-6400 (102.4 GB/s) is ~2x faster than DDR4-3200 dual-channel (51.2 GB/s) for inference.

4. **72B on 275HX is viable but slow**: ~1.5-2.3 tok/s expected. Usable for batch/async tasks. Not great for interactive chat. The cluster feature (splitting across 2-3 nodes) would make this practical.

5. **Verify VivoBook memory config**: The 2.0-2.2 tok/s on 14B seems low for dual-channel DDR4. Run `dmidecode -t 17` to check if it's actually running single-channel.

6. **Flash attention is free performance**: Always enable `--flash-attn` on CPU. Reduces memory, doesn't hurt speed.

---

## Sources

- [llama.cpp Discussion #4167 — Apple Silicon Performance](https://github.com/ggml-org/llama.cpp/discussions/4167) (ggerganov, memory bandwidth vs TG analysis)
- [llama-server README](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md) (official flags documentation)
- [llama.cpp Discussion #3167 — CPU Performance](https://github.com/ggml-org/llama.cpp/discussions/3167) (EPYC benchmark data)
- [llama.cpp Performance Tips](https://github.com/ggml-org/llama.cpp/blob/master/docs/development/token_generation_performance_tips.md) (official thread oversaturation guidance)
- [Optimization Guide](https://notes.suhaib.in/docs/tech/latest/cracking-the-code-of-llamacpp-optimizing-threads-batch-size-and-context-for-peak-performance/) (threads vs bandwidth, batch size, context length)
- [Intel Core Ultra 9 275HX Specs](https://www.intel.com/content/www/us/en/products/sku/242293/intel-core-ultra-9-processor-275hx-36m-cache-up-to-5-40-ghz/specifications.html) (DDR5-6400, 102.4 GB/s, dual-channel)
- [OpenBenchmarking llama.cpp](https://openbenchmarking.org/test/pts/llama-cpp) (community CPU benchmarks)
- [Clarifai llama.cpp Guide](https://www.clarifai.com/blog/ilama.cpp) (performance tuning overview)
- [Threadripper CPU-Only Benchmarks](https://www.thedroptimes.com/55298/cpu-only-llm-inference-threadripper-sergiu-nagailic-benchmarks-llamacpp-performance) (channel scaling data)
- [Justine Tunney — LLaMA Now Goes Faster on CPUs](https://justine.lol/matmul/) (CPU matmul optimization, llamafile)
