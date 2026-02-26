# CPU Speed Optimization Projects for LLM Inference

## Overview

This document covers projects, techniques, and kernel-level optimizations that significantly improve CPU-based LLM inference speed. While GPU inference dominates in raw throughput, CPU inference matters for Llamaste because the target audience is consumer hardware without dedicated GPUs or with limited VRAM.

---

## 1. llamafile (Mozilla / Justine Tunney)

### What It Is
llamafile packages a llama.cpp-based inference engine into a single executable that runs on any x86-64 or ARM64 system without installation. The key innovation is **Cosmopolitan Libc**, which produces fat binaries containing code for Windows, macOS, Linux, FreeBSD, and OpenBSD simultaneously.

### Key Technical Innovations

#### Cosmopolitan Libc
- Single binary runs on 6 operating systems
- APE (Actually Portable Executable) format: valid as both ELF and PE
- ~500 KB overhead for cross-platform capability
- Binary starts as a shell script that re-execs itself correctly on each OS

#### tinyBLAS Custom Kernels
The most impactful contribution. Justine Tunney wrote hand-optimized matrix multiplication kernels that:
- **Beat Intel MKL by 2x** on many workloads
- Use **register blocking** to keep data in CPU registers across inner loops
- Exploit **L2 cache geometry** to tile matrices for optimal cache residency
- Implement **software prefetching** to hide memory latency
- Use **branch-free decoding** for quantized formats to avoid branch misprediction

#### mixmul Dispatcher
```
mixmul(A_quantized, B_float) -> C_float
```
- Dispatches to the best kernel based on quantization format + CPU features at runtime
- Separate optimized paths for Q4_0, Q4_K, Q5_K, Q6_K, Q8_0, IQ formats
- Each path has AVX, AVX2, AVX-512 variants
- ARM paths use NEON with optional dot product and I8MM instructions

### Performance Impact
- 10-50% faster than stock llama.cpp on x86-64 with AVX2
- Up to 2x faster than MKL-based builds on certain quantization formats
- Smaller binary than llama.cpp + system BLAS dependency

### Relevance to Llamaste
llamafile's kernel optimizations have been partially upstreamed to ggml. The architecture of runtime CPU dispatch aligns with Llamaste's `GGML_CPU_ALL_VARIANTS=ON` strategy.

---

## 2. ggml CPU Internals

### Block Quantization Decode-on-the-Fly

ggml's fundamental CPU optimization: quantized weights are **never** fully dequantized into memory. Instead, each dot product kernel:

1. Loads a block of quantized weights (e.g., 32 values in Q4_0)
2. Decodes scales and values into registers
3. Computes partial dot product in registers
4. Accumulates and moves to next block

This means the memory bandwidth consumed is proportional to the quantized size, not FP32 size.

#### Q4_0 AVX2 Inner Loop (Conceptual)
```c
// For each block of 32 weights:
// 1. Load 16 bytes of Q4_0 data (32 x 4-bit values)
// 2. Split into low/high nibbles using bit manipulation
// 3. Subtract zero point (8) to get signed values
// 4. Use _mm256_maddubs_epi16 for uint8 x int8 multiply-add
// 5. Horizontal sum and accumulate with scale factor
```

The key intrinsic `_mm256_maddubs_epi16` performs 32 multiply-adds in a single instruction, making INT8/INT4 dot products extremely efficient on modern CPUs.

#### ARM NEON Equivalent
```c
// ARM uses vdotq_s32 (dot product instruction)
// Available on Cortex-A76+ and all Apple Silicon
// Computes 4 dot products of 4 int8 values each = 16 multiply-adds
// Combined with vld1q to stream quantized data from memory
```

### K-Quant Decode Paths

K-quants (Q4_K_M, Q5_K_M, Q6_K) use a more complex block structure:
- **Super-blocks** containing multiple sub-blocks
- Per-sub-block scales and mins (higher precision than Q4_0)
- More complex decode, but better quality per bit

The decode kernels for K-quants are significantly more code than Q4_0 but still operate entirely in registers.

### IQ-Quant (Importance Matrix) Paths

IQ formats (IQ2_XXS, IQ3_M, IQ4_NL) use:
- **Lookup tables** for codebook-based quantization
- Importance-weighted quantization (more bits for important weights)
- Excellent quality at very low bit rates (2-3 bits)
- Slightly slower decode due to table lookups, but much better quality than K-quants at same bit rate

