# Distributed 128GB RAM Cluster: Android/ARM Device Research

**Date**: 2026-03-17
**Goal**: Find the cheapest collection of ARM/Android devices totaling 128GB RAM for distributed LLM inference via Llamaste mesh clustering
**Target budget**: Under $3,000-4,000 (significantly less than a dedicated GPU server)

---

## Baseline Comparison: Dedicated GPU Server

| Option | VRAM/RAM | Used Price (2026) | Performance |
|--------|----------|-------------------|-------------|
| NVIDIA A100 80GB (single GPU) | 80GB VRAM | $4,000-9,000 | ~100+ tok/s on 70B |
| NVIDIA A100 server (8-GPU) | 640GB VRAM | $16,000-50,000 | Massive throughput |
| Used 2x A100 80GB workstation | 160GB VRAM | $10,000-20,000 | 200+ tok/s |
| Single A100 80GB + 128GB system RAM | 80GB+128GB | ~$5,000-10,000 | 100+ tok/s (GPU layers) |

A used single A100 80GB GPU alone costs $4,000-9,000 — our target is to beat this price point with distributed ARM devices providing 128GB total system RAM for CPU-only inference.

---

## Device Categories & Pricing (March 2026)

### Category A: ARM Single Board Computers (Best Value)

| Device | CPU | RAM | Price (New) | Price (Used) | Notes |
|--------|-----|-----|-------------|--------------|-------|
| **Orange Pi 5 Plus 32GB** | RK3588 (4xA76 + 4xA55, 2.4GHz) | 32GB LPDDR4X | ~$170-200 | ~$130-160 | Dual 2.5GbE, PCIe, best value/GB |
| **Orange Pi 5 32GB** | RK3588S (4xA76 + 4xA55) | 32GB LPDDR4X | ~$140-170 | ~$110-140 | No dual ethernet |
| **Orange Pi 5 16GB** | RK3588S | 16GB LPDDR4X | ~$90-110 | ~$70-90 | Good budget option |
| **Radxa ROCK 5T 32GB** | RK3588 (4xA76 + 4xA55) | 32GB LPDDR4X | ~$220 | ~$170-190 | Dual M.2, excellent I/O |
| **Radxa ROCK 5B 16GB** | RK3588 | 16GB LPDDR4X | ~$189 | ~$140-160 | Mature ecosystem |
| **Khadas Edge2 Pro 16GB** | RK3588S (4xA76 + 4xA55) | 16GB LPDDR4X | ~$299-330 | ~$220-260 | Credit card sized, WiFi 6 |
| **Raspberry Pi 5 16GB** | BCM2712 (4xA76, 2.4GHz) | 16GB LPDDR4X | **$205** | ~$170-190 | Price inflated by RAM shortage |
| **Firefly ROC-RK3588-PC 32GB** | RK3588 | 32GB LPDDR4X | ~$549-615 | ~$400-500 | Industrial grade, overpriced |

**Key insight**: The Orange Pi 5 Plus 32GB at ~$170-200 offers the best RAM-per-dollar at ~$5.50-6.25/GB.

### Category B: Android Tablets (Used/Refurbished)

| Device | CPU | RAM | Used Price | Notes |
|--------|-----|-----|------------|-------|
| **Samsung Galaxy Tab S8** | Snapdragon 8 Gen 1 | 8GB | ~$298 (Swappa) | WiFi, good CPU |
| **Samsung Galaxy Tab S8+** | Snapdragon 8 Gen 1 | 8GB | ~$300-350 | Larger screen |
| **Samsung Galaxy Tab S8 Ultra** | Snapdragon 8 Gen 1 | 8-16GB | ~$400+ (Swappa) | 16GB version rare/expensive |
| **Samsung Galaxy Tab S9 FE** | Exynos 1380 | 6-8GB | ~$268 (refurb) | Weaker CPU (A78 cores) |
| **Xiaomi Pad 6 Pro** | Snapdragon 870 | 8GB | ~$200-250 | Good CPU, hard to find in US |
| **Lenovo Tab P12 Pro** | Snapdragon 870 | 8GB | ~$200-280 | |
| **Samsung Galaxy Tab S7 FE** | Snapdragon 778G | 4-6GB | ~$150-200 | Only 4-6GB, not ideal |
| **OnePlus Pad** | Dimensity 9000 | 8-12GB | ~$250-300 | 12GB version ~$300 |
| **Xiaomi Pad 7 Pro** | Snapdragon 8 Gen 1 | 8-12GB | ~$300-350 | Newer, 12GB available |

