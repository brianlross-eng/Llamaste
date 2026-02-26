# llama.cpp: Build System, Server, and Embedding in Minimal Linux

## Static Binary Compilation

### Core CMake Build Recipe (CPU-only, static)

```bash
cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_STATIC=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=ON \
  -DGGML_CPU=ON \
  -DGGML_CUDA=OFF \
  -DGGML_VULKAN=OFF \
  -DGGML_METAL=OFF \
  -DGGML_RPC=OFF \
  -DGGML_BLAS=OFF \
  -DLLAMA_CURL=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-static" \
  -DCMAKE_C_FLAGS="-static" \
  -DCMAKE_CXX_FLAGS="-static"

cmake --build build --config Release -j$(nproc)
strip build/bin/llama-server build/bin/llama-cli build/bin/llama-bench
```

### For Distributable Binaries (multi-SIMD dispatch)

```bash
cmake -B build \
  -DGGML_CPU_ALL_VARIANTS=ON \
  -DGGML_NATIVE=OFF \
  # ... rest of static flags same as above
```

### musl libc for Truly Portable Static Binaries

```bash
CC=musl-gcc CXX=musl-g++ cmake -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DGGML_STATIC=ON \
  -DBUILD_SHARED_LIBS=OFF \
  -DGGML_NATIVE=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-static"
```

### Static Binary Sizes (CPU-only, stripped)

- llama-server: ~3-5 MB
- llama-cli: ~2-4 MB
- llama-bench: ~2-3 MB

## llama-server Component

### OpenAI-Compatible API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| /v1/chat/completions | POST | Chat completions (messages array) |
| /v1/completions | POST | Text completions (raw prompt) |
| /v1/embeddings | POST | Text embeddings |
| /v1/models | GET | List loaded models |
| /health | GET | Health check |
| /slots | GET | Active inference slots |
| /metrics | GET | Prometheus-compatible metrics |
| /completion | POST | Native llama.cpp format |
| /tokenize | POST | Tokenize text |
| /detokenize | POST | Detokenize tokens |

### Key Command-Line Options

```bash
llama-server \
  -m /path/to/model.gguf \     # Model file
  --host 0.0.0.0 \             # Listen address
  --port 8080 \                # Listen port
  -c 4096 \                    # Context size (tokens)
  -ngl 0 \                     # GPU layers (0 = all CPU)
  -t 8 \                       # Generation threads
  -tb 8 \                      # Batch processing threads
  --parallel 4 \               # Concurrent request slots
  --cont-batching \            # Enable continuous batching
  --mlock \                    # Lock model in RAM
  -fa \                        # Flash Attention
  --metrics \                  # Enable /metrics endpoint
  --api-key "secret" \         # API authentication
  --chat-template chatml       # Chat template
```

### Built-in Web UI

Compiled into the binary at build time. Provides chat interface, completion playground, parameter controls, and token probability visualization. Disable with `--no-webui`.

## CPU Feature Detection

### Two Modes

**GGML_NATIVE=ON:** Compile-time optimization using `-march=native`. Optimal for known hardware. Crashes on older CPUs.

**GGML_CPU_ALL_VARIANTS=ON:** Builds multiple SIMD variants. Runtime selects the best:
- ggml-cpu-avx
- ggml-cpu-avx2
- ggml-cpu-avx512
- ggml-cpu-amx

### Performance by SIMD Level (relative to SSE4.2 baseline)

| SIMD Level | Speedup | Era |
|-----------|---------|-----|
| SSE4.2 | 1.0x | 2008+ |
| AVX | ~1.3x | Sandy Bridge 2011 |
| AVX2+FMA | ~2.0-2.5x | Haswell 2013 |
| AVX-512 | ~2.5-4.0x | Skylake-X 2017 |
| AMX | ~4.0-6.0x | Sapphire Rapids 2023 |

## Memory Mapping (mmap)

Default model loading method. Maps GGUF file directly into virtual address space.

**Benefits:**
- Near-instant "loading" (just creates page table entries)
- Shared memory between processes using same model
- Can "load" models larger than RAM (with thrashing)
- No double-buffering

**Control flags:**
- `--mlock`: Lock model in RAM, prevents swapping
- `--no-mmap`: Disable mmap, use standard read()

