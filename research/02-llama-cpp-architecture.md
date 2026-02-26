# llama.cpp Deep Architecture Reference

## 1. Source Code Structure

The llama.cpp repository is organized into several top-level directories, each with a distinct responsibility. As of late 2024/early 2025, the structure reflects a major refactoring that extracted ggml into its own subtree and modularized backends.

### Top-Level Directory Layout

```
llama.cpp/
├── ggml/                    # The tensor computation library (extracted subtree)
│   ├── include/             # Public headers: ggml.h, ggml-alloc.h, ggml-backend.h
│   └── src/                 # Core ggml implementation + all backends
│       ├── ggml.c           # Core tensor ops, graph, memory management
│       ├── ggml-alloc.c     # Tensor memory allocator
│       ├── ggml-backend.c   # Backend abstraction layer
│       ├── ggml-quants.c    # Quantization kernels (Q4_0, Q4_K_M, etc.)
│       ├── ggml-cpu/        # CPU backend (with SIMD dispatching)
│       ├── ggml-cuda/       # NVIDIA CUDA backend
│       ├── ggml-vulkan/     # Vulkan compute backend
│       ├── ggml-metal/      # Apple Metal backend
│       ├── ggml-rpc/        # RPC distributed backend
│       ├── ggml-blas/       # BLAS backend (OpenBLAS, MKL, etc.)
│       ├── ggml-sycl/       # Intel SYCL/oneAPI backend
│       └── ggml-cann/       # Huawei Ascend CANN backend
├── src/                     # Core llama.cpp library
│   ├── llama.cpp            # THE main file: model loading, graph building, inference
│   ├── llama-vocab.cpp      # Tokenizer/vocabulary implementation
│   ├── llama-sampling.cpp   # Sampling chain implementation
│   ├── llama-grammar.cpp    # Grammar-constrained sampling (GBNF)
│   ├── llama-batch.cpp      # Batch management
│   ├── llama-kv-cache.cpp   # KV cache management
│   ├── llama-mmap.cpp       # Memory-mapped file I/O
│   ├── llama-model.cpp      # Model struct and architecture dispatch
│   ├── llama-context.cpp    # Context (session state) management
│   └── unicode*.cpp         # Unicode handling for tokenizers
├── include/                 # Public API headers
│   └── llama.h              # The public C API (stable interface)
├── common/                  # Shared utilities for examples/tools
│   ├── common.h/cpp         # Argument parsing, model loading helpers
│   ├── sampling.h/cpp       # High-level sampling wrapper
│   ├── json.hpp             # nlohmann JSON (header-only)
│   ├── chat-template.hpp    # Jinja2-lite chat template engine
│   └── log.h/cpp            # Logging infrastructure
├── examples/                # Example programs and tools
│   ├── main/                # llama-cli (interactive/batch text generation)
│   ├── server/              # llama-server (HTTP API + web UI)
│   ├── perplexity/          # Perplexity evaluation tool
│   ├── quantize/            # llama-quantize (model quantization)
│   ├── bench/               # llama-bench (performance benchmarking)
│   ├── embedding/           # Embedding extraction
│   ├── speculative/         # Speculative decoding
│   ├── infill/              # Code infill (fill-in-the-middle)
│   ├── batched/             # Batched generation example
│   ├── lookup/              # Lookup-based speculative decoding
│   ├── parallel/            # Parallel generation (multiple sequences)
│   ├── retrieval/           # RAG-style retrieval
│   └── rpc/                 # llama-rpc-server (distributed worker)
├── models/                  # Model conversion scripts
│   └── ggml-common.h        # Shared quantization tables
├── scripts/                 # Helper scripts (CI, benchmarking)
├── tests/                   # Unit and integration tests
├── CMakeLists.txt           # Primary build system
├── Makefile                 # Legacy Makefile (deprecated, removed in latest)
└── convert_hf_to_gguf.py   # HuggingFace -> GGUF converter
```

### What Each Directory Does