**Key insight**: Android tablets average ~$30-40/GB of RAM — 5-7x worse value than ARM SBCs.

### Category C: RK3588 Mini PCs

| Device | CPU | RAM | Price | Notes |
|--------|-----|-----|-------|-------|
| **DEC-3588 Mini PC 32GB** | RK3588 | 32GB | ~$250-350 | Android 12, eBay |
| **DEC-3588 Mini PC 16GB** | RK3588 | 16GB | ~$180-250 | |
| **Firefly EC-A3588L 32GB** | RK3588 | 32GB | ~$400-500 | Industrial, fanless |
| **Generic RK3588 TV Box 8GB** | RK3588 | 8GB | ~$100-150 | H96 Max M9 etc. |

### Category D: Android TV Boxes (Cheapest, Weakest)

| Device | CPU | RAM | Price | Notes |
|--------|-----|-----|-------|-------|
| **H96 Max M9 (RK3576)** | RK3576 (4xA72 + 4xA53) | 8GB | ~$60-80 | Decent cores |
| **Generic RK3528 boxes** | RK3528 (4xA53) | 2-4GB | ~$25-40 | Too weak, A53 only |
| **T95Z Plus (H618)** | Allwinner H618 (4xA53) | 4GB | ~$35-50 | Too weak |

**Key insight**: Most cheap TV boxes use Cortex-A53 cores only — terrible for inference. Only RK3576/RK3588-based boxes with A72/A76 cores are viable.

### Category E: Used Chromebooks (x86 alternative)

| Device | CPU | RAM | Used Price | Notes |
|--------|-----|-----|------------|-------|
| **Bulk refurb Chromebooks 8GB** | Various Intel/AMD | 8GB | ~$80-120 ea | x86 = AVX2 support |
| **HP Fortis x360 G5 8GB** | Intel N-series | 8GB | ~$90-110 | |
| **Lenovo IdeaPad 3 8GB** | Various | 8GB | ~$100-130 | |

**Key insight**: Chromebooks need Linux installed (Crouton/Crostini or full ChromeOS Flex). x86 CPUs with AVX2 may actually outperform ARM NEON for llama.cpp inference.

---

## Configurations to Reach 128GB

### Config 1: Orange Pi 5 Plus 32GB Fleet (BEST VALUE)
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 4 | Orange Pi 5 Plus 32GB | 32GB | $180 | $720 |
| 4 | SD card 64GB + power supply | - | $25 | $100 |
| 1 | Gigabit switch (8-port) | - | $20 | $20 |
| 4 | Ethernet cables | - | $5 | $20 |
| | | **128GB** | | **$860** |

**Pros**: Cheapest path to 128GB. Dual 2.5GbE on each board. RK3588 has 4x Cortex-A76 @ 2.4GHz.
**Cons**: 4 nodes = more network overhead per inference step.

### Config 2: Mixed Orange Pi (Budget Optimized)
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 2 | Orange Pi 5 Plus 32GB | 64GB | $180 | $360 |
| 4 | Orange Pi 5 16GB | 64GB | $100 | $400 |
| 6 | SD + PSU | - | $25 | $150 |
| 1 | 8-port gigabit switch | - | $20 | $20 |
| | | **128GB** | | **$930** |

### Config 3: RK3588 Mini PC Fleet
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 4 | DEC-3588 Mini PC 32GB | 32GB | $300 | $1,200 |
| 1 | 8-port gigabit switch | - | $20 | $20 |
| | | **128GB** | | **$1,220** |

