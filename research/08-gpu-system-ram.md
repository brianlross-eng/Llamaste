# GPU + System RAM Techniques for LLM Inference

## Overview

This document covers techniques for using GPU compute with system RAM (not just VRAM) for LLM inference. This is critical when models exceed available VRAM, when using integrated GPUs with shared memory, or when optimizing cost/performance tradeoffs on consumer hardware.

---

## 1. Partial GPU Offloading in llama.cpp (-ngl)

### How It Works

The `-ngl` (number of GPU layers) flag controls how many transformer layers are placed on GPU vs CPU. A typical LLM has a structure like:

```
[Token Embedding] -> [Layer 0] -> [Layer 1] -> ... -> [Layer N-1] -> [Output Head]
```

When you set `-ngl K`, the first K layers (starting from layer 0) are placed on GPU, and the remaining layers stay on CPU. The token embedding and output head have their own placement logic (output head goes to GPU if all layers are on GPU).

### Layer Anatomy

Each transformer layer contains these tensors:
- **Attention weights**: Q, K, V projection matrices + output projection (4 matrices)
- **MLP/FFN weights**: gate, up, and down projection matrices (3 matrices for LLaMA-style)
- **Normalization**: RMSNorm/LayerNorm weights (small)
- **KV cache**: per-layer key and value cache for the context window

For a 7B model (32 layers, Q4_K_M):
- Each layer is approximately **~130 MB** of weights
- Total model weights: ~4.2 GB
- KV cache: additional memory proportional to context length

### Memory Calculation for Partial Offload

```
GPU VRAM needed = (ngl layers * per_layer_weight_size) + (ngl layers * kv_cache_per_layer) + compute_buffers
CPU RAM needed  = ((total_layers - ngl) * per_layer_weight_size) + ((total_layers - ngl) * kv_cache_per_layer) + embedding + output_head
```

### Performance Implications

Partial offload creates a **pipeline** where data must transfer between CPU and GPU at layer boundaries:

1. **GPU-only layers**: Fast matrix multiplications using GPU tensor cores / shader units
2. **CPU-only layers**: Slower but benefits from SIMD (AVX2/AVX-512)
3. **Transfer overhead**: At each CPU<->GPU boundary, activations must be copied across PCIe

**The key performance equation:**
```
time_per_token = max(gpu_compute_time, cpu_compute_time) + transfer_overhead
```

Since layers execute sequentially (pipeline parallelism within a single forward pass), the bottleneck is the **slowest segment** plus transfer costs.

### Performance Cliff Pattern

There is a characteristic performance curve:

| ngl Setting | Behavior | Typical Speed (7B Q4, RTX 3060 12GB) |
|-------------|----------|---------------------------------------|
| 0 | All CPU | 8-15 tok/s |
| 10 | Mixed (mostly CPU) | 12-20 tok/s |
| 20 | Mixed (balanced) | 18-30 tok/s |
| 33 (all layers) | Full GPU | 60-100+ tok/s |

The transition is **not linear**. You get:
- Modest gains moving the first few layers to GPU
- Accelerating gains as more layers move
- A large jump when ALL layers fit on GPU (eliminates CPU<->GPU transfers entirely)

### Which Layers Benefit Most From GPU

All transformer layers are structurally identical in standard architectures (LLaMA, Mistral, Qwen), so no specific layer inherently benefits more from GPU placement. However:

1. **Attention layers** benefit from GPU due to the O(n^2) attention computation scaling with context length
2. **MLP/FFN layers** are pure matrix multiplications -- the bread-and-butter of GPU compute
3. **First/last layers** have slightly different behavior (embedding lookup, logit computation)

**Practical advice**: Maximize ngl to fill available VRAM. Every layer on GPU helps, but the biggest win is getting ALL layers on GPU.

### Optimal -ngl Strategy

```bash
# Let llama.cpp auto-calculate (recent versions)
llama-server -m model.gguf -ngl 99  # Will use max that fits in VRAM

# Manual calculation
VRAM_AVAILABLE_MB=6000  # Actual free VRAM
PER_LAYER_MB=130        # For 7B Q4_K_M
BUFFER_MB=500           # Compute buffers, KV cache overhead
NGL=$((($VRAM_AVAILABLE_MB - $BUFFER_MB) / $PER_LAYER_MB))
```

---

## 2. Unified Memory Architecture (UMA)

### Apple Silicon (M1/M2/M3/M4)

Apple Silicon uses a true unified memory architecture where CPU and GPU share the same physical memory pool with a single address space.

**Key characteristics:**
- No PCIe transfer bottleneck -- CPU and GPU access the same memory directly
- Memory bandwidth shared between CPU and GPU
- M1: 68.25 GB/s, M1 Pro/Max: 200-400 GB/s, M2 Ultra: 800 GB/s, M3 Max: 400 GB/s, M4 Pro: 273 GB/s
- All system RAM is effectively "VRAM"
- A 64GB M2 Max can load a 64GB model entirely "on GPU" via Metal

**Performance implications for LLM inference:**
- No penalty for "GPU accessing system RAM" because it IS the same RAM
- Memory bandwidth is the bottleneck (same as CPU inference)
- M1 Pro 32GB can run 32B Q4_K_M models at full GPU speed
- M2 Ultra 192GB can run 70B models at FP16
- Token generation speed = memory_bandwidth / model_size (same formula as CPU)
- Prompt processing benefits enormously from GPU cores (parallel matrix multiply)

**Benchmark ballpark (M2 Max 96GB, llama.cpp Metal):**
| Model | Prompt (tok/s) | Generation (tok/s) |
|-------|---------------|-------------------|
| 7B Q4_K_M | 300-500 | 35-50 |
| 13B Q4_K_M | 150-300 | 20-30 |
| 70B Q4_K_M | 20-40 | 5-10 |