---

## 3. KleidiAI (ARM)

### What It Is
ARM's official optimized micro-kernel library for AI workloads on ARM CPUs.

### Key Features
- **I8MM instructions** (ARMv8.6): 8x8 int8 matrix multiply in hardware
- **SVE/SVE2** support: Scalable Vector Extension for variable-width SIMD
- **SME** (Scalable Matrix Extension): Matrix tile operations on newest ARM CPUs
- Hand-tuned kernels for specific Cortex-A and Cortex-X cores

### Performance
- 20-40% improvement over default NEON kernels in llama.cpp
- Greatest impact on newer ARM cores (Cortex-X4, Apple M3+)
- Being integrated into ggml as the preferred ARM backend

### Relevance to Llamaste
ARM support is Phase 4 of Llamaste. When ARM images are built, KleidiAI kernels (via ggml integration) will provide significant speedups on Raspberry Pi 5, Orange Pi, and other ARM SBCs.

---

## 4. Intel oneAPI / oneDNN

### AMX (Advanced Matrix Extensions)
- Available on Sapphire Rapids+ (Xeon 4th/5th gen, 2023+)
- **Tile-based operations**: Load 2D tiles into dedicated registers
- INT8 and BF16 tile matrix multiply
- 4-6x improvement for batched inference over AVX-512

### VNNI (Vector Neural Network Instructions)
- Available on Ice Lake+ (2019+)
- `vpdpbusd`: 4 INT8 multiply-adds per element per clock
- Significant improvement for quantized inference
- Supported in ggml CPU backend

### JIT Kernel Compilation
oneDNN uses runtime JIT compilation to generate optimal kernels for the specific CPU:
- Detects exact CPU model and cache sizes
- Generates kernels tailored to available ISA extensions
- Caches compiled kernels for reuse

### Relevance to Llamaste
Intel-specific optimizations are handled by `GGML_CPU_ALL_VARIANTS=ON`, which includes VNNI and AMX dispatch paths. No special integration needed.

---

## 5. PowerInfer

### Concept: Hot/Cold Neuron Partitioning
PowerInfer exploits **activation sparsity** in LLMs: during inference, only a fraction of neurons in each layer are actually "hot" (activated above threshold). The rest contribute minimally.

### Architecture
1. **Offline profiling**: Run calibration dataset through model, record per-neuron activation frequency
2. **Hot neurons** (frequently activated): Preloaded into GPU VRAM
3. **Cold neurons** (rarely activated): Stay in CPU RAM
4. **Adaptive predictor**: Lightweight classifier predicts which neurons will activate for each token
5. **Sparse computation**: Only compute hot neurons on GPU, cold on CPU (or skip)

### Performance Claims
- 11x speedup over llama.cpp on Falcon 40B with RTX 4090
- 70% of neurons in typical LLM are "cold" (activated <10% of the time)
- Works best with MoE models (Mixtral) where sparsity is structural

### Limitations
- Requires per-model profiling (offline step)
- Benefit varies greatly by model architecture
- Best for models with natural activation sparsity
- Less applicable to smaller models (1.5B-7B) where sparsity is lower

### Relevance to Llamaste
Not directly applicable for Phase 1-2 (pure CPU inference). Could be explored in Phase 3+ for hybrid GPU+CPU setups where VRAM is limited.

---

## 6. Speculative Decoding

### Concept
Use a small, fast "draft" model to propose multiple tokens, then verify them in parallel with the large "target" model.

### How It Works
1. Draft model (e.g., 1.5B) generates K candidate tokens autoregressively (fast)
2. Target model (e.g., 14B) evaluates all K tokens in a single forward pass (batch)
3. Accept tokens where draft and target agree (typically 60-80%)
4. Reject and resample from target distribution where they disagree
5. Net effect: multiple tokens per target forward pass

### Performance
- 2-3x speedup for generation (not prompt processing)
- Speedup ratio depends on acceptance rate (domain-specific text = higher acceptance)
- Draft model must be architecturally compatible (same tokenizer preferred)
- Diminishing returns beyond K=4-5 draft tokens