**Pros**: Ready-to-run mini PCs, no assembly. Built-in eMMC storage.
**Cons**: Higher price than bare SBCs.

### Config 4: Raspberry Pi 5 Fleet
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 8 | Raspberry Pi 5 16GB | 16GB | $205 | $1,640 |
| 8 | SD card + PSU + case | - | $30 | $240 |
| 1 | 8-port gigabit switch | - | $20 | $20 |
| | | **128GB** | | **$1,900** |

**Pros**: Largest ecosystem, best documentation, most community support.
**Cons**: Expensive due to 2026 RAM price crisis ($205 per board). 8 nodes = high network overhead.

### Config 5: Samsung Galaxy Tab S8 Fleet (Android Tablets)
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 16 | Samsung Galaxy Tab S8 (used, 8GB) | 8GB | $298 | $4,768 |
| | | **128GB** | | **$4,768** |

OR with 12GB+ tablets:
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 8 | Galaxy Tab S8 Ultra 16GB (used) | 16GB | $500+ | $4,000+ |
| | | **128GB** | | **$4,000+** |

**Pros**: Built-in battery (UPS), screen, WiFi 6, Snapdragon 8 Gen 1 (A710 cores).
**Cons**: Expensive per GB. Software complexity (Termux). Cannot easily run Llamaste natively. WiFi mesh is slower than Ethernet.

### Config 6: Hybrid (SBCs + Tablets)
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 2 | Orange Pi 5 Plus 32GB | 64GB | $180 | $360 |
| 4 | Used Galaxy Tab S8 8GB | 32GB | $250 | $1,000 |
| 2 | Orange Pi 5 16GB | 32GB | $100 | $200 |
| | Accessories (switch, cables, SD, PSU) | - | - | $200 |
| | | **128GB** | | **$1,760** |

### Config 7: Bulk Refurb Chromebooks (x86 Alternative)
| Qty | Device | RAM | Unit Price | Total |
|-----|--------|-----|------------|-------|
| 16 | Refurb Chromebook 8GB (bulk lot) | 8GB | $90 | $1,440 |
| 1 | 16-port gigabit switch | - | $40 | $40 |
| | | **128GB** | | **$1,480** |

**Pros**: x86 AVX2 may beat ARM NEON. Built-in screen, keyboard, battery. Cheap in bulk.
**Cons**: 16 nodes = extreme network overhead. Need Linux installed. Low-end CPUs (Celeron/N-series).

---

## Performance Estimates

### Per-Node Inference Speed (llama.cpp, Q4_K_M quantization)

| Device / CPU | 1.5B Model | 3B Model | 7B Model | 14B Model |
|-------------|------------|----------|----------|-----------|
| **RK3588 (OPi 5 Plus)** | ~15-28 tok/s | ~5-10 tok/s | ~2-4 tok/s | OOM (16GB) / ~1 tok/s (32GB) |
| **Raspberry Pi 5** | ~8-15 tok/s | ~3-5 tok/s | ~1-2 tok/s | OOM (16GB) |
| **Snapdragon 8 Gen 1** | ~10-20 tok/s | ~5-8 tok/s | ~2-4 tok/s | N/A (8GB) |
| **Cortex-A53 (TV box)** | ~2-5 tok/s | ~1-2 tok/s | OOM/unusable | N/A |
| **i5-1035G1 (Llamaste ref)** | ~40 tok/s | ~14 tok/s | ~6-8 tok/s | ~2.2 tok/s |

### Distributed Cluster Performance (Estimates)

For a 70B Q4_K_M model (~40GB) distributed across 128GB cluster:

| Config | Nodes | Network | Est. Speed | Limiting Factor |
|--------|-------|---------|------------|-----------------|
| 4x OPi 5 Plus 32GB (GbE) | 4 | 2.5GbE | ~1-3 tok/s | Network bandwidth |
| 4x OPi 5 Plus 32GB (WiFi) | 4 | WiFi 5 | ~0.3-1 tok/s | WiFi latency |
| 8x RPi 5 16GB (GbE) | 8 | 1GbE | ~0.5-1.5 tok/s | Network round-trips |
| 16x Tab S8 (WiFi) | 16 | WiFi 6 | ~0.1-0.5 tok/s | Node count, WiFi |

**Critical insight from llama.cpp RPC benchmarks**: Network bandwidth is the primary bottleneck in distributed inference. Each forward pass requires transferring tensor data between nodes sequentially (not parallel). With 1GbE, users report ~500Mbps effective throughput. 2.5GbE significantly helps. WiFi is borderline unusable for real-time inference with many nodes.

### Power Consumption

| Device | Idle | Under Load | Annual (24/7 load) |
|--------|------|------------|---------------------|
| Orange Pi 5 Plus | ~5W | ~12-15W | ~$13-16/year |
| Raspberry Pi 5 | ~4W | ~10-12W | ~$11-13/year |
| Samsung Galaxy Tab S8 | ~2W | ~8-10W | ~$9-11/year |
| i5 laptop (ref) | ~15W | ~35-45W | ~$38-49/year |
| A100 GPU server | ~100W | ~300-400W | ~$330-440/year |

4x Orange Pi 5 Plus cluster: ~48-60W max = ~$52-66/year
vs A100 server: ~300-400W = ~$330-440/year

**Power savings**: 5-8x less power consumption than a GPU server.

---

## Software Requirements

### For ARM SBCs (Orange Pi, Raspberry Pi, Radxa)
- **OS**: Ubuntu/Debian ARM64 (native, full Linux)
- **Inference**: llama.cpp built with NEON SIMD (`-DGGML_NEON=ON`)
- **Clustering**: llama-rpc-server on worker nodes, llama-server on master
- **Llamaste integration**: Compile Llamaste for ARM64 (musl or glibc). Each SBC runs llama-rpc-server. Master node runs full Llamaste binary.
- **Network**: Ethernet preferred (2.5GbE on OPi 5 Plus)

### For Android Tablets (Termux)
- **Runtime**: Termux (no root required)
- **Build**: `pkg install clang cmake` in Termux, then build llama.cpp
- **Limitations**:
  - Cannot run Llamaste natively (Android != Linux for PID 1)
  - Each tablet runs llama-rpc-server in Termux
  - WiFi only (no Ethernet without USB-C adapter)
  - Android may kill background processes (need wake lock / Termux:Boot)
  - No access to GPU in llama.cpp on most Android devices (Vulkan support limited)
  - Snapdragon CPU with NEON gives ~4 tok/s on 7B with CPU, ~16 tok/s with OpenCL (limited support)

### For Chromebooks
- **OS**: Linux via Crostini, or wipe and install full Linux
- **Build**: Standard llama.cpp x86_64 build with AVX2
- **Advantage**: x86 AVX2 typically 2-3x faster than ARM NEON per core for quantized inference
- **Disadvantage**: Low-end Intel N-series CPUs have 4 cores, limited memory bandwidth

---

## Pros and Cons Summary

### ARM SBC Cluster vs Dedicated GPU Server

| Factor | ARM SBC Cluster | GPU Server |
|--------|----------------|------------|
| **Cost** | $860-1,900 | $4,000-10,000+ |
| **Power** | 48-120W | 300-400W |
| **Speed (70B)** | ~1-3 tok/s | ~100+ tok/s |
| **Speed (14B)** | ~4-10 tok/s distributed | ~50+ tok/s |
| **Maintenance** | Multiple devices | Single device |
| **Noise** | Silent (fanless) | Server fans |
| **Portability** | Highly portable | Rack mount |
| **Scalability** | Add more nodes | Buy more GPUs |
| **Reliability** | Node failure = partial degradation | Single point of failure |

### ARM SBC Cluster vs Windows PC Cluster