### AMD APUs (Shared Memory)

AMD APUs (Ryzen with integrated Radeon graphics) share system RAM between CPU and GPU.

**Key characteristics:**
- GPU uses a portion of system RAM as VRAM (configurable in BIOS, typically 512MB-4GB)
- Can be expanded via Resizable BAR / Smart Access Memory
- Bandwidth is limited by system DDR (DDR5-5600 dual channel = ~89 GB/s shared)
- Ryzen 7000/8000 series APUs: RDNA 3 integrated graphics
- ROCm support is limited for APUs (Vulkan backend preferred in llama.cpp)

**Performance characteristics:**
- Significantly lower bandwidth than Apple Silicon (DDR5 dual-channel vs Apple's wide bus)
- GPU compute can still accelerate prompt processing
- For generation, memory bandwidth is the limit regardless
- AMD Ryzen AI 300 series (Strix Point) has larger GPU (RDNA 3.5, up to 16 CUs) with better VRAM allocation

**Practical usage with llama.cpp:**
```bash
# Use Vulkan backend with AMD APU
cmake -B build -DGGML_VULKAN=ON
./llama-server -m model.gguf -ngl 99  # Vulkan will use shared memory
```

### Intel Integrated Graphics

Intel's integrated GPUs (UHD, Iris, Arc integrated) also share system memory.

**Key characteristics:**
- Share system DDR bandwidth
- Intel Xe-LPG/HPG iGPUs in Meteor Lake/Arrow Lake
- SYCL/oneAPI backend support in llama.cpp
- Limited compute capability compared to discrete GPUs
- Can provide modest acceleration for prompt processing

**Practical note:** Intel integrated GPUs generally don't provide enough compute advantage over well-optimized CPU inference (with AVX2/AVX-512) to justify the integration complexity. The CPU backend with SIMD is usually better.

---

## 3. CUDA Unified Memory

### How It Works

NVIDIA's CUDA Unified Memory (`cudaMallocManaged`) creates a single address space accessible from both CPU and GPU code. The CUDA driver handles page migration transparently.

```cpp
// Traditional: explicit allocations
float *d_data, *h_data;
cudaMalloc(&d_data, size);         // GPU memory
h_data = malloc(size);              // CPU memory
cudaMemcpy(d_data, h_data, size, cudaMemcpyHostToDevice);  // Explicit copy

// Unified Memory: single allocation
float *data;
cudaMallocManaged(&data, size);     // Accessible from both CPU and GPU
// No explicit copies needed -- pages migrate on demand
```

### Page Migration Mechanism

1. Memory is allocated in **pages** (typically 4KB or 2MB with huge pages)
2. Pages start on CPU (first-touch policy) or GPU
3. When GPU accesses a page residing in CPU memory, a **page fault** occurs
4. The CUDA driver migrates the page from CPU to GPU memory over PCIe
5. If GPU VRAM is full, least-recently-used pages are evicted back to CPU
6. **Prefetch hints** (`cudaMemPrefetchAsync`) can pre-migrate pages before access

### Performance vs Dedicated VRAM

**Dedicated VRAM allocation (cudaMalloc):**
- Full GPU memory bandwidth (e.g., RTX 3090: 936 GB/s)
- No page fault overhead
- Predictable performance

**Unified Memory when model fits in VRAM:**
- Near-identical to dedicated (pages stay on GPU after initial migration)
- Small overhead from page table management (~1-5%)
- Prefetch hints can eliminate most migration latency

**Unified Memory when model exceeds VRAM (spilling to system RAM):**
- Dramatic performance degradation
- Page faults trigger PCIe transfers (PCIe 4.0 x16 = ~25 GB/s vs 936 GB/s VRAM)
- **~30-40x slower** for GPU operations that touch spilled pages
- Thrashing: pages constantly migrating back and forth
- Some mitigation with `cudaMemAdvise` hints

### CUDA Unified Memory in LLM Inference

**llama.cpp does NOT use cudaMallocManaged.** It uses explicit memory management:
- Model weights are allocated in GPU memory via `cudaMalloc` (for GPU layers)
- CPU layers use standard host memory
- KV cache is placed based on where the layer resides

**Projects that do use Unified Memory for LLM:**
- Some experimental forks and research projects
- vLLM has explored unified memory for KV cache overflow
- HuggingFace Accelerate has a `device_map="auto"` that does something similar at a higher level

**Why llama.cpp avoids it:** Explicit layer-by-layer placement with `-ngl` gives predictable performance. Unified memory's page-fault-based migration creates unpredictable latency spikes during inference, which is worse for user experience than consistent (if slower) CPU execution.

---

## 4. NVIDIA GPU + System RAM Spilling

### How llama.cpp Handles Oversized Models

When a model exceeds VRAM, llama.cpp uses a **split placement** strategy:

1. User specifies `-ngl K` (number of layers on GPU)
2. First K layers: weights loaded to GPU VRAM, KV cache on GPU
3. Remaining layers: weights in system RAM, computed on CPU, KV cache in RAM
4. Embedding layer: CPU (unless all layers on GPU)
5. Output head: GPU only if all layers on GPU

### The Pipeline

```
Token -> [CPU: Embedding] -> [GPU: Layers 0..K-1] -> [PCIe transfer] -> [CPU: Layers K..N-1] -> [CPU: Output] -> Token
```

Each token generation requires:
- One CPU->GPU activation transfer (after embedding, before first GPU layer)
- One GPU->CPU activation transfer (after last GPU layer, before first CPU layer)
- Activation tensors are relatively small: `batch_size * hidden_dim * sizeof(float16)` = typically 8-32 KB per transfer

### Performance Cliff Analysis

The performance curve for partial offload has a distinctive shape:

```
tok/s
 ^
 |                                          **** (all GPU)
 |                                       ***
 |                                    **
 |                                 **
 |                             ***
 |                          **
 |                      ***
 |                  ***
 |             ****
 |        ****
 |   ****
 |***  (all CPU)
 +-----------------------------------------> ngl
 0     8    16    24    32 (all layers)
```

**Key observations:**
- The jump from ngl=0 to ngl=1 gives minimal improvement (PCIe overhead dominates)
- Steady improvement as more layers offloaded
- Biggest single jump: going from ngl=(N-1) to ngl=N (full GPU, no PCIe transfers during inference)
- For token generation, the ceiling is still `memory_bandwidth / active_model_size` for the slower device

### Partial Offload Strategy Guide

| VRAM Available | 7B Q4_K_M (4.2 GB) | 13B Q4_K_M (7.9 GB) | 70B Q4_K_M (40 GB) |
|---------------|---------------------|----------------------|---------------------|
| 4 GB | ngl ~26/33 | ngl ~14/40 | ngl ~2/80 |
| 6 GB | ngl 33 (full GPU) | ngl ~22/40 | ngl ~5/80 |
| 8 GB | ngl 33 (full GPU) | ngl ~30/40 | ngl ~8/80 |
| 12 GB | ngl 33 (full GPU) | ngl 40 (full GPU) | ngl ~14/80 |
| 24 GB | ngl 33 (full GPU) | ngl 40 (full GPU) | ngl ~35/80 |

**Practical advice:**
1. Always try to fit the entire model in VRAM first (use more aggressive quantization if needed)
2. If partial offload is necessary, maximize ngl to saturate VRAM
3. Use `-ngl 99` to let llama.cpp auto-calculate maximum layers
4. Monitor with `nvidia-smi` to see actual VRAM usage
5. Leave 200-500 MB VRAM headroom for compute buffers and KV cache

### Flash Attention Impact

Flash Attention (`-fa` flag) reduces KV cache memory on GPU, allowing more layers to fit:
- Without FA: KV cache grows O(n^2) with context length
- With FA: KV cache is computed in tiles, reducing peak memory
- Can free 500MB-2GB+ of VRAM depending on context length
- Allows fitting 2-5 more layers on GPU

---

## 5. PowerInfer and PowerInfer-2

### PowerInfer (Original)

**Key Insight:** In LLM inference, neuron activation is highly skewed. For models like LLaMA and Falcon, approximately 10-20% of neurons are "hot" (activated >90% of the time) and 80-90% are "cold" (rarely activated). This follows a power-law distribution.

**Architecture:**
1. **Offline profiling**: Run calibration data through the model to measure neuron activation frequencies
2. **Hot/cold partitioning**: Frequently activated neurons placed on GPU, rarely activated neurons on CPU
3. **Predictors**: Small neural networks predict which neurons will activate for each token, allowing pre-loading
4. **Adaptive scheduling**: At runtime, hot neurons execute on GPU, cold neurons on CPU, with speculation

**Why it works:**
- Hot neurons (GPU): ~10-20% of parameters but needed for ~90% of computations
- Cold neurons (CPU): ~80-90% of parameters but rarely needed
- Net effect: GPU only needs to hold 10-20% of model weights, but handles most computation
- CPU handles cold neurons in parallel while GPU handles hot ones

**Performance (from the paper, LLaMA-2 70B on RTX 4090 + 96GB system RAM):**
- llama.cpp (full CPU): ~2 tok/s
- llama.cpp (partial GPU offload): ~5-7 tok/s
- PowerInfer: ~11-13 tok/s
- Near-native GPU speed (~18 tok/s full GPU) despite model not fitting in VRAM

**Limitations:**
- Requires model-specific profiling (offline step)
- Only works well with models that have skewed activation (LLaMA, Falcon, Mistral)
- Less effective for dense-activation models
- Profiling data must be regenerated for each model
- Custom fork, not merged into mainline llama.cpp

### PowerInfer-2

**Focus:** Bringing LLM inference to smartphones and edge devices with very limited memory.

**Key innovations:**
1. **Neuron cluster-level management**: Instead of individual neurons, groups neurons into clusters based on co-activation patterns. Reduces management overhead.
2. **Polymorphic neuron engine**: CPU and NPU execute different neuron types optimally
   - CPU: Handles sparse, unpredictable activations efficiently
   - NPU: Handles dense, batch-friendly operations
   - GPU (mobile): Assists with specific compute patterns
3. **Segmented neuron caching**: Model stored on flash/NVMe, frequently-used neuron clusters cached in DRAM
4. **I/O-computation pipelining**: Overlaps loading cold neurons from storage with computing hot neurons

**Performance (from the paper):**
- Tested on smartphones (Snapdragon, MediaTek) with 8-12 GB RAM
- TurboSparse-Mixtral-47B on a phone: ~11 tok/s (vs <1 tok/s with standard approaches)
- Achieves this by only loading ~10% of model into RAM at any time

**Relevance to Llamaste:** PowerInfer's techniques are research-stage and model-specific. For a general-purpose appliance, llama.cpp's `-ngl` approach is more practical, but PowerInfer demonstrates what's theoretically possible.

---

## 6. llama.cpp Tensor Offloading Details

### Backend Architecture (ggml)

llama.cpp's compute engine (ggml) has a modular backend system:

```
ggml-backend.h          -- Abstract backend interface
  ggml-backend-cpu.c    -- CPU backend
  ggml-backend-cuda.cu  -- NVIDIA CUDA backend
  ggml-backend-metal.m  -- Apple Metal backend
  ggml-backend-vulkan.c -- Vulkan compute backend
  ggml-backend-sycl.cpp -- Intel SYCL/oneAPI backend
  ggml-backend-rpc.c    -- Remote Procedure Call backend
```

### Tensor-Level Placement

While `-ngl` operates at the **layer** level, internally ggml operates at the **tensor** level:

1. **Layer assignment**: `-ngl` determines which layers go to which backend
2. **Tensor creation**: Each layer's tensors (Q/K/V/O projections, FFN weights, norms) are allocated on the assigned backend's memory
3. **Compute graph**: When building the compute graph, operations are scheduled on the backend that owns the tensors
4. **Cross-backend transfers**: When an operation needs tensors from different backends, ggml inserts copy operations

### The Backend Scheduler

The `ggml_backend_sched` scheduler manages multi-backend execution:

```
1. Build compute graph (all tensor operations for one forward pass)
2. Assign each tensor to a backend (based on where its weights live)
3. Identify cross-backend edges (where data must transfer between backends)
4. Insert copy nodes at backend boundaries
5. Split graph into per-backend subgraphs
6. Execute subgraphs in dependency order
```

### Tensor Types and Typical Placement

| Tensor Type | Size (7B Q4_K_M) | Placement |
|-------------|-------------------|-----------|
| token_embd.weight | ~64 MB | CPU (unless ngl = all) |
| blk.N.attn_q.weight | ~16 MB per layer | GPU if layer N < ngl |
| blk.N.attn_k.weight | ~4 MB per layer (GQA) | GPU if layer N < ngl |
| blk.N.attn_v.weight | ~4 MB per layer (GQA) | GPU if layer N < ngl |
| blk.N.attn_output.weight | ~16 MB per layer | GPU if layer N < ngl |
| blk.N.ffn_gate.weight | ~22 MB per layer | GPU if layer N < ngl |
| blk.N.ffn_up.weight | ~22 MB per layer | GPU if layer N < ngl |
| blk.N.ffn_down.weight | ~22 MB per layer | GPU if layer N < ngl |
| blk.N.attn_norm.weight | ~16 KB per layer | Same as layer |
| blk.N.ffn_norm.weight | ~16 KB per layer | Same as layer |
| output.weight | ~64 MB | GPU if all layers on GPU |

### Multi-GPU Support

llama.cpp supports splitting across multiple GPUs:

```bash
# Split across 2 GPUs
llama-server -m model.gguf -ngl 99 --split-mode layer --tensor-split 0.5,0.5

# Or by VRAM proportion
llama-server -m model.gguf -ngl 99 --tensor-split 12,24  # 12GB GPU0, 24GB GPU1
```

Split modes:
- `layer`: Each layer goes entirely to one GPU (pipeline parallelism)
- `row`: Individual tensor rows split across GPUs (tensor parallelism, CUDA only)

### Row Splitting (Tensor Parallelism)

Row splitting (`--split-mode row`) in CUDA backend can split individual weight matrices across GPUs:
- Each GPU holds a portion of every layer
- All GPUs compute in parallel for each layer
- Results gathered via NCCL or PCIe
- Better utilization than layer splitting but requires fast inter-GPU connection (NVLink ideal)

---

## 7. Apple Metal Backend

### How Metal Leverages Unified Memory

The Metal backend in llama.cpp takes full advantage of Apple Silicon's UMA:

```
System RAM (e.g., 64 GB)
    |
    +-- Accessible by CPU cores (P-cores, E-cores)
    |
    +-- Accessible by GPU cores (Metal shaders)
    |
    +-- Accessible by Neural Engine
    |
    +-- No copy needed between CPU and GPU
```

**Implementation details:**
1. Model weights loaded into shared memory (mmap)
2. Metal compute shaders read directly from these buffers
3. No `cudaMemcpy` equivalent needed -- pointers work on both CPU and GPU
4. KV cache allocated in shared memory, accessible by both CPU and GPU paths
5. Result: A 96GB M2 Max can run a model "on GPU" that uses 90+ GB of memory

### Metal Compute Shaders for LLM Ops

The Metal backend implements key operations as compute shaders:
- Matrix multiplication (various quantized formats: Q4_0, Q4_1, Q4_K, Q5_K, Q6_K, Q8_0, F16, F32)
- Softmax
- RoPE (rotary position embeddings)
- RMS normalization
- Element-wise operations
- Flash Attention (Metal-optimized implementation)

### Performance Characteristics

**Advantages:**
- No PCIe bottleneck
- All memory available as "VRAM"
- Efficient for both prompt processing (parallel) and generation (bandwidth)
- M-series GPU has high bandwidth per watt

**Disadvantages:**
- Lower raw FLOPS than discrete NVIDIA GPUs (M2 Max ~13.6 TFLOPS FP32 vs RTX 4090 ~82 TFLOPS)
- Shared bandwidth means CPU and GPU compete for bandwidth
- Metal shader compiler less mature than CUDA for ML workloads
- Some quantization formats less optimized than CUDA equivalents

### Practical Apple Silicon Performance

| Chip | RAM Options | Bandwidth | 7B Q4 Gen (tok/s) | 70B Q4 Gen (tok/s) |
|------|-------------|-----------|-------------------|-------------------|
| M1 | 8-16 GB | 68 GB/s | ~15-20 | N/A (doesn't fit) |
| M1 Pro | 16-32 GB | 200 GB/s | ~30-40 | N/A |
| M1 Max | 32-64 GB | 400 GB/s | ~50-60 | ~8-12 |
| M2 Ultra | 64-192 GB | 800 GB/s | ~80-100 | ~15-22 |
| M3 Max | 36-128 GB | 400 GB/s | ~55-65 | ~9-14 |
| M4 Pro | 24-48 GB | 273 GB/s | ~40-50 | N/A (24GB) or ~7-10 (48GB) |

**Note:** These are approximate generation speeds. Prompt processing is significantly faster (5-20x) due to GPU parallelism.

---

## 8. Vulkan Backend

### How Vulkan Compute Works in llama.cpp

Vulkan is a low-level graphics and compute API that works across GPU vendors. llama.cpp uses Vulkan's compute shader pipeline (no graphics rendering).

**Architecture:**
1. Vulkan device enumeration finds available GPUs
2. Compute shaders (SPIR-V bytecode) compiled at build time
3. Model weights uploaded to GPU-accessible memory (Vulkan buffers)
4. Compute commands submitted via command buffers to compute queues
5. Synchronization via Vulkan fences and semaphores

### Cross-Vendor Compatibility

| GPU Vendor | Vulkan Support | Performance in llama.cpp |
|-----------|---------------|------------------------|
| NVIDIA (Maxwell+) | Excellent | 70-85% of CUDA performance |
| AMD (GCN 1.0+) | Excellent | Best non-ROCm option for AMD |
| Intel (Gen 9+) | Good | Modest performance, improving |
| Apple (via MoltenVK) | Partial | Metal backend preferred |
| Qualcomm (Adreno) | Good | Mobile/ARM use cases |

### Build and Usage

```bash
# Build with Vulkan
cmake -B build -DGGML_VULKAN=ON
cmake --build build

# Run (auto-detects Vulkan GPU)
./llama-server -m model.gguf -ngl 99

# Select specific GPU (if multiple)
GGML_VK_DEVICE=0 ./llama-server -m model.gguf -ngl 99
```

### Vulkan vs CUDA Performance

Vulkan typically achieves **70-85% of CUDA performance** on NVIDIA GPUs due to:
- CUDA has more mature optimization for matrix operations (cuBLAS, tensor cores)
- Vulkan compute shaders are more generic
- CUDA kernel launch overhead is lower
- CUDA has tensor core support; Vulkan compute shaders don't directly use tensor cores

However, Vulkan is the **universal** backend:
- Works on AMD GPUs where ROCm may not be available (older GPUs, Windows)
- Works on Intel GPUs
- Works on Linux, Windows, and Android
- No vendor-specific driver/toolkit dependency

### Vulkan Memory Management

Vulkan exposes different memory types:
- `DEVICE_LOCAL`: GPU VRAM (fastest for GPU compute)
- `HOST_VISIBLE | HOST_COHERENT`: System RAM accessible by both CPU and GPU
- `DEVICE_LOCAL | HOST_VISIBLE`: Resizable BAR / Smart Access Memory (system RAM mapped into GPU address space)

llama.cpp's Vulkan backend:
- Prefers `DEVICE_LOCAL` for model weights (loads onto GPU VRAM)
- Falls back to `HOST_VISIBLE` if VRAM is insufficient
- With Resizable BAR enabled, `HOST_VISIBLE | DEVICE_LOCAL` can be large (full system RAM)

### Resizable BAR / Smart Access Memory

Modern systems support mapping system RAM into the GPU's address space:
- GPU can access system RAM directly (bypasses traditional 256MB BAR limit)
- Performance: PCIe bandwidth limited (~25 GB/s for PCIe 4.0 x16) vs native VRAM
- Useful when model slightly exceeds VRAM -- overflow into system RAM via BAR
- AMD calls it "Smart Access Memory" (SAM)
- NVIDIA calls it "Resizable BAR"

---

## 9. ROCm/HIP for AMD GPUs

### Current State of AMD GPU Support

ROCm (Radeon Open Compute) is AMD's CUDA-equivalent compute platform. HIP (Heterogeneous-compute Interface for Portability) is the programming API.

**llama.cpp ROCm/HIP support:**
```bash
# Build with ROCm
cmake -B build -DGGML_HIP=ON
cmake --build build
```

### Supported Hardware

| GPU | Architecture | ROCm Support | llama.cpp Performance |
|-----|-------------|-------------|----------------------|
| RX 7900 XTX | RDNA 3 | Official | Good (70-80% of RTX 4090 level) |
| RX 7900 XT | RDNA 3 | Official | Good |
| RX 7800 XT | RDNA 3 | Community | Good |
| RX 7600 | RDNA 3 | Community | Moderate |
| RX 6900 XT | RDNA 2 | Official | Good |
| RX 6800/6700 | RDNA 2 | Official | Moderate |
| MI300X | CDNA 3 | Official | Excellent (datacenter) |
| MI250X | CDNA 2 | Official | Excellent (datacenter) |
| MI100 | CDNA 1 | Official | Good (datacenter) |

### Performance vs NVIDIA

**Rough comparison (token generation, 7B Q4_K_M, full GPU offload):**

| GPU | VRAM | Approx. tok/s | Notes |
|-----|------|--------------|-------|
| RTX 4090 | 24 GB | 100-130 | CUDA, tensor cores |
| RTX 3090 | 24 GB | 70-90 | CUDA, tensor cores |
| RTX 3060 | 12 GB | 40-55 | CUDA |
| RX 7900 XTX | 24 GB | 70-90 | ROCm/HIP |
| RX 7900 XT | 20 GB | 55-75 | ROCm/HIP |
| RX 7600 | 8 GB | 30-45 | ROCm/HIP or Vulkan |
| RX 6800 XT | 16 GB | 45-60 | ROCm/HIP |

### ROCm Pain Points

1. **Driver compatibility**: ROCm versions are tightly coupled with kernel versions
2. **Consumer GPU support**: Only officially supports some consumer GPUs; many require `HSA_OVERRIDE_GFX_VERSION`
3. **Windows support**: ROCm for Windows (HIP SDK) is less mature; Vulkan often preferred on Windows
4. **Setup complexity**: More difficult to install than NVIDIA drivers + CUDA
5. **No tensor cores equivalent**: RDNA GPUs lack dedicated matrix units (WMMA instructions on CDNA only)

### Workaround for Unsupported AMD GPUs

```bash
# Force ROCm to treat GPU as a different (supported) architecture
# Example: RX 7800 XT (gfx1101) pretending to be RX 7900 XTX (gfx1100)
HSA_OVERRIDE_GFX_VERSION=11.0.0 ./llama-server -m model.gguf -ngl 99
```

### When to Use ROCm vs Vulkan for AMD

| Scenario | Recommended Backend |
|----------|-------------------|
| Linux + officially supported GPU | ROCm/HIP |
| Linux + unsupported consumer GPU | Vulkan (or ROCm with override) |
| Windows + any AMD GPU | Vulkan |
| AMD APU (integrated) | Vulkan |
| Datacenter AMD (MI series) | ROCm/HIP |

---

## 10. SYCL/oneAPI for Intel GPUs

### Intel Arc GPU Support

Intel's discrete GPUs (Arc A-series) are supported in llama.cpp via the SYCL/oneAPI backend.

```bash
# Build with SYCL (Intel oneAPI)
source /opt/intel/oneapi/setvars.sh
cmake -B build -DGGML_SYCL=ON -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx
cmake --build build
```

### Supported Hardware

| GPU | VRAM | Architecture | Status |
|-----|------|-------------|--------|
| Arc A770 | 16 GB | Alchemist (Xe-HPG) | Good support |
| Arc A750 | 8 GB | Alchemist (Xe-HPG) | Good support |
| Arc A580 | 8 GB | Alchemist (Xe-HPG) | Good support |
| Arc A380 | 6 GB | Alchemist (Xe-HPG) | Basic support |
| Data Center Max (Ponte Vecchio) | 48-128 GB | Xe-HPC | Good support |
| Integrated (Meteor Lake) | Shared | Xe-LPG | Limited |

### Performance Characteristics

| GPU | 7B Q4_K_M tok/s | Notes |
|-----|-----------------|-------|
| Arc A770 16GB | 30-50 | Competitive with RTX 3060 |
| Arc A750 8GB | 25-40 | Slightly less than A770 |
| Arc A580 8GB | 20-35 | Budget option |

### Intel GPU + System RAM

Intel Arc GPUs support Resizable BAR, allowing the GPU to access system RAM. However:
- PCIe bandwidth limits effectiveness
- Better to use aggressive quantization to fit in VRAM
- SYCL backend prefers device-local memory

### oneAPI/SYCL Advantages

- Supports Intel GPUs, CPUs, and FPGAs with same code
- Can target NVIDIA GPUs via SYCL-to-CUDA translation (experimental)
- MKL integration for optimized math operations
- Growing ecosystem and active development

### oneAPI/SYCL Disadvantages

- Large toolkit install (~20+ GB for full oneAPI)
- Compiler (icx/icpx) required for optimal performance
- Less community support than CUDA or Vulkan
- Performance optimization still catching up to CUDA maturity

---

## 11. GPU Memory Management Strategies

### Strategy: Maximize Effective Memory

The goal is to use GPU for what it's best at (compute-heavy parallel operations) and CPU for what it handles adequately (sequential, memory-bound operations).

### Layer Classification by Compute Intensity

| Operation | Compute Intensity | Best Placement |
|-----------|-------------------|---------------|
| Token embedding lookup | Low (table lookup) | CPU |
| Attention QKV projection | High (large matmul) | GPU |
| Attention score computation | High (matmul + softmax) | GPU |
| Attention output projection | High (large matmul) | GPU |
| MLP/FFN gate + up projection | High (large matmul) | GPU |
| MLP/FFN activation (SiLU) | Low (element-wise) | Same as surrounding ops |
| MLP/FFN down projection | High (large matmul) | GPU |
| RMS normalization | Low (reduction + scale) | Same as surrounding ops |
| Output logit computation | Medium (matmul, done once per token) | CPU acceptable |

### Practical Strategies by VRAM Size

**Strategy A: VRAM < 50% of model size** (e.g., 4GB VRAM, 7B model at 4.2GB)
- Offload as many layers as fit (ngl ~20-25 out of 33)
- Keep embedding and output on CPU
- Benefit: 1.5-2.5x speedup over pure CPU

**Strategy B: VRAM ~ 75% of model size** (e.g., 6GB VRAM, 7B model at 4.2GB)
- All layers fit on GPU with some headroom
- Place everything on GPU (ngl 99)
- Benefit: Full GPU speed

**Strategy C: VRAM > model but KV cache overflows** (e.g., 8GB VRAM, 7B model, 32K context)
- Model weights on GPU, KV cache partly on GPU
- Use KV cache quantization (-ctk q8_0) to reduce KV size
- Use Flash Attention (-fa) to reduce peak memory
- Reduce context length if needed

**Strategy D: Multi-model serving** (e.g., 24GB VRAM, want 7B + 13B simultaneously)
- Assign layers proportionally between models
- Or use model swapping with `--cont-batching`
- Consider quantization tradeoffs: two models at Q4 vs one at Q8

### KV Cache Memory Optimization

```bash
# KV cache quantization (reduces KV from FP16 to Q8_0 or Q4_0)
llama-server -m model.gguf -ngl 99 -ctk q8_0 -ctv q8_0

# Saves ~50% KV cache memory with Q8_0 (minimal quality impact)
# Saves ~75% KV cache memory with Q4_0 (some quality impact on long contexts)
```

### VRAM Budget Calculator

```
VRAM_budget = Total_VRAM - OS_overhead(~200-500MB)

Model_weights_on_GPU = ngl * per_layer_size
KV_cache_on_GPU = ngl * 2 * kv_heads * head_dim * context_len * kv_precision_bytes
Compute_buffers = ~200-500 MB (scales with batch size and context)

Required: Model_weights_on_GPU + KV_cache_on_GPU + Compute_buffers <= VRAM_budget
```

---

## 12. KV Cache on GPU vs CPU

### What Is the KV Cache

During autoregressive generation, each transformer layer caches the Key and Value projections of all previous tokens. This avoids recomputing them for each new token.

**Size per layer (FP16):**
```
KV_per_layer = 2 * num_kv_heads * head_dim * context_length * 2 bytes
```

**Example (LLaMA 7B, 32 layers, 32 KV heads, 128 dim, 4096 context):**
```
Per layer: 2 * 32 * 128 * 4096 * 2 = 64 MB
Total: 32 * 64 MB = 2048 MB = 2 GB
```

**Example (Qwen2.5 7B, 28 layers, 4 KV heads (GQA), 128 dim, 4096 context):**
```
Per layer: 2 * 4 * 128 * 4096 * 2 = 8 MB
Total: 28 * 8 MB = 224 MB
```

GQA (Grouped Query Attention) dramatically reduces KV cache size.

### Performance Trade-offs: GPU vs CPU KV Cache

**KV cache on GPU:**
- Pros: Attention computation reads KV from fast VRAM (up to 936 GB/s on RTX 4090)
- Cons: Consumes VRAM that could hold more model layers
- Best when: Model fits with room to spare, long contexts

**KV cache on CPU:**
- Pros: Frees VRAM for model weights
- Cons: Attention must either (a) run on CPU (slow) or (b) transfer KV to GPU each step (PCIe bottleneck)
- Best when: VRAM-constrained, short contexts, batch processing

### In llama.cpp

KV cache placement follows the layer:
- If layer N is on GPU, its KV cache is in VRAM
- If layer N is on CPU, its KV cache is in system RAM
- No independent KV cache placement control (as of early 2025)

This means the `-ngl` decision also determines KV cache placement. If you put 30 of 33 layers on GPU, 30 layers' KV caches are in VRAM and 3 layers' KV caches are in RAM.

### KV Cache Quantization Impact

| KV Precision | Memory per 1K Context (7B, full model) | Quality Impact |
|-------------|----------------------------------------|----------------|
| FP16 (default) | ~500 MB (LLaMA-style) / ~56 MB (GQA) | Baseline |
| Q8_0 | ~250 MB / ~28 MB | Negligible |
| Q4_0 | ~125 MB / ~14 MB | Minor on long contexts |

**Recommendation:** Always use `-ctk q8_0` on VRAM-constrained systems. The quality impact is minimal and the VRAM savings allow more layers on GPU.

---

## 13. Benchmarks: Partial Offload Scenarios

### Methodology Notes

These are approximate benchmarks compiled from community reports, llama.cpp GitHub discussions, and independent testing as of late 2024 / early 2025. Actual performance varies by exact hardware, drivers, quantization, context length, and llama.cpp version.

### Scenario 1: 7B Q4_K_M with 4GB VRAM (GTX 1650, DDR4 system)

| ngl | VRAM Used | Gen (tok/s) | Prompt (tok/s) | Notes |
|-----|-----------|-------------|----------------|-------|
| 0 | 0 MB | 6-10 | 15-25 | Pure CPU (i5-12400, DDR4-3200) |
| 10 | ~1.5 GB | 10-14 | 25-40 | Partial offload |
| 20 | ~2.8 GB | 14-20 | 40-60 | Most layers on GPU |
| 26 | ~3.6 GB | 16-22 | 50-75 | Near-max for 4GB |
| 33 | OOM | - | - | Doesn't fit |

**Takeaway:** Even a 4GB GPU provides meaningful acceleration. But the jump from partial to full offload (which isn't possible here) would be 3-5x additional.

### Scenario 2: 13B Q4_K_M with 8GB VRAM (RTX 3060 Ti, DDR4)

| ngl | VRAM Used | Gen (tok/s) | Prompt (tok/s) | Notes |
|-----|-----------|-------------|----------------|-------|
| 0 | 0 MB | 4-7 | 8-15 | Pure CPU (Ryzen 5 5600X) |
| 15 | ~3.5 GB | 7-10 | 20-35 | Partial |
| 30 | ~6.5 GB | 12-18 | 50-80 | Most layers on GPU |
| 40 | OOM at ctx>1K | 18-25 | 80-120 | Tight fit, low context |
| 40 | ~7.5 GB | 25-35 | 100-150 | With -ctk q8_0, -fa |

**Takeaway:** KV cache quantization + Flash Attention can enable full GPU offload that otherwise wouldn't fit, yielding 3-5x speedup.

### Scenario 3: 7B Q4_K_M Full GPU (RTX 3060 12GB, plenty of room)

| Configuration | Gen (tok/s) | Prompt (tok/s) |
|--------------|-------------|----------------|
| CPU only (Ryzen 7 5800X) | 10-15 | 30-50 |
| GPU ngl=33, ctx=4096 | 55-75 | 300-500 |
| GPU ngl=33, ctx=8192 | 50-65 | 250-400 |
| GPU ngl=33, ctx=4096, -fa | 60-80 | 400-600 |

**Takeaway:** Full GPU offload is 5-7x faster for generation and 10-15x faster for prompt processing.

### Scenario 4: 70B Q4_K_M with 24GB VRAM (RTX 4090, DDR5)

| ngl | VRAM Used | Gen (tok/s) | Prompt (tok/s) | Notes |
|-----|-----------|-------------|----------------|-------|
| 0 | 0 MB | 2-4 | 5-10 | CPU only (Ryzen 9 7950X) |
| 20 | ~11 GB | 5-8 | 15-25 | Quarter offload |
| 40 | ~21 GB | 8-14 | 30-50 | Half offload |
| 55 | ~23.5 GB | 10-18 | 40-70 | Max fit (with -ctk q8_0) |
| 80 | OOM | - | - | Needs ~40 GB VRAM |

**Takeaway:** Even partial offload of a 70B model provides significant improvement. For full offload, need 2x RTX 4090 or a single 48GB GPU (RTX 6000 Ada, A6000).

### Scenario 5: Apple Silicon Unified Memory

| Chip + RAM | Model | Gen (tok/s) | Prompt (tok/s) |
|-----------|-------|-------------|----------------|
| M1 8GB | 7B Q4_K_M | 12-18 | 60-100 |
| M1 Pro 16GB | 7B Q4_K_M | 25-35 | 150-250 |
| M1 Max 64GB | 70B Q4_K_M | 8-12 | 15-25 |
| M2 Ultra 192GB | 70B Q4_K_M | 15-22 | 30-50 |
| M3 Max 128GB | 70B Q4_K_M | 10-15 | 20-35 |

**Takeaway:** Apple Silicon's unified memory means any amount of RAM is usable as "VRAM" with no penalty beyond bandwidth sharing. This makes it uniquely well-suited for large models.

### Scenario 6: AMD GPU (ROCm/HIP)

| GPU | Model | ngl | Gen (tok/s) | Notes |
|-----|-------|-----|-------------|-------|
| RX 7900 XTX 24GB | 7B Q4_K_M | 33 | 65-85 | Full offload |
| RX 7900 XTX 24GB | 13B Q4_K_M | 40 | 40-55 | Full offload |
| RX 7900 XTX 24GB | 70B Q4_K_M | ~35 | 5-9 | Partial, rest on CPU |
| RX 6800 XT 16GB | 7B Q4_K_M | 33 | 40-55 | Full offload |
| RX 6800 XT 16GB | 13B Q4_K_M | ~30 | 15-25 | Partial offload |

### Summary: When GPU + System RAM Matters

| Scenario | Best Approach | Expected Gain vs CPU-only |
|----------|--------------|--------------------------|
| Model fits in VRAM | Full GPU offload (-ngl 99) | 5-10x generation, 15-30x prompt |
| Model slightly exceeds VRAM | Quantize harder (Q3 instead of Q4), use -fa and -ctk q8_0 | Try to get full offload |
| Model significantly exceeds VRAM | Partial offload, maximize ngl | 1.5-3x generation, 3-8x prompt |
| Apple Silicon | Always -ngl 99 (all memory is VRAM) | N/A (always use Metal) |
| AMD GPU + Linux | ROCm if supported, else Vulkan | Similar to NVIDIA at same VRAM |
| AMD GPU + Windows | Vulkan backend | 70-85% of CUDA equivalent |
| Intel Arc | SYCL or Vulkan | Modest improvement for small models |
| No GPU | CPU with AVX2/AVX-512, maximize bandwidth | Baseline |

---

## Relevance to Llamaste

### Current Architecture (CPU-only)
Llamaste is currently designed as a CPU-only inference appliance. This is correct for the primary use case of turning old PCs into AI servers -- most old PCs have weak or no discrete GPUs.

### Future GPU Support Considerations

If GPU support is added (e.g., "Llamaste Full" variant):

1. **Auto-detection needed:** `S20hwdetect` should detect GPU presence, vendor, VRAM size
2. **Backend selection:** Auto-select CUDA/ROCm/Vulkan based on detected GPU
3. **Partial offload calculation:** Calculate optimal `-ngl` based on model size, VRAM, and context length
4. **Build implications:**
   - GPU builds require glibc (not musl) for CUDA
   - Binary size increases significantly (~50-200 MB for GPU backends)
   - Multiple binary variants needed (CUDA, ROCm, Vulkan)
5. **Vulkan as universal GPU backend:** Vulkan works across all vendors, could be single GPU binary
   - Build: `-DGGML_VULKAN=ON`
   - ~70-85% of vendor-specific backend performance
   - Works with both discrete and integrated GPUs

### Recommended Llamaste GPU Strategy

**Phase 1 (current):** CPU-only, musl, minimal binary
**Future Phase:**
- Detect GPU at boot
- If NVIDIA: use CUDA backend (separate build)
- If AMD/Intel/generic: use Vulkan backend (universal GPU build)
- Auto-calculate ngl from VRAM
- Formula: `ngl = min(total_layers, (free_vram - 500MB) / per_layer_size)`
- Serve with optimal split between GPU and CPU

### Build Matrix

| Variant | Backends | C Library | Binary Size | GPU Support |
|---------|----------|-----------|-------------|-------------|
| llamaste-cpu | CPU only | musl | ~5 MB | None |
| llamaste-vulkan | CPU + Vulkan | glibc | ~15-20 MB | AMD, Intel, NVIDIA |
| llamaste-cuda | CPU + CUDA | glibc | ~50-100 MB | NVIDIA only |
| llamaste-rocm | CPU + HIP | glibc | ~30-50 MB | AMD only |

---

## Key References

- llama.cpp GitHub: https://github.com/ggerganov/llama.cpp
- ggml backend architecture: https://github.com/ggerganov/llama.cpp/tree/master/ggml
- PowerInfer paper: "PowerInfer: Fast Large Language Model Serving with a Consumer-grade GPU" (SJTU, 2023)
- PowerInfer-2 paper: "PowerInfer-2: Fast Large Language Model Inference on a Smartphone" (SJTU, 2024)
- NVIDIA CUDA Unified Memory: https://developer.nvidia.com/blog/unified-memory-cuda-beginners/
- ROCm documentation: https://rocm.docs.amd.com/
- Intel oneAPI/SYCL: https://www.intel.com/content/www/us/en/developer/tools/oneapi/overview.html
- Vulkan compute: https://www.khronos.org/vulkan/
- Apple Metal Performance Shaders: https://developer.apple.com/metal/