### llama.cpp Implementation
```bash
# Speculative decoding with draft model
./llama-cli -m large_model.gguf \
  --model-draft small_model.gguf \
  -np 4 \    # Number of draft tokens
  -ngd 0     # Draft model on CPU (no GPU layers)
```

### Llamaste-Specific Pairings
| Target Model | Draft Model | Expected Speedup |
|-------------|------------|-----------------|
| Qwen2.5 14B Q4_K_M | Qwen2.5 1.5B Q4_K_M | 2-2.5x |
| Qwen2.5 7B Q4_K_M | Qwen2.5 1.5B Q4_K_M | 1.5-2x |
| Qwen2.5 32B Q4_K_M | Qwen2.5 7B Q4_K_M | 2-3x |

### Relevance to Llamaste
High priority for Phase 2+. Llamaste could ship draft models alongside target models and auto-configure speculative decoding when sufficient RAM is available.

---

## 7. Flash Attention on CPU

### Problem
Standard attention computes full N x N attention matrix, requiring O(N^2) memory. For context length 8192, that's 64M entries per head.

### Flash Attention Solution
Tile the attention computation:
1. Process query/key/value in blocks that fit in L2 cache
2. Compute partial softmax within each tile
3. Combine partial results using online softmax algorithm
4. Never materialize the full attention matrix

### CPU Implementation in ggml
- `GGML_OP_FLASH_ATTN_EXT`: Fused QKV attention operation
- Operates on tiled blocks sized for L2 cache
- Supports FP16, FP32, and quantized KV cache
- Significant memory reduction: O(N) instead of O(N^2)
- Speed improvement from better cache utilization

### Impact
- Enables longer context on RAM-limited systems
- 20-40% faster attention computation on CPU
- Critical for 16K+ context lengths

---

## 8. bitnet.cpp (Microsoft)

### Concept: 1-Bit (Ternary) Models
Instead of using floating-point weights, BitNet models use ternary weights: {-1, 0, +1}.

### Revolutionary Implication
Matrix multiplication becomes **addition and subtraction**:
```
y = W * x  where W ∈ {-1, 0, +1}

// Traditional: multiply + accumulate
for each weight w, input x:
    result += w * x    // Expensive multiplication

// BitNet: conditional add/subtract
for each weight w, input x:
    if w == 1:  result += x
    if w == -1: result -= x
    // w == 0: skip
```

### Performance
- 1.37-5.07x faster than Q4_0 llama.cpp (CPU only)
- 55-70% less energy consumption
- Models fit in ~1/4 the memory of Q4_0
- **No GPU needed** -- CPU becomes competitive with GPU inference

### Current Status (Early 2025)
- Requires models trained specifically with BitNet architecture
- Not a post-training quantization -- must be trained ternary from scratch
- Microsoft released bitnet.cpp runtime
- Kernel support in ggml for ARM (lookup table approach) and x86 (TL1/TL2 encoding)

### Relevance to Llamaste
Long-term high impact. When quality ternary models become available at 7B+ scale, Llamaste could offer them as the default for CPU-only systems. The speed and memory benefits would transform the user experience.

---

## 9. Memory-Mapped (mmap) Optimizations

### mmap Model Loading in llama.cpp
Models are loaded via `mmap()` by default, which provides:
- **Zero-copy loading**: OS maps file pages directly into process address space
- **Demand paging**: Only pages actually accessed are loaded from disk
- **Shared pages**: Multiple processes using same model share physical pages
- **No malloc overhead**: Avoids heap fragmentation

### Optimization Techniques

#### madvise Hints
```c
// Tell kernel about access patterns
madvise(model_data, model_size, MADV_SEQUENTIAL);   // During initial load
madvise(model_data, model_size, MADV_RANDOM);        // During inference
madvise(model_data, model_size, MADV_WILLNEED);      // Prefetch all pages
```

#### Huge Pages for mmap
```bash
# Enable huge pages for mmap regions
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled
# Then in code:
madvise(model_data, model_size, MADV_HUGEPAGE);
```
- 5-15% improvement from reduced TLB pressure
- 7B Q4_K_M model = ~1M 4KB pages vs ~2K 2MB huge pages

#### Software Prefetching
```c
// Prefetch next layer's weights while computing current layer
__builtin_prefetch(next_layer_weights, 0, 1);  // Read, low temporal locality
```