| Factor | ARM SBCs | Windows PCs |
|--------|----------|-------------|
| **Cost/GB** | ~$5-6/GB (OPi 5 Plus) | ~$8-15/GB (used DDR4 system) |
| **Per-core speed** | Slower (A76 < i5) | Faster (AVX2) |
| **Power/GB** | ~0.5W/GB | ~2-3W/GB |
| **Size** | Credit card per node | Full PC per node |
| **llama.cpp support** | NEON (good) | AVX2 (better) |
| **Network** | 2.5GbE available | 1GbE typical |
| **Ease of setup** | Linux native | Need Linux or WSL |

---

## Network Architecture Recommendation

For distributed llama.cpp RPC:
1. **Wired Ethernet is mandatory** — WiFi adds 5-10x latency penalty
2. **2.5GbE preferred** — Orange Pi 5 Plus has dual 2.5GbE natively
3. **Direct connections for 2-node** — No switch needed, just a crossover cable
4. **Flat network topology** — All nodes on same subnet, <1ms latency
5. **llama.cpp RPC max 16 workers** — Not a problem for 4-8 node clusters

### Recommended Network Setup (4x OPi 5 Plus)
```
[OPi 5 Plus #1 - Master]---2.5GbE---[8-port 2.5GbE Switch]---2.5GbE---[OPi 5 Plus #2 - Worker]
                                              |                   |
                                     [OPi 5 Plus #3]    [OPi 5 Plus #4]
```
- Switch: ~$40-60 for 8-port 2.5GbE (TP-Link TL-SG108-M2 or similar)

---

## Best Value Recommendation

### Winner: Config 1 — 4x Orange Pi 5 Plus 32GB

**Total: ~$860-$1,000 including all accessories**

This is **75-90% cheaper** than a used A100 80GB GPU ($4,000-9,000).

**Why this wins**:
1. **Best cost/GB**: ~$5.50-6.25/GB vs $30-40/GB for tablets
2. **RK3588 has 4x Cortex-A76 @ 2.4GHz** — strongest ARM cores available in SBC form factor
3. **Dual 2.5 Gigabit Ethernet** — critical for distributed inference, no USB adapter needed
4. **Native Linux** — full Debian/Ubuntu, compile llama.cpp directly, no Termux hacks
5. **32GB per board** — only 4 nodes needed (less network overhead than 8-16 nodes)
6. **PCIe expansion** — future option for NVMe or even eGPU acceleration
7. **~48-60W total power** — runs on a single 100W USB-C charger with splitter, or individual 5V/4A adapters
8. **Fanless/silent** — with heatsink cases
9. **Llamaste compatible** — can compile and run Llamaste ARM64 binary, or run llama-rpc-server as worker

**Expected performance**:
- 14B Q4_K_M model: ~4-8 tok/s across 4 nodes (model fits in ~2 nodes, others share KV cache)
- 70B Q4_K_M model (~40GB): ~1-3 tok/s (network-bound, all nodes active)
- 7B model (single node): ~2-4 tok/s per node

**What you'd need to buy**:
| Item | Qty | Unit Price | Total |
|------|-----|------------|-------|
| Orange Pi 5 Plus 32GB | 4 | $180 | $720 |
| 64GB microSD card (Samsung EVO) | 4 | $10 | $40 |
| 5V/4A USB-C power supply | 4 | $12 | $48 |
| Heatsink case (aluminum) | 4 | $15 | $60 |
| TP-Link TL-SG108-M2 2.5GbE switch | 1 | $50 | $50 |
| Cat6 Ethernet cables (1m) | 4 | $3 | $12 |
| | | | **$930** |

### Runner Up: Config 2 — Mixed Orange Pi Fleet ($930)
Same price, 6 nodes instead of 4. More network overhead but uses cheaper 16GB boards.

### Honorable Mention: Config 7 — Bulk Chromebooks ($1,480)
If you can find bulk lots of 8GB Chromebooks at ~$60-70/each, x86 AVX2 may outperform ARM NEON. But 16 nodes is too many for efficient distributed inference.