## RPC Distributed Inference

### Architecture

Pipeline parallelism: different layers on different machines, data flows sequentially.

### Setup

```bash
# Build with RPC
cmake -B build -DGGML_RPC=ON

# Worker nodes:
./llama-rpc-server --host 0.0.0.0 --port 50052

# Coordinator:
./llama-server -m model.gguf \
  --rpc worker1:50052,worker2:50052 \
  -ngl 99
```

### Limitations
- Unencrypted TCP (use on trusted networks only)
- No built-in authentication
- Pipeline parallelism only (no tensor parallelism)
- Static layer assignment

## Thread Configuration

- `-t N`: Generation threads (set to physical core count - 1)
- `-tb N`: Batch threads (set to physical core count)
- `--cpu-mask` / `--cpu-range`: Pin to specific cores
- `--numa distribute|isolate|numactl`: NUMA awareness

## CMake Build Flags Reference

### Backend Selection
| Flag | Default | Description |
|------|---------|-------------|
| GGML_CPU | ON | CPU backend |
| GGML_CUDA | OFF | NVIDIA CUDA |
| GGML_VULKAN | OFF | Vulkan compute |
| GGML_METAL | OFF | Apple Metal |
| GGML_RPC | OFF | RPC distributed |
| GGML_BLAS | OFF | BLAS library |

### CPU Optimization
| Flag | Default | Description |
|------|---------|-------------|
| GGML_NATIVE | ON | Use -march=native |
| GGML_CPU_ALL_VARIANTS | OFF | Multi-SIMD dispatch |
| GGML_AVX | ON | AVX instructions |
| GGML_AVX2 | ON | AVX2 instructions |
| GGML_AVX512 | OFF | AVX-512 |
| GGML_AMX_TILE | OFF | Intel AMX |

## GGUF Model Format

Self-describing binary format containing:
- Model architecture metadata
- Vocabulary and tokenizer
- Hyperparameters
- Chat template (Jinja2)
- Quantization details
- Tensor data (bulk of file)

## Quantization Formats

| Format | Bits/Weight | 7B Size | Quality vs FP16 | Recommended Use |
|--------|------------|---------|-----------------|-----------------|
| Q8_0 | 8.0 | ~7.5 GB | ~99.5% | Max quality |
| Q6_K | 6.5 | ~5.9 GB | ~99% | Near-lossless |
| Q5_K_M | 5.5 | ~5.1 GB | ~98% | High quality |
| **Q4_K_M** | **4.5** | **~4.4 GB** | **~96-97%** | **Default choice** |
| Q3_K_M | 3.5 | ~3.5 GB | ~92-94% | RAM constrained |
| IQ3_M | ~3.5 | ~3.1 GB | ~93-95% | Better than Q3 |
| Q2_K | 2.5 | ~2.8 GB | ~85-90% | Extreme low RAM |

## RAM Requirements

| Model + Quant | File Size | Total RAM Needed |
|---------------|-----------|-----------------|
| 1.5B Q4_K_M | ~1.0 GB | ~1.5 GB |
| 3B Q4_K_M | ~2.0 GB | ~3.0 GB |
| 7-8B Q4_K_M | ~4.5 GB | ~6.5 GB |
| 13B Q4_K_M | ~7.9 GB | ~10 GB |
| 32B Q4_K_M | ~19.5 GB | ~22 GB |
| 70B Q4_K_M | ~40 GB | ~48 GB |

## Minimum Binaries for Llamaste

1. **llama-server** (API + web UI, ~3-5 MB static) -- required
2. **llama-bench** (hardware validation) -- optional
3. **llama-rpc-server** (distributed worker) -- for clustering

## Recommended Kernel Parameters

```bash
vm.overcommit_memory=1
vm.max_map_count=1048576
vm.swappiness=10
```

## Recent Project Changes (2024-2025)

- Binaries renamed with `llama-` prefix (server -> llama-server, main -> llama-cli)
- ggml extracted into separate sub-directory
- Backends modularized under ggml/src/
- CMake flags migrated from LLAMA_* to GGML_* prefix
- Flash Attention added (-fa flag)
- Makefile build deprecated; CMake is primary