#### mlock (Prevent Swapping)
```bash
# llama.cpp flag
./llama-server -m model.gguf --mlock
```
- Locks all model pages in physical RAM
- Prevents OS from swapping model to disk under memory pressure
- Critical for consistent inference latency

---

## 10. Thread Optimization

### Physical vs Logical Cores
For memory-bandwidth-bound inference, **use physical cores only**:
```bash
# Detect physical core count
PHYS=$(lscpu -p=CORE | grep -v '#' | sort -u | wc -l)

# Run with physical cores minus 1 (leave 1 for OS)
./llama-server -m model.gguf -t $((PHYS - 1))
```

SMT (HyperThreading) adds no benefit for memory-bound workloads and can cause cache contention.

### Thread Affinity
```bash
# Pin to specific cores (avoid core migration)
taskset -c 0-7 ./llama-server -m model.gguf -t 8

# Or use NUMA binding
numactl --cpunodebind=0 --membind=0 ./llama-server -m model.gguf -t 8
```

### Batch vs Generation Threading
- **Prompt processing** (batch): Benefits from all physical cores (compute-bound)
- **Token generation**: Benefits from fewer threads (memory-bound)
- llama.cpp `-t` sets generation threads, `-tb` sets batch threads

```bash
# Optimal: fewer threads for generation, all cores for batch
./llama-server -m model.gguf -t 4 -tb 8
```

---

## 11. Practical Speed Optimization Summary for Llamaste

### Priority Order (Highest Impact First)

| Priority | Optimization | Impact | Complexity |
|----------|-------------|--------|------------|
| 1 | Right model size for RAM | 2-10x | None (auto-detect) |
| 2 | Q4_K_M quantization (default) | Baseline | None |
| 3 | Physical cores only, correct thread count | 10-30% | Low (auto-detect) |
| 4 | CPU governor = performance | 5-20% | Low (sysctl) |
| 5 | GGML_CPU_ALL_VARIANTS dispatch | 10-50% | Build flag |
| 6 | mlock model in RAM | Consistent latency | Flag |
| 7 | Huge pages (THP madvise) | 5-15% | Kernel config |
| 8 | Speculative decoding | 2-3x generation | Medium (needs draft model) |
| 9 | NUMA binding (multi-socket) | 20-40% | Medium |
| 10 | Disable deep C-states | 5-10% | Kernel config |

### Llamaste Auto-Tuning Script (Conceptual)
```bash
#!/bin/sh
# Phase 2: Auto-detect and optimize

# 1. Detect hardware
PHYS_CORES=$(lscpu -p=CORE | grep -v '#' | sort -u | wc -l)
TOTAL_RAM_GB=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
NUMA_NODES=$(lscpu | awk '/NUMA node\(s\)/{print $3}')

# 2. Set CPU governor
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$gov" 2>/dev/null
done

# 3. Enable THP (madvise mode)
echo madvise > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null

# 4. Set thread counts
GEN_THREADS=$((PHYS_CORES - 1))
BATCH_THREADS=$PHYS_CORES

# 5. NUMA optimization
if [ "$NUMA_NODES" -gt 1 ]; then
    NUMA_PREFIX="numactl --interleave=all"
else
    NUMA_PREFIX=""
fi

# 6. Select model
# (model selection logic from 07-model-selection.md)

# 7. Launch
$NUMA_PREFIX ./llama-server \
    -m "$MODEL_PATH" \
    -t "$GEN_THREADS" \
    -tb "$BATCH_THREADS" \
    --mlock \
    --host 0.0.0.0 \
    --port 8080
```

---

## 12. Future Directions

### BitNet Mass Adoption
When ternary models reach quality parity with FP16 at 7B+ scale, CPU inference will approach real-time conversational speed even on modest hardware. This could eliminate the need for GPU entirely for most consumer use cases.

### Sparse Computation
Models like Mixtral (MoE) only activate a fraction of parameters per token. Future optimizations may skip loading inactive expert weights entirely, reducing memory bandwidth requirements.

### Hardware Evolution
- DDR5 bandwidth improving ~20% per generation
- ARM SVE2 and SME providing wider SIMD without the power penalty of AVX-512
- AMD Zen 5 bringing wider AVX-512 execution
- Intel AMX expanding beyond server to client CPUs