---

## Important Caveats

1. **Distributed inference is NOT linear scaling** — 4 nodes does NOT give 4x speed. Network overhead means diminishing returns. Expect 1.5-2.5x speedup from 4 nodes vs 1 node for a model that fits in 1 node's RAM.

2. **Large models (70B+) that MUST be distributed will be slow** — The model must be split across nodes. Each forward pass requires sequential tensor transfers. At 2.5GbE (~300MB/s effective), transferring tensors dominates inference time.

3. **This is NOT a GPU replacement for speed** — A single A100 will be 30-100x faster. The value proposition is:
   - 75-90% cheaper
   - 5-8x less power
   - Silent operation
   - Distributed resilience
   - Educational/hobbyist use case
   - "Good enough" for casual/personal assistant use at 3-8 tok/s

4. **Android tablets are poor value** — They cost 5-7x more per GB than SBCs, can only run via Termux, are WiFi-only, and Android may kill background inference processes.

5. **The 2026 LPDDR4 price crisis** — RAM prices have increased significantly in 2026 due to AI infrastructure demand. Raspberry Pi 5 16GB went from $120 to $205. Orange Pi boards have also seen modest increases.

6. **Llamaste mesh clustering already supports this** — The existing multi-node RPC architecture in Llamaste (tested with 2 VMs) can orchestrate ARM worker nodes. Each OPi 5 Plus would run `llama-rpc-server` and register with the master node.

---

## Future Considerations

- **RISC-V SBCs with vector extensions** — Emerging boards may offer better inference/$ in 2027+
- **RK3588 NPU (6 TOPS)** — Currently underutilized by llama.cpp. If rknn-llm matures, could add ~50% speedup
- **USB4/Thunderbolt ARM boards** — Would dramatically improve node-to-node bandwidth
- **Shared memory clustering** — CXL or similar interconnects could eliminate network overhead entirely (future tech)
- **Graviton-class ARM** — AWS Graviton4 shows ARM can match x86 for inference; consumer SBCs will catch up

---

## Sources

- [Orange Pi 5 Plus 32GB specs](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-5-plus-32GB.html)
- [Khadas Edge2 Pro](https://www.khadas.com/edge2)
- [Raspberry Pi 5 pricing](https://www.raspberrypi.com/news/more-memory-driven-price-rises/)
- [Swappa Galaxy Tab S8 ($298)](https://swappa.com/buy/samsung-galaxy-tab-s8)
- [Swappa Galaxy Tab S8 Ultra ($400+)](https://swappa.com/buy/samsung-galaxy-tab-s8-ultra)
- [LLM Inference on SBCs (arXiv 2511.07425)](https://arxiv.org/html/2511.07425v1) — Benchmark paper: RPi 4/5, OPi 5 Pro
- [llama.cpp RPC distributed inference](https://github.com/ggml-org/llama.cpp/blob/master/tools/rpc/README.md)
- [llama.cpp ARM Graviton4 distributed guide](https://learn.arm.com/learning-paths/servers-and-cloud-computing/distributed-inference-with-llama-cpp/)
- [llama.cpp on Android (Termux)](https://github.com/ggml-org/llama.cpp/blob/master/docs/android.md)
- [RK3588 LLM benchmarks](https://github.com/ggml-org/llama.cpp/issues/722)
- [NVIDIA A100 pricing guide](https://jarvislabs.ai/ai-faqs/nvidia-a100-gpu-price)
- [Framework cluster 4-node RPC test](https://community.frame.work/t/building-a-two-node-amd-strix-halo-cluster-for-llms-with-llama-cpp-rpc-minimax-m2-glm-4-6/77583)
- [Radxa ROCK 5T](https://liliputing.com/rock-5t-is-a-rk3588-single-board-pc-with-up-to-32gb-ram-two-m-2-sockets-and-plenty-of-i-o/)
- [Snapdragon llama.cpp Android benchmarks](https://arxiv.org/html/2410.03613v1)