**ggml/** -- The computation engine. This is a standalone C library that knows nothing about LLMs. It provides: tensor types, quantized data formats, mathematical operations (matmul, softmax, rope, etc.), computation graph construction, memory management, and a pluggable backend system. It is designed to be reusable in other projects.

**src/** -- The LLM-specific logic. This is where llama.cpp implements: GGUF file parsing, model architecture dispatch (LLaMA, Mistral, Qwen, Phi, Gemma, etc.), transformer graph construction (attention, FFN, normalization), KV cache, tokenization, sampling, and the public C API (`llama.h`).

**include/** -- The public `llama.h` header defines the stable C API. All downstream consumers (server, CLI, bindings) use only this API. It exposes: model loading, context creation, batched decoding, token sampling, embedding extraction, and KV cache manipulation.

**common/** -- Shared utility code used by example programs. Not part of the library itself. Provides command-line argument parsing, high-level model loading wrappers, sampling parameter management, chat template rendering, and JSON handling.

**examples/** -- Standalone programs built on top of the llama.h API. The two most important are `server/` (the HTTP inference server with OpenAI-compatible API) and `main/` (the CLI tool). Each example is a self-contained program demonstrating different capabilities.

---

## 2. The ggml Tensor Library

ggml (Georgi Gerganov's Machine Learning library) is the computational foundation. It is a pure C library implementing a tensor computation framework optimized for transformer inference on consumer hardware.

### Core Design Principles

1. **No dynamic memory allocation during computation** -- All memory is pre-allocated
2. **Computation graph based** -- Operations build a DAG, which is then executed
3. **Quantization-first** -- Native support for low-bit data types (2-8 bit)
4. **No external dependencies** -- Self-contained C99 code (with optional SIMD intrinsics)

### Tensor Types (`ggml_tensor`)

The fundamental data structure is `ggml_tensor`:

```c
struct ggml_tensor {
    enum ggml_type type;      // Data type (F32, F16, Q4_0, Q4_K_M, etc.)
    enum ggml_backend_type backend;  // Where this tensor lives
    struct ggml_backend_buffer * buffer;  // The buffer holding data

    int64_t ne[GGML_MAX_DIMS];   // Number of elements per dimension (up to 4D)
    size_t  nb[GGML_MAX_DIMS];   // Stride in bytes per dimension

    enum ggml_op op;              // Operation that produced this tensor
    struct ggml_tensor * src[GGML_MAX_SRC];  // Source tensors (inputs to op)

    char name[GGML_MAX_NAME];     // Human-readable name (e.g. "blk.0.attn_q.weight")
    void * data;                  // Pointer to raw data

    // ... padding, extra metadata
};
```

Key tensor properties:
- **Up to 4 dimensions**: `ne[0]` is the innermost (contiguous) dimension, `ne[3]` is the outermost
- **Strides** (`nb[]`): Byte stride per dimension, enabling views and transposes without data copies
- **Operation tracking**: Each tensor records the `op` and `src[]` that produced it, forming the computation graph implicitly
- **Named tensors**: Every tensor has a name string, used for mapping to GGUF file keys

### Supported Data Types

ggml's primary innovation is native support for quantized types:

| Type | Bits/Weight | Block Size | Description |
|------|------------|------------|-------------|
| GGML_TYPE_F32 | 32 | 1 | Standard float |
| GGML_TYPE_F16 | 16 | 1 | Half precision |
| GGML_TYPE_BF16 | 16 | 1 | Brain float 16 |
| GGML_TYPE_Q8_0 | 8.5 | 32 | 8-bit quantized (32 values + 1 scale) |
| GGML_TYPE_Q4_0 | 4.5 | 32 | 4-bit quantized (32 values + 1 scale) |
| GGML_TYPE_Q4_1 | 5.0 | 32 | 4-bit + min (32 values + scale + min) |
| GGML_TYPE_Q5_0 | 5.5 | 32 | 5-bit quantized |
| GGML_TYPE_Q5_1 | 6.0 | 32 | 5-bit + min |
| GGML_TYPE_Q2_K | 2.5-3.4 | 256 | K-quant 2-bit (super-blocks) |
| GGML_TYPE_Q3_K | 3.4-3.9 | 256 | K-quant 3-bit |
| GGML_TYPE_Q4_K | 4.5 | 256 | K-quant 4-bit |
| GGML_TYPE_Q5_K | 5.5 | 256 | K-quant 5-bit |
| GGML_TYPE_Q6_K | 6.6 | 256 | K-quant 6-bit |
| GGML_TYPE_IQ* | 2-4 | varies | Importance-matrix quantized |

**K-quants** (Q2_K through Q6_K) use a "super-block" structure of 256 values with nested quantization scales, achieving better quality than simple per-32-block quantization at the same bit rate. They were introduced by Ikawrakow.

**IQ-quants** (IQ1_S, IQ2_XXS, IQ2_XS, IQ3_M, etc.) use importance-matrix calibration data to allocate bits where they matter most, achieving better quality at extremely low bit rates.

### Quantization Block Structure

All quantized types use a block-based approach. For example, Q4_0:

```c
typedef struct {
    ggml_fp16_t d;        // Scale factor (delta) for the block
    uint8_t qs[16];       // 32 x 4-bit quantized values packed into 16 bytes
} block_q4_0;             // Total: 18 bytes for 32 values = 4.5 bits/value
```

K-quants use nested super-blocks:

```c
typedef struct {
    uint8_t scales[12];    // Scales and mins for 8 sub-blocks (packed)
    ggml_fp16_t d;         // Super-block scale
    ggml_fp16_t dmin;      // Super-block min
    uint8_t qs[128];       // 256 x 4-bit quantized values
} block_q4_K;              // Total: 144 bytes for 256 values = 4.5 bits/value
```

### Tensor Operations

ggml supports ~100+ operations. The most important for transformer inference:

**Matrix Operations:**
- `ggml_mul_mat(a, b)` -- Matrix multiplication (the dominant operation). Supports mixed-precision: e.g., Q4_K weight * F32 activation -> F32 result
- `ggml_mul_mat_id(as, ids, b)` -- Indexed matmul for Mixture-of-Experts routing

**Element-wise Operations:**
- `ggml_add`, `ggml_mul`, `ggml_scale` -- Basic arithmetic
- `ggml_silu`, `ggml_gelu`, `ggml_relu` -- Activation functions
- `ggml_rms_norm`, `ggml_norm` -- Normalization

**Attention Operations:**
- `ggml_flash_attn_ext` -- Fused flash attention (Q, K, V -> output in one op)
- `ggml_rope` / `ggml_rope_ext` -- Rotary Position Embeddings
- `ggml_soft_max` / `ggml_soft_max_ext` -- Softmax (with optional scaling and masking)

**Tensor Manipulation:**
- `ggml_view_*d`, `ggml_reshape_*d` -- Zero-copy views and reshapes
- `ggml_permute`, `ggml_transpose` -- Dimension reordering
- `ggml_cont` -- Force contiguous memory layout (triggers a copy)
- `ggml_concat` -- Concatenation
- `ggml_get_rows` -- Embedding lookup (gather operation)

**Quantization Operations:**
- `ggml_quantize_*` -- Quantize F32 -> quantized type
- Dequantize is implicit inside `ggml_mul_mat` -- the kernel reads quantized blocks and dequantizes on-the-fly during the dot product

### Memory Management: ggml_context

ggml uses a simple bump allocator for tensor metadata:

```c
struct ggml_context * ctx = ggml_init({
    .mem_size   = 256 * 1024 * 1024,  // Size of the metadata arena
    .mem_buffer = NULL,                 // NULL = ggml allocates
    .no_alloc   = true,                 // Don't allocate tensor data
});
```

When `no_alloc = true` (the common case for inference), the context only allocates tensor descriptor structs (~200 bytes each), not their data. The actual tensor data is managed separately by the backend buffer system.

This is critical: **ggml_context is for tensor metadata only, not for tensor data**. Tensor data lives in `ggml_backend_buffer` objects allocated by backends.

---

## 3. Backend System

The backend system is how ggml abstracts over different hardware. It allows the same computation graph to run on CPU, CUDA, Vulkan, Metal, or distributed across machines via RPC -- all behind a unified interface.

### Architecture Overview

```
                   llama.cpp (builds computation graph)
                              |
                              v
                     ggml_backend_sched
                     (multi-backend scheduler)
                    /          |          \
                   v           v           v
           ggml_backend    ggml_backend    ggml_backend
              (CPU)          (CUDA)         (RPC)
                |              |              |
                v              v              v
         ggml_backend     ggml_backend    ggml_backend
           _buffer          _buffer         _buffer
           (RAM)           (VRAM)        (remote RAM)
```

### The ggml_backend Interface

Every backend implements a vtable of function pointers:

```c
struct ggml_backend_i {
    const char * (*get_name)(ggml_backend_t backend);

    // Buffer management
    ggml_backend_buffer_type_t (*get_default_buffer_type)(ggml_backend_t backend);

    // Graph execution
    enum ggml_status (*graph_compute)(ggml_backend_t backend, struct ggml_cgraph * cgraph);

    // Synchronization
    void (*synchronize)(ggml_backend_t backend);

    // Event support (for async ops)
    ggml_backend_event_t (*event_new)(ggml_backend_t backend);
    void (*event_record)(ggml_backend_event_t event);
    void (*event_wait)(ggml_backend_t backend, ggml_backend_event_t event);
};
```

### Backend Buffer Types

Each backend provides one or more buffer types:

```c
struct ggml_backend_buffer_type_i {
    const char *          (*get_name)        (ggml_backend_buffer_type_t buft);
    ggml_backend_buffer_t (*alloc_buffer)    (ggml_backend_buffer_type_t buft, size_t size);
    size_t                (*get_alignment)   (ggml_backend_buffer_type_t buft);
    size_t                (*get_alloc_size)  (ggml_backend_buffer_type_t buft, struct ggml_tensor * tensor);
    bool                  (*is_host)         (ggml_backend_buffer_type_t buft);
};
```

**Buffer types define WHERE memory is allocated** (system RAM, VRAM, pinned memory, etc.). Each buffer type can allocate multiple buffers.

### Backend Buffer

```c
struct ggml_backend_buffer_i {
    void   (*free_buffer)(ggml_backend_buffer_t buffer);
    void * (*get_base)   (ggml_backend_buffer_t buffer);
    void   (*init_tensor)(ggml_backend_buffer_t buffer, struct ggml_tensor * tensor);
    void   (*memset_tensor)(ggml_backend_buffer_t buffer, struct ggml_tensor * tensor, uint8_t value, size_t offset, size_t size);
    void   (*set_tensor) (ggml_backend_buffer_t buffer, struct ggml_tensor * tensor, const void * data, size_t offset, size_t size);
    void   (*get_tensor) (ggml_backend_buffer_t buffer, struct ggml_tensor * tensor, void * data, size_t offset, size_t size);
    void   (*clear)      (ggml_backend_buffer_t buffer, uint8_t value);
};
```

### Specific Backends

**CPU Backend** (`ggml-cpu/`):
- Uses SIMD-optimized kernels: SSE, AVX, AVX2, AVX-512, ARM NEON, SVE
- Runtime dispatching via `GGML_CPU_ALL_VARIANTS`: builds multiple shared libraries (ggml-cpu-avx.so, ggml-cpu-avx2.so, etc.), selects at startup based on CPUID
- Implements all operations (the reference implementation)
- Supports threading via OpenMP or pthreads
- Memory is standard system RAM (malloc/mmap)

**CUDA Backend** (`ggml-cuda/`):
- Dedicated CUDA kernels for each quantized type and operation
- Tensor data lives in GPU VRAM via cudaMalloc
- Supports multiple GPUs (tensor split across devices)
- Key operations: quantized matmul, flash attention, RoPE, all in CUDA
- Uses CUDA streams for async execution
- Pinned host memory for fast CPU<->GPU transfers

**Vulkan Backend** (`ggml-vulkan/`):
- Compute shaders for tensor operations
- Works across vendors (NVIDIA, AMD, Intel, Qualcomm)
- Tensor data in Vulkan device memory
- Uses compute queues and pipeline barriers for synchronization

**Metal Backend** (`ggml-metal/`):
- Apple GPU compute shaders (.metal files)
- Unified memory on Apple Silicon (CPU and GPU share the same physical RAM)
- Optimized for Apple Neural Engine patterns
- Uses Metal Performance Shaders where applicable

**RPC Backend** (`ggml-rpc/`):
- Implements pipeline parallelism across machines
- Custom binary protocol over raw TCP (port 50052 default)
- Operations: ALLOC_BUFFER, FREE_BUFFER, BUFFER_GET_BASE, SET_TENSOR, GET_TENSOR, COPY_TENSOR, GRAPH_COMPUTE
- Worker node runs `llama-rpc-server` which accepts these commands
- Coordinator sends weight tensors at model load time, then dispatches sub-graphs for computation

### The Backend Scheduler (`ggml_backend_sched`)

The scheduler is the orchestrator that allows **multiple backends to cooperate on a single computation graph**. This is the key mechanism for GPU offloading (some layers on GPU, some on CPU).

```c
struct ggml_backend_sched * sched = ggml_backend_sched_new(
    backends,       // Array of backends [cuda, cpu]
    bufts,          // Corresponding buffer types
    n_backends,     // Number of backends (e.g., 2)
    max_graph_size, // Max nodes in graph
    parallel        // Enable parallel backend execution
);
```

**How the scheduler works:**

1. **Tensor assignment**: Each tensor in the graph is assigned to a backend. Weight tensors are assigned to whichever backend's buffer they reside in. Intermediate tensors are assigned based on where their inputs are and which backend can compute them most efficiently.

2. **Graph splitting**: The scheduler splits the computation graph into sub-graphs, one per backend. It inserts data copy operations at the boundaries (e.g., CPU->CUDA transfer nodes).

3. **Execution**: Sub-graphs are dispatched to their respective backends in topological order. The scheduler handles synchronization between backends using events.

4. **Layer offloading** (`-ngl N`): When you specify N GPU layers, the model loader assigns the first N transformer layer weight tensors to the CUDA buffer. The scheduler then naturally routes those layers' computations to the CUDA backend, with copy nodes for cross-backend data transfers.

### Backend Selection at Build Time

CMake flags control which backends are compiled:

```cmake
GGML_CPU=ON           # Always included
GGML_CUDA=OFF         # Requires CUDA toolkit
GGML_VULKAN=OFF       # Requires Vulkan SDK
GGML_METAL=OFF        # macOS only, requires Metal framework
GGML_RPC=OFF          # TCP networking
GGML_BLAS=OFF         # Requires BLAS library
GGML_SYCL=OFF         # Requires Intel oneAPI
```

At runtime, available backends are registered and the user controls assignment through `-ngl` (GPU layers), `--rpc` (remote backends), etc.

---

## 4. Model Loading Pipeline

### GGUF File Format

GGUF (GGML Universal File format) is a self-describing binary format. It replaced the older GGML and GGJT formats.

```
GGUF File Layout:
+---------------------------+
| Magic: "GGUF" (4 bytes)  |
| Version: uint32           |
| n_tensors: uint64         |
| n_kv: uint64              |
+---------------------------+
| KV Metadata               |  <- Architecture, vocab, hyperparams, chat template
|   key: string             |
|   type: enum              |
|   value: varies           |
|   ... (n_kv entries)      |
+---------------------------+
| Tensor Infos              |  <- Name, shape, type, offset for each tensor
|   name: string            |
|   n_dims: uint32          |
|   dims: uint64[]          |
|   type: enum              |
|   offset: uint64          |
|   ... (n_tensors entries) |
+---------------------------+
| Alignment Padding         |
+---------------------------+
| Tensor Data               |  <- Bulk binary data, contiguous, aligned
|   (tensor 0 data)         |
|   (alignment padding)     |
|   (tensor 1 data)         |
|   ...                     |
+---------------------------+
```

### Key Metadata Fields

```
general.architecture = "llama"          # Which model class to use
general.name = "Qwen2.5 7B Instruct"   # Human-readable name

llama.context_length = 32768
llama.embedding_length = 4096
llama.block_count = 32                  # Number of transformer layers
llama.attention.head_count = 32
llama.attention.head_count_kv = 8       # GQA heads
llama.rope.freq_base = 10000.0
llama.feed_forward_length = 11008

tokenizer.ggml.model = "gpt2"          # BPE tokenizer type
tokenizer.ggml.tokens = [...]           # Vocabulary
tokenizer.ggml.merges = [...]           # BPE merge rules
tokenizer.ggml.bos_token_id = 1
tokenizer.ggml.eos_token_id = 2
tokenizer.chat_template = "{% for ... %}"  # Jinja2 template

general.quantization_version = 2
```

### The Loading Process (Step by Step)

**Step 1: Open and parse the GGUF header**

```
llama_model_load()
  -> llama_model_loader()
    -> Open file (mmap by default)
    -> Read magic, version, counts
    -> Parse all KV metadata into a map
    -> Parse all tensor info entries (name, shape, type, offset)
```

**Step 2: Determine model architecture**

Based on `general.architecture`, llama.cpp selects the correct model struct and graph-building function:

```
"llama"    -> LLM_ARCH_LLAMA     -> llm_build_llama()
"mistral"  -> same as llama (architecturally identical)
"qwen2"    -> LLM_ARCH_QWEN2     -> llm_build_qwen2()
"phi3"     -> LLM_ARCH_PHI3      -> llm_build_phi3()
"gemma"    -> LLM_ARCH_GEMMA     -> llm_build_gemma()
"gpt2"     -> LLM_ARCH_GPT2      -> llm_build_gpt2()
"starcoder" -> LLM_ARCH_STARCODER -> llm_build_starcoder()
... (40+ architectures supported)
```

**Step 3: Read hyperparameters**

From the KV metadata, populate the `llama_hparams` struct:

```c
struct llama_hparams {
    uint32_t n_vocab;
    uint32_t n_ctx_train;     // Training context length
    uint32_t n_embd;          // Embedding dimension
    uint32_t n_layer;         // Number of transformer blocks
    uint32_t n_head;          // Number of attention heads
    uint32_t n_head_kv;       // Number of KV heads (GQA)
    uint32_t n_ff;            // Feed-forward hidden dimension
    uint32_t n_expert;        // Number of experts (MoE)
    uint32_t n_expert_used;   // Active experts per token
    float    rope_freq_base;
    float    rope_freq_scale;
    // ... many more
};
```

**Step 4: Build the vocabulary/tokenizer**

From `tokenizer.ggml.*` keys, construct the tokenizer:
- Load token strings, scores, and types
- For BPE: load merge rules, build merge ranking table
- For SentencePiece: load unigram model
- Set special tokens (BOS, EOS, PAD, etc.)

**Step 5: Create model tensors and assign to backends**

This is where the backend system comes in:

```
For each tensor in GGUF:
  1. Determine which backend buffer to place it in:
     - If layer index < n_gpu_layers: assign to GPU buffer
     - Else: assign to CPU buffer
     - Embedding and output head: configurable placement
  2. Create a ggml_tensor in the appropriate ggml_context
  3. Register the tensor name -> tensor pointer mapping
```

**Step 6: Load tensor data**

Two paths based on `--no-mmap`:

**mmap path (default):**
- The GGUF file is memory-mapped via `mmap()` (Linux) or `CreateFileMapping()` (Windows)
- Tensor data pointers are set directly into the mapped region
- No data is copied -- the OS loads pages on demand from disk
- With `--mlock`, `mlock()` is called to pin the mapped region in RAM, preventing swap-out

**read() path (--no-mmap):**
- Backend buffers are allocated (e.g., `cudaMalloc` for CUDA, `malloc` for CPU)
- Tensor data is read from the file using standard I/O
- For GPU tensors: read into staging CPU buffer, then upload to GPU
- For CPU tensors: read directly into the allocated buffer

**Step 7: Validate and finalize**

- Verify all expected tensors were found
- Check shapes match hyperparameters
- Set up any derived state (RoPE frequency tables, alibi slopes, etc.)
- Print model summary (type, size, parameters, quantization)

### Memory Mapping Details

mmap is the preferred loading method because:
- **Near-instant startup**: Creating page table entries is microseconds. Actual data is demand-paged.
- **Shared memory**: Multiple processes using the same GGUF file share physical pages (e.g., running two copies of llama-server with the same model).
- **No double-buffering**: The file IS the memory. No need to allocate RAM and then copy from a file read buffer.
- **Graceful over-commitment**: You can "load" a model larger than RAM. Pages will be swapped in/out, though this thrashes and is slow.

---

## 5. Inference Pipeline

The inference pipeline for generating a single token follows this flow:

```
Input text
    |
    v
[Tokenization] -> token IDs
    |
    v
[Batch construction] -> llama_batch
    |
    v
[Graph build] -> ggml_cgraph (the transformer forward pass)
    |
    v
[Graph compute] -> backend_sched dispatches to CPU/GPU/etc.
    |
    v
[Extract logits] -> float[n_vocab] for last token
    |
    v
[Sampling] -> apply temperature, top-k, top-p, penalties -> next token ID
    |
    v
[Detokenization] -> text output
    |
    v
[Update KV cache] -> ready for next token
```

### Step 1: Tokenization

```c
// Tokenize input text
std::vector<llama_token> tokens = llama_tokenize(model, text, add_bos, special);
```

The tokenizer implementation depends on the model:
- **BPE** (GPT-2 style, used by LLaMA, Mistral, Qwen): Byte-pair encoding with merge rules. Pre-tokenizes by regex (splitting on whitespace, punctuation), then iteratively merges the most frequent pairs.
- **SentencePiece/Unigram** (used by some older models): Unigram language model selects the tokenization with highest probability.
- **WordPiece** (BERT-style): Greedy longest-match from a vocabulary.

### Step 2: Batch Construction

```c
struct llama_batch batch = llama_batch_init(n_tokens, 0, 1);
for (int i = 0; i < n_tokens; i++) {
    batch.token[i]    = tokens[i];
    batch.pos[i]      = i;              // Position in sequence
    batch.n_seq_id[i] = 1;
    batch.seq_id[i]   = &seq_id;       // Sequence ID (for parallel sequences)
    batch.logits[i]   = (i == n_tokens - 1);  // Only compute logits for last token
}
batch.n_tokens = n_tokens;
```

The batch structure allows:
- **Prompt processing** (prefill): Many tokens at once, only need logits for the last
- **Token generation** (decode): Single token, need logits for it
- **Parallel sequences**: Multiple sequences in one batch (different seq_ids)
- **Selective logit computation**: Set `batch.logits[i]` to decide which positions produce output logits

### Step 3: Graph Build (The Forward Pass as a Graph)

The call `llama_decode(ctx, batch)` triggers graph construction. Internally:

```c
// Pseudocode of the graph build for a LLaMA-style model
struct ggml_cgraph * build_llama(context, batch) {
    ggml_cgraph * gf = ggml_new_graph(ctx);

    // 1. Token embedding lookup
    cur = ggml_get_rows(ctx, model.tok_embd, tokens);  // [n_embd, n_tokens]

    // 2. For each transformer layer:
    for (int il = 0; il < n_layer; il++) {
        struct ggml_tensor * inpSA = cur;  // Save for residual

        // --- Attention sub-layer ---

        // a. RMS Norm (pre-norm)
        cur = ggml_rms_norm(ctx, cur);
        cur = ggml_mul(ctx, cur, model.layers[il].attn_norm);

        // b. Q, K, V projections
        Qcur = ggml_mul_mat(ctx, model.layers[il].wq, cur);  // [n_embd, n_tokens] x [n_embd, n_embd] -> [n_embd, n_tokens]
        Kcur = ggml_mul_mat(ctx, model.layers[il].wk, cur);  // [n_embd, n_tokens] x [n_embd, n_kv_embd]
        Vcur = ggml_mul_mat(ctx, model.layers[il].wv, cur);

        // c. Apply RoPE (Rotary Position Embeddings)
        Qcur = ggml_rope_ext(ctx, Qcur, positions, rope_freqs, ...);
        Kcur = ggml_rope_ext(ctx, Kcur, positions, rope_freqs, ...);

        // d. Store K, V in KV cache
        kv_cache_store(ctx, il, Kcur, Vcur, kv_head, batch);

        // e. Attention computation
        // Option A: Flash attention (fused, memory efficient)
        cur = ggml_flash_attn_ext(ctx, Q, K_from_cache, V_from_cache, mask, scale);
        // Option B: Standard attention (Q*K^T -> softmax -> *V)
        KQ = ggml_mul_mat(ctx, K_from_cache, Q);
        KQ = ggml_soft_max_ext(ctx, KQ, mask, scale);
        cur = ggml_mul_mat(ctx, V_from_cache, KQ);

        // f. Output projection
        cur = ggml_mul_mat(ctx, model.layers[il].wo, cur);

        // g. Residual connection
        cur = ggml_add(ctx, cur, inpSA);

        struct ggml_tensor * inpFF = cur;  // Save for residual

        // --- Feed-Forward sub-layer ---

        // h. RMS Norm
        cur = ggml_rms_norm(ctx, cur);
        cur = ggml_mul(ctx, cur, model.layers[il].ffn_norm);

        // i. FFN: SwiGLU variant
        // gate = W_gate * cur, up = W_up * cur
        // cur = silu(gate) * up
        // cur = W_down * cur
        gate = ggml_mul_mat(ctx, model.layers[il].ffn_gate, cur);
        up   = ggml_mul_mat(ctx, model.layers[il].ffn_up, cur);
        gate = ggml_silu(ctx, gate);
        cur  = ggml_mul(ctx, gate, up);
        cur  = ggml_mul_mat(ctx, model.layers[il].ffn_down, cur);

        // j. Residual connection
        cur = ggml_add(ctx, cur, inpFF);
    }

    // 3. Final RMS Norm
    cur = ggml_rms_norm(ctx, cur);
    cur = ggml_mul(ctx, cur, model.output_norm);

    // 4. Language model head (project to vocabulary)
    cur = ggml_mul_mat(ctx, model.output, cur);  // [n_embd, n_tokens] x [n_embd, n_vocab] -> [n_vocab, n_tokens]

    return gf;
}
```

**Important**: This function does NOT compute anything. It builds a computation graph (DAG) of tensor operations. The actual computation happens in the next step.

### Step 4: Graph Compute

```c
ggml_backend_sched_graph_compute(ctx->sched, gf);
```

The scheduler:
1. Assigns each node to a backend
2. Splits the graph at backend boundaries
3. Inserts copy operations for cross-backend data transfers
4. Dispatches sub-graphs to each backend's `graph_compute()` function
5. Handles synchronization

For the CPU backend, `graph_compute` iterates through nodes topologically and executes each operation using SIMD-optimized kernels with thread parallelism.

### Step 5: Extract Logits and Sample

```c
float * logits = llama_get_logits_ith(ctx, batch_idx);
// logits is a float[n_vocab] array

llama_token new_token = llama_sampler_sample(sampler, ctx, batch_idx);
```

### Step 6: Detokenization

```c
std::string piece = llama_token_to_piece(model, new_token);
```

The detokenizer reverses the tokenization, handling:
- BPE byte-level tokens
- SentencePiece underscore-as-space convention
- Multi-byte UTF-8 characters that may span multiple tokens

---

## 6. KV Cache Architecture

The Key-Value cache is one of the most critical and complex subsystems. It stores the key and value projections from all previous tokens across all layers, avoiding recomputation during autoregressive generation.

### Structure

```c
struct llama_kv_cache {
    uint32_t head;          // Current write position (ring buffer head)
    uint32_t size;          // Total number of slots (= context size)
    uint32_t used;          // Number of occupied slots

    // Per-slot metadata
    std::vector<llama_kv_cell> cells;  // Sequence assignments, positions

    // The actual K and V tensors:
    // k_l[layer] has shape [n_kv_embd, kv_size] -- all K vectors for one layer
    // v_l[layer] has shape [n_kv_embd, kv_size] (or transposed for flash attn)
    std::vector<struct ggml_tensor *> k_l;  // [n_layer] K tensors
    std::vector<struct ggml_tensor *> v_l;  // [n_layer] V tensors

    // Type for quantized KV cache
    enum ggml_type type_k;  // Default: F16, can be Q8_0, Q4_0, etc.
    enum ggml_type type_v;
};

struct llama_kv_cell {
    llama_pos pos;                // Token position this cell represents
    llama_pos delta;              // Position shift (for context shifting)
    std::set<llama_seq_id> seq_id;  // Which sequences use this cell
    bool has_seq_id(llama_seq_id id) const;
};
```

### Memory Layout

The KV cache is allocated as a single contiguous buffer per layer:

```
KV Cache Memory (for one layer):
K tensor: [head_dim * n_kv_heads, kv_size] in row-major
V tensor: [head_dim * n_kv_heads, kv_size] (or transposed)

Total KV cache size = 2 * n_layer * n_kv_heads * head_dim * kv_size * sizeof(type)

Example: LLaMA 7B, 4096 context, FP16:
  = 2 * 32 * 8 * 128 * 4096 * 2 bytes
  = ~512 MB

With Q8_0 KV quantization (-ctk q8_0 -ctv q8_0):
  = ~256 MB (halved)
```

### KV Cache Operations

**Insertion**: When new tokens are processed, their K and V projections are written to the cache at position `kv_head`. The head advances. This is implemented as tensor copy operations in the graph.

**Lookup**: During attention computation, the full K and V tensors from the cache are used as inputs. Only the cells marked as belonging to the current sequence (and within the attention window) are relevant; the attention mask zeroes out irrelevant positions.

**Slot management**: Each cell tracks which sequence(s) it belongs to and what position it represents. This enables:
- Multiple sequences sharing prefix KV entries (system prompt caching)
- Sequence forking (beam search, parallel completions)
- Selective clearing of specific sequences

### Context Shifting (Infinite Generation)

When the KV cache fills up (all `kv_size` slots used), llama.cpp can perform **context shifting** rather than failing:

1. Discard the oldest N tokens' KV entries (typically the first half or a configurable amount, keeping the beginning for system prompt)
2. Shift all remaining cells' positions by -N
3. New tokens are written starting from the freed slots

This allows unbounded generation at the cost of losing old context. The shift amount and behavior are configurable.

### KV Cache Defragmentation

Over time, as sequences are added and removed, the KV cache can become fragmented (gaps between active cells). llama.cpp implements defragmentation:

1. Identify gaps (unused cells between used cells)
2. Move used cells to fill gaps (compact the cache)
3. Update all cell metadata (positions, sequence IDs)
4. This is done as ggml copy operations in the computation graph

### KV Cache Quantization

By default, KV cache uses F16. For memory savings, you can quantize:

```bash
llama-server -m model.gguf -ctk q8_0 -ctv q8_0   # ~50% KV memory reduction
llama-server -m model.gguf -ctk q4_0 -ctv q4_0   # ~75% KV memory reduction
```

This quantizes the K and V tensors as they are stored. There is a small quality loss, especially at very low quantization (Q4_0 for KV is aggressive). Q8_0 KV quantization is generally safe.

---

## 7. Sampling Chain

Sampling is the process of selecting the next token from the probability distribution (logits) produced by the model. llama.cpp implements a **chain of sampling stages** that transform logits step by step.

### The Sampling Pipeline

```
Raw logits [n_vocab floats]
    |
    v
[Repetition penalty] -> Penalize recently used tokens
    |
    v
[Temperature] -> Sharpen or flatten distribution
    |
    v
[Top-K] -> Keep only K highest probability tokens
    |
    v
[Tail-Free Sampling (TFS)] -> Remove low-probability tail
    |
    v
[Locally Typical Sampling] -> Keep tokens close to expected entropy
    |
    v
[Top-P (Nucleus)] -> Keep tokens until cumulative probability >= P
    |
    v
[Min-P] -> Keep tokens with probability >= min_p * max_prob
    |
    v
[Temperature (final)] -> Optional second temperature application
    |
    v
[Token selection] -> Greedy, random weighted, Mirostat, etc.
    |
    v
Selected token ID
```

### Key Sampling Parameters

**Temperature** (default: 0.8):
- Divides logits by temperature before softmax
- T < 1.0: sharper distribution (more deterministic)
- T > 1.0: flatter distribution (more creative/random)
- T = 0.0: greedy (always pick highest probability)

**Top-K** (default: 40):
- Sort tokens by probability, keep only the top K
- K = 0: disabled
- Prevents sampling from the long tail of unlikely tokens

**Top-P / Nucleus Sampling** (default: 0.95):
- Sort by probability, keep tokens until cumulative probability reaches P
- More adaptive than top-K: adjusts effective vocabulary size per step
- P = 1.0: disabled

**Min-P** (default: 0.05):
- Keep tokens where probability >= min_p * probability_of_best_token
- Elegant alternative to top-K/top-P: adapts to the shape of the distribution
- Min-P = 0.0: disabled

**Repetition Penalty** (default: 1.1):
- Divide logits of recently seen tokens by the penalty factor
- Applied over a configurable window (`--repeat-last-n`, default 64)
- 1.0 = disabled, >1.0 = penalize repetition

**Frequency Penalty** and **Presence Penalty**:
- OpenAI-style penalties
- Frequency: `logit -= freq_penalty * count_in_context`
- Presence: `logit -= presence_penalty * (count > 0 ? 1 : 0)`

**Mirostat** (versions 1 and 2):
- Adaptive sampling that targets a specific "surprise" level (perplexity)
- Automatically adjusts the effective temperature/truncation to maintain consistent output quality
- Parameters: `tau` (target surprise), `eta` (learning rate)
- Does not use top-K/top-P; replaces them

### Grammar-Constrained Sampling (GBNF)

llama.cpp supports constraining output to follow a formal grammar defined in GBNF (GGML BNF) format:

```
root   ::= object
object ::= "{" ws pair ("," ws pair)* ws "}"
pair   ::= string ":" ws value
value  ::= string | number | "true" | "false" | "null" | object | array
...
```

The grammar sampler works by:
1. Maintaining a grammar parse state (stack of rules and positions)
2. Before token selection, masking out all tokens that would not advance the parse
3. After token selection, advancing the parse state
4. This guarantees the output is always a valid string in the grammar's language

This is particularly powerful for structured output (JSON, XML, code).

### Sampler Implementation

```c
// The sampler chain is a linked list of stages
struct llama_sampler_chain {
    std::vector<struct llama_sampler *> samplers;
    // Each sampler implements:
    //   void apply(llama_token_data_array * cur_p);
    //   void accept(llama_token token);
};

// Usage:
llama_sampler * smpl = llama_sampler_chain_init(params);
llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95, 1));
llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.8));
llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed));

llama_token token = llama_sampler_sample(smpl, ctx, idx);
llama_sampler_accept(smpl, token);  // Update internal state (penalties, grammar)
```

---

## 8. Batching and Slots (Server Architecture)

The `llama-server` implements continuous batching to handle multiple concurrent requests efficiently.

### Slot-Based Architecture

```c
struct server_slot {
    int id;
    int id_task;                    // Current task assigned to this slot
    enum slot_state state;          // IDLE, PROCESSING_PROMPT, GENERATING

    llama_seq_id seq_id;            // Sequence ID in the KV cache
    int n_past;                     // Tokens already in KV cache for this slot
    int n_prompt_tokens;            // Number of prompt tokens
    int n_decoded;                  // Number of generated tokens so far

    std::vector<llama_token> prompt_tokens;
    std::vector<llama_token> generated_tokens;

    // Sampling state
    struct llama_sampler * smpl;
    // ... parameters, timing, etc.
};
```

The server pre-creates a fixed number of slots (`--parallel N`). Each slot represents one concurrent inference session.

### Continuous Batching

Traditional batching processes one request at a time. Continuous batching (also called "iteration-level scheduling") dynamically combines tokens from multiple requests into a single batch, maximizing hardware utilization.

```
Time Step 1:
  Batch = [Slot 0: prompt tokens 0-127] + [Slot 1: generate token 45] + [Slot 2: generate token 12]

Time Step 2:
  Batch = [Slot 0: prompt tokens 128-255] + [Slot 1: generate token 46] + [Slot 2: generate token 13] + [Slot 3: prompt tokens 0-63]

Time Step 3:
  Batch = [Slot 0: generate token 1] + [Slot 1: generate token 47] + [Slot 2: EOS, slot freed] + [Slot 3: prompt tokens 64-127]
```

### Request Processing Flow

```
HTTP Request arrives
    |
    v
[Parse request] -> Extract prompt, parameters, stream flag
    |
    v
[Queue task] -> Add to task queue
    |
    v
[Main loop picks up task] -> Assigns to idle slot
    |
    v
[Slot: PROCESSING_PROMPT]
    |-- Tokenize prompt
    |-- Check for prompt cache hit (reuse KV cache if prefix matches)
    |-- Add prompt tokens to next batch
    |
    v
[Batch submitted to llama_decode()]
    |
    v
[Slot: GENERATING]
    |-- Sample next token
    |-- Check stopping conditions (EOS, max tokens, stop strings)
    |-- If streaming: send SSE event with token
    |-- Add generated token to next batch
    |-- Repeat until done
    |
    v
[Slot: IDLE] -> Available for next request
```

### Server Main Loop (Simplified)

```c
while (running) {
    // 1. Dequeue waiting tasks, assign to idle slots
    for (auto & task : pending_tasks) {
        server_slot * slot = find_idle_slot();
        if (slot) assign_task_to_slot(slot, task);
    }

    // 2. Build batch from all active slots
    llama_batch batch = llama_batch_init(...);
    for (auto & slot : slots) {
        if (slot.state == PROCESSING_PROMPT) {
            // Add a chunk of prompt tokens
            add_prompt_tokens_to_batch(batch, slot);
        }
        else if (slot.state == GENERATING) {
            // Add the last generated token
            add_single_token_to_batch(batch, slot);
        }
    }

    // 3. Run inference
    if (batch.n_tokens > 0) {
        llama_decode(ctx, batch);
    }

    // 4. Process results
    for (auto & slot : slots) {
        if (slot.state == GENERATING) {
            llama_token token = sample(slot);
            if (is_done(slot, token)) {
                complete_slot(slot);
            } else {
                slot.generated_tokens.push_back(token);
                if (slot.streaming) send_sse(slot, token);
            }
        }
    }
}
```

### Prompt Caching

The server implements prompt caching: if a new request's prompt shares a prefix with a slot's existing KV cache (same system prompt, for example), only the new suffix needs to be processed. The shared prefix's KV cache entries are reused.

This is very effective for chat applications where the system prompt + conversation history grows incrementally.

### API Endpoints

| Endpoint | Description |
|----------|-------------|
| POST /v1/chat/completions | OpenAI-compatible chat API |
| POST /v1/completions | OpenAI-compatible text completion |
| POST /v1/embeddings | Text embedding extraction |
| GET /v1/models | List loaded models |
| GET /health | Health check (loading, ready, error) |
| GET /slots | Current slot status |
| GET /metrics | Prometheus metrics (tokens/sec, queue depth, etc.) |
| POST /completion | Native llama.cpp format |
| POST /tokenize | Tokenize text to token IDs |
| POST /detokenize | Token IDs to text |

---

## 9. Memory Allocation Strategy

llama.cpp uses a deliberate strategy to minimize dynamic allocation during inference, which is critical for deterministic performance and avoiding fragmentation.

### Memory Regions

The system uses several distinct memory regions:

**1. Model Weights Buffer**
- Allocated once at model load
- Typically the largest allocation (e.g., ~4.5 GB for 7B Q4_K_M)
- Managed by the backend buffer system (`ggml_backend_buffer`)
- For CPU: mmap'd from GGUF file (zero-copy), or malloc'd and filled
- For GPU: cudaMalloc'd in VRAM, data uploaded from host
- Never freed until model is unloaded

**2. KV Cache Buffer**
- Allocated once at context creation
- Size = 2 * n_layer * n_kv_heads * head_dim * kv_size * sizeof(kv_type)
- Managed by a backend buffer (same backend as the layers it serves)
- Pre-allocated to maximum context size, not grown dynamically
- This is why `-c 4096` vs `-c 32768` has a huge impact on RAM usage

**3. Compute Buffer (Scratch)**
- Pre-allocated by `ggml_backend_sched`
- Used for intermediate tensors during graph computation
- The scheduler performs a "dry run" (measure pass) to determine the worst-case memory needed
- Allocated once, reused every inference step
- Two buffers used in alternation (double-buffering) for pipeline overlap

**4. Graph Metadata (ggml_context)**
- Small arena (a few MB) for tensor descriptor structs
- Allocated via ggml_init with a fixed `mem_size`
- Contains only tensor metadata (~200 bytes per tensor), not data
- Rebuilt every inference step (graphs are reconstructed each time)

### The Measure Pass

Before the first inference, the scheduler performs a "measure" pass:

1. Build the computation graph (same as real inference)
2. Walk the graph, simulating memory allocation for each tensor
3. Determine the peak memory usage for each backend
4. Allocate compute buffers of exactly that size
5. From then on, every inference step reuses these pre-allocated buffers

This is implemented in `ggml_gallocr` (graph allocator):

```c
struct ggml_gallocr {
    // Per-buffer allocation plan
    struct {
        size_t max_size;       // Peak memory needed
        ggml_backend_buffer_t buffer;  // The allocated buffer
    } buffers[MAX_BUFFERS];

    // Per-tensor allocation info
    struct {
        int buffer_id;         // Which buffer this tensor uses
        size_t offset;         // Offset within that buffer
    } tensor_allocs[MAX_TENSORS];
};
```

The graph allocator uses a greedy algorithm:
- Tensors that are no longer needed (all consumers have been computed) have their memory freed for reuse
- This dramatically reduces peak memory vs. naive "allocate everything at once"
- The allocation plan is computed once and reused unless the graph structure changes

### Memory Flow Summary

```
Startup:
  1. mmap GGUF file -> model weights (zero-copy)
  2. Allocate KV cache buffer (backend_buffer_alloc)
  3. Build test graph -> measure pass -> allocate compute buffers

Each inference step:
  4. Build computation graph (cheap: just tensor descriptors in ggml_context)
  5. Assign tensors to pre-computed buffer offsets (ggml_gallocr_alloc_graph)
  6. Execute graph (all data in pre-allocated buffers)
  7. Reset ggml_context for next step (cheap: just reset bump pointer)

No malloc/free during steady-state inference.
```

### mmap and Virtual Memory

The default loading path uses mmap, which means:

```
Virtual Address Space:
  [0x7f0000000000 - 0x7f0100000000]  GGUF file mapped (4 GB for 7B Q4_K_M)
  [0x7f0200000000 - 0x7f0220000000]  KV cache (512 MB for 4K ctx FP16)
  [0x7f0220000000 - 0x7f0228000000]  Compute buffers (~128 MB)
  [heap, stack, shared libs, etc.]

Physical RAM (with mmap, pages loaded on demand):
  - Initially: only pages accessed during graph build are resident
  - During first prompt: pages with used weight tensors are faulted in
  - Steady state: most weight pages resident, OS may evict under memory pressure

With --mlock:
  - All mmap'd pages are locked in RAM immediately
  - mlockall() prevents any swapping
  - Guarantees consistent performance (no page fault latency)
```

---

## 10. The Computation Graph

### Graph Structure

ggml uses a static computation graph (`ggml_cgraph`):

```c
struct ggml_cgraph {
    int size;                    // Maximum number of nodes
    int n_nodes;                 // Actual number of nodes (operations)
    int n_leafs;                 // Number of leaf nodes (inputs/weights)

    struct ggml_tensor ** nodes; // Topologically sorted operation nodes
    struct ggml_tensor ** grads; // Gradient tensors (for training, rarely used in inference)
    struct ggml_tensor ** leafs; // Input/weight tensors (no source operation)

    // Hash table for quick tensor lookup
    struct ggml_hash_set visited_hash_set;
};
```

### Graph Construction

The graph is built **implicitly** through tensor operations. Each call to `ggml_mul_mat()`, `ggml_add()`, etc. creates a new tensor that records:
- Its operation type (`op`)
- Its source tensors (`src[0]`, `src[1]`, ...)
- Its shape and type

Then `ggml_build_forward_expand(graph, final_tensor)` walks backward from the final tensor through `src[]` pointers, collecting all nodes into topological order.

### Graph for One Transformer Layer

A single transformer layer (LLaMA-style) produces roughly this computation graph:

```
Input (from previous layer)
  |
  +--[ggml_rms_norm]--[ggml_mul (norm weights)]
  |     |
  |     +--[ggml_mul_mat (Wq)]--[ggml_rope]--+
  |     |                                      |
  |     +--[ggml_mul_mat (Wk)]--[ggml_rope]--+--[Flash Attention or Q*K, softmax, *V]
  |     |                                      |
  |     +--[ggml_mul_mat (Wv)]----------------+
  |                                             |
  |                              [ggml_mul_mat (Wo)]
  |                                             |
  +--[ggml_add (residual)]---------------------+
  |
  +--[ggml_rms_norm]--[ggml_mul (ffn norm weights)]
  |     |
  |     +--[ggml_mul_mat (W_gate)]--[ggml_silu]--+
  |     |                                          |--[ggml_mul]--[ggml_mul_mat (W_down)]
  |     +--[ggml_mul_mat (W_up)]------------------+
  |                                                                        |
  +--[ggml_add (residual)]------------------------------------------------+
  |
  Output (to next layer)
```

### Graph Size

A full model graph has roughly:
- ~20-30 nodes per transformer layer
- Plus embedding lookup, final norm, and output projection
- For a 32-layer model: ~700-1000 nodes total
- This is rebuilt from scratch every inference step (cheap: just pointer manipulation)

### Flash Attention Optimization

When flash attention is enabled (`-fa`), the Q*K^T, softmax, and *V operations are fused into a single `ggml_flash_attn_ext` node:

**Without flash attention:**
```
Q, K_cache, V_cache
  -> ggml_mul_mat(K^T, Q) -> [n_ctx, n_tokens, n_heads]
  -> ggml_soft_max_ext(KQ, mask, scale)
  -> ggml_mul_mat(V, KQ_soft) -> [head_dim, n_tokens, n_heads]
```
This materializes the full attention matrix [n_ctx, n_tokens] which is O(n^2) in memory.

**With flash attention:**
```
Q, K_cache, V_cache
  -> ggml_flash_attn_ext(Q, K, V, mask, scale) -> [head_dim, n_tokens, n_heads]
```
Never materializes the full attention matrix. Computes attention in tiles, O(n) memory. Significant memory savings for long contexts.

### MoE (Mixture of Experts) Graph

For models like Mixtral, the FFN graph is more complex:

```
Input to FFN
  |
  [ggml_mul_mat (gate/router)]  -> [n_expert] scores per token
  |
  [ggml_top_k]  -> select top-K experts (e.g., K=2)
  |
  [ggml_mul_mat_id (expert FFN weights, selected indices)]
  |
  -> Weighted sum of expert outputs
```

`ggml_mul_mat_id` is a specialized operation that performs matrix multiplication against different weight matrices based on per-row indices -- the key primitive for efficient MoE inference.

### Graph Optimization

ggml performs limited graph optimization:
- **Operation fusion**: Flash attention fuses Q*K*V, softmax
- **In-place operations**: Where safe, operations write output over their input (e.g., `ggml_add` can be in-place, saving a buffer)
- **View/reshape elimination**: View operations are zero-cost (just metadata)
- **Memory reuse via graph allocator**: The allocator assigns overlapping memory regions to tensors with non-overlapping lifetimes

The graph is NOT optimized by extensive pass-based transformations like in deep learning frameworks (no kernel fusion pass, no layout optimization pass, etc.). The philosophy is to keep it simple and let backend-specific kernels handle performance.

---

## Summary: How Everything Fits Together

```
User sends request to llama-server
    |
    v
Server assigns request to a slot
    |
    v
Tokenizer converts text to token IDs
    |
    v
Tokens added to llama_batch with positions and sequence IDs
    |
    v
llama_decode() called:
    |
    +-- Graph builder constructs ggml_cgraph based on model architecture
    |     (one of: llm_build_llama, llm_build_qwen2, etc.)
    |     KV cache read/write operations embedded in graph
    |
    +-- ggml_backend_sched splits graph across backends
    |     CPU tensors -> CPU sub-graph
    |     GPU tensors -> CUDA sub-graph
    |     Copy nodes at boundaries
    |
    +-- Each backend executes its sub-graph:
    |     CPU: SIMD-optimized kernels (AVX2/AVX-512/NEON)
    |     GPU: CUDA/Vulkan/Metal compute shaders
    |     All using pre-allocated scratch buffers
    |
    v
Logits extracted from final tensor
    |
    v
Sampling chain applied:
    penalties -> temperature -> top-K -> top-P -> min-P -> token selection
    (optionally grammar-constrained)
    |
    v
Token sent back to client (SSE stream or final response)
KV cache updated for next iteration
Slot ready for next token
```

The key engineering insight of llama.cpp is that by pre-allocating all memory, rebuilding the computation graph each step (cheap), and using quantized tensor operations as the fundamental primitive, it achieves high performance on consumer hardware without any framework overhead. The backend abstraction allows the same graph to transparently execute on CPUs, GPUs, or across a network, while the server's continuous batching maximizes throughput for concurrent users.
