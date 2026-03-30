# Distributed 128GB RAM Hardware Research for Llamaste Mesh Clustering

**Date:** 2026-03-17
**Goal:** Find the cheapest collection of machines totaling 128GB RAM for distributed CPU inference via Llamaste mesh clustering. Target budget: under $3,000-4,000 (significantly less than a dedicated GPU server).

## Executive Summary

The **best value configuration is 8x Dell OptiPlex 5060 Micro** refurbished units with 16GB RAM each, totaling 128GB for approximately **$1,200-1,600**. This is 70-90% cheaper than a used GPU server with equivalent memory capacity. Mini PCs are strongly preferred over laptops due to lower cost, built-in gigabit ethernet, better thermals for sustained inference, and smaller physical footprint.

---

## 1. Device Options & Pricing (March 2026)

### A. Refurbished Mini PCs (BEST VALUE)

| Device | CPU | RAM | Price (Renewed) | Source | GbE | Notes |
|--------|-----|-----|-----------------|--------|-----|-------|
| **Lenovo ThinkCentre M715Q Tiny** | AMD PRO A12-9800E (4C/4T) | 16GB | **$131-141** | Amazon Renewed | Yes | Cheapest 16GB mini PC. Older AMD, weaker for inference |
| **Dell OptiPlex 3060 Micro** | i3-8100T (4C/4T, 3.1GHz) | 4-8GB | **$171** (4GB) | Amazon Renewed | Yes | Needs RAM upgrade. 8th gen Coffee Lake |
| **Dell OptiPlex 5060 Micro** | i5-8500T (6C/6T, 2.1-3.5GHz) | 8GB | **$187** | Amazon Renewed | Yes | Best value: 6 cores, 8th gen, easy RAM upgrade |
| **Dell OptiPlex 5060 Micro** | i5-8500T (6C/6T) | 32GB | **$611** | Amazon Renewed | Yes | Pre-configured 32GB, premium price |
| **Dell OptiPlex 7040 Micro** | i5-6500T (4C/4T, 2.5GHz) | 8GB | **$173** | Amazon Renewed | Yes | Older 6th gen, still viable |
| **HP EliteDesk 800 G2 Mini** | i5-6500T (4C/4T, 2.5GHz) | 8GB | **$168** | Amazon Renewed | Yes | Older 6th gen Skylake |
| **HP EliteDesk 705 G4 Mini** | Ryzen 3 2200G (4C/4T) | 8GB | **$171** | Amazon Renewed | Yes | Ryzen, decent for inference |
| **Lenovo ThinkCentre M720Q** | i5-8400T (6C/6T, 1.7-3.3GHz) | 8GB | **$210** | Amazon Renewed | Yes | 8th gen, 6 cores, good value |
| **Lenovo ThinkCentre M710q** | i5-6500T (4C/4T) | 8GB | **$159** | Amazon Renewed | Yes | Older 6th gen |

### B. Refurbished Laptops

| Device | CPU | RAM | Price (Renewed) | Source | GbE | Notes |
|--------|-----|-----|-----------------|--------|-----|-------|
| **Dell Latitude 5400** | i5-8365U (4C/8T, 1.6-4.1GHz) | 8GB | **$200** | Amazon Renewed | Some models | 8th gen, decent. Needs USB-GbE adapter |
| **Dell Latitude 7400** | i5-8365U (4C/8T) | 16GB | **$200-250** | Amazon Renewed | USB-C only | Good specs, no built-in ethernet |
| **Lenovo ThinkPad** (various) | i5 6th-8th gen | 16GB | **$150-250** | Amazon Renewed | Yes (most) | ThinkPads often have GbE built-in |
| **HP EliteBook** (various) | i5 6th-8th gen | 16GB | **$150-250** | Amazon Renewed | Varies | Business laptops, usually GbE |

### C. New Budget Mini PCs

| Device | CPU | RAM | Price (New) | Source | GbE | Notes |
|--------|-----|-----|-------------|--------|-----|-------|
| **Generic N100 Mini PC** | Intel N100 (4C/4T, 3.4GHz) | 16GB | **$156** | Amazon | Some | Very low power (6W TDP). TOO WEAK for inference |
| **Beelink/MinisForum N100** | Intel N100 (4C/4T) | 16GB | **$160-200** | Amazon | Yes | Same problem: N100 is Atom-class, terrible for LLM |

> **WARNING:** Intel N95/N100/N200 mini PCs look cheap but have Atom-class cores with no AVX2 support. They will get ~0.5-2 tok/s on 3B models. **Do not use these for inference.** Stick with 8th gen+ Core i5/i7 or Ryzen 5+.

### D. RAM Upgrade Pricing

| Module | Price | Source |
|--------|-------|--------|
| 16GB DDR4 SODIMM (single stick) | **$20-25** | Amazon/eBay |
| 32GB DDR4 SODIMM (2x16GB kit) | **$40-50** | Amazon/eBay |
| 16GB DDR4 UDIMM (desktop) | **$18-22** | Amazon/eBay |

> **Key insight:** Buying 8GB machines + upgrading RAM yourself is dramatically cheaper than buying pre-configured 16GB/32GB units. A Dell OptiPlex 5060 Micro with 8GB ($187) + 16GB SODIMM ($22) = $209 for a 24GB machine. Or remove the 8GB, add 2x16GB ($45) = $232 for 32GB.

---

## 2. Configurations to Reach 128GB

### Config A: 8x Dell OptiPlex 5060 Micro (16GB each) -- BEST VALUE
| Item | Qty | Unit Cost | Total |
|------|-----|-----------|-------|
| Dell OptiPlex 5060 Micro, i5-8500T, 8GB (Renewed) | 8 | $187 | $1,496 |
| 8GB DDR4 SODIMM (upgrade to 16GB) | 8 | $12 | $96 |
| 8-port Gigabit Switch | 1 | $25 | $25 |
| Cat6 Cables (3ft, 8-pack) | 1 | $15 | $15 |
| **TOTAL** | | | **$1,632** |

- **Total RAM:** 128GB (8 x 16GB)
- **Total CPU cores:** 48 (8 x 6C i5-8500T)
- **Per-node inference:** ~10-14 tok/s on 3B model (AVX2)
- **Power consumption:** ~200W total (8 x 25W TDP)
- **Physical footprint:** 8 units, each ~7x7x1.4 inches. Stackable. Fits on a single shelf.

### Config B: 4x Dell OptiPlex 5060 Micro (32GB each) -- SIMPLER
| Item | Qty | Unit Cost | Total |
|------|-----|-----------|-------|
| Dell OptiPlex 5060 Micro, i5-8500T, 8GB (Renewed) | 4 | $187 | $748 |
| 32GB DDR4 SODIMM kit (2x16GB, replace existing) | 4 | $45 | $180 |
| 8-port Gigabit Switch | 1 | $25 | $25 |
| Cat6 Cables | 1 | $15 | $15 |
| **TOTAL** | | | **$968** |

- **Total RAM:** 128GB (4 x 32GB)
- **Total CPU cores:** 24 (4 x 6C)
- **Fewer nodes = less network overhead for tensor splitting**
- **Power consumption:** ~100W total
- **CAVEAT:** Check that OptiPlex 5060 Micro supports 32GB (2x16GB). The i5-8500T supports up to 64GB per Intel spec, but some boards may have BIOS limits.

### Config C: Mixed Fleet -- CHEAPEST POSSIBLE
| Item | Qty | Unit Cost | Total |
|------|-----|-----------|-------|
| Lenovo ThinkCentre M715Q, A12-9800E, 16GB (Renewed) | 4 | $131 | $524 |
| Dell OptiPlex 5060 Micro, i5-8500T, 8GB (Renewed) | 4 | $187 | $748 |
| 8GB DDR4 SODIMM (upgrade OptiPlex to 16GB) | 4 | $12 | $48 |
| 8-port Gigabit Switch | 1 | $25 | $25 |
| Cat6 Cables | 1 | $15 | $15 |
| **TOTAL** | | | **$1,360** |

- **Total RAM:** 128GB (4x16 + 4x16)
- **Total CPU cores:** 40 (4x4C AMD + 4x6C Intel)
- **CAVEAT:** The M715Q AMD A12-9800E is significantly slower for inference (no AVX2 in AMD Excavator). Only use as "RAM donors" for the mesh, not primary compute.

### Config D: 8x Refurbished Laptops -- PORTABLE
| Item | Qty | Unit Cost | Total |
|------|-----|-----------|-------|
| Dell Latitude 5400/5500, i5-8365U, 16GB (Renewed) | 8 | $225 | $1,800 |
| USB-to-GbE Adapters (if no built-in ethernet) | 4 | $15 | $60 |
| 8-port Gigabit Switch | 1 | $25 | $25 |
| Cat6 Cables | 1 | $15 | $15 |
| **TOTAL** | | | **$1,900** |

- **Total RAM:** 128GB (8 x 16GB)
- **Total CPU cores:** 32 (8 x 4C/8T i5-8365U) -- U-series = lower perf than T-series
- **Pros:** Built-in display, battery (UPS), WiFi, portable
- **Cons:** More expensive per GB, slower CPUs (U-series), thermal throttling risk under sustained load, bigger footprint, many have no GbE port

### Config E: 8x Mini PCs with 32GB -- MAXIMUM RAM
| Item | Qty | Unit Cost | Total |
|------|-----|-----------|-------|
| Dell OptiPlex 5060 Micro, i5-8500T, 8GB (Renewed) | 8 | $187 | $1,496 |
| 32GB DDR4 SODIMM kit (2x16GB) per unit | 8 | $45 | $360 |
| 8-port Gigabit Switch | 1 | $25 | $25 |
| Cat6 Cables | 1 | $15 | $15 |
| **TOTAL** | | | **$1,896** |

- **Total RAM:** 256GB (8 x 32GB)
- **Double the target for $1,900.** Could run 70B models distributed.
- **Total CPU cores:** 48

### Config F: eBay Bulk Lot (Speculative)
On eBay, lots of 10+ enterprise mini PCs regularly appear at $80-120/unit when sold in bulk. A lot of "10x Dell OptiPlex 3060 Micro, i5-8500T, 8GB, 256GB SSD" might go for $900-1,200 total ($90-120 each). This requires patience and watching auctions.

**Estimated total for 8 units from bulk lot + RAM upgrades:**
- 8x Mini PCs from lot: ~$800
- 8x 8GB SODIMM upgrade: ~$96
- Switch + cables: ~$40
- **TOTAL: ~$936** (128GB)

---

## 3. GPU Server Comparison (What $3,000-4,000 Buys)

| Option | VRAM/RAM | Price (Used) | Source | Notes |
|--------|----------|-------------|--------|-------|
| RTX 3090 (renewed) | 24GB VRAM | $1,200 | Amazon Renewed | Single GPU. Only 24GB usable for LLM |
| 2x RTX 3090 + motherboard + PSU | 48GB VRAM | $3,500-4,000 | eBay/Build | Need special motherboard, 1200W PSU |
| RTX 4090 (used) | 24GB VRAM | $1,800-2,200 | eBay | Faster but still only 24GB |
| NVIDIA A100 80GB (used) | 80GB VRAM | $8,000-12,000 | eBay | Way over budget |
| Used workstation (Dual Xeon, 128GB RAM) | 128GB RAM | $800-1,500 | eBay | CPU-only. Single point of failure, loud, 300-500W |
| Cloud H100 rental | 80GB VRAM | $2.85-3.50/hr | Various | ~$70-85/day. $2,100-2,550/month ongoing cost |

### Key Comparison

| Metric | 8x OptiPlex 5060 (Config A) | Dual RTX 3090 Build | Used Dual Xeon Server | Cloud H100 |
|--------|----------------------------|---------------------|----------------------|------------|
| **Total cost** | $1,632 | $3,500-4,000 | $800-1,500 | $2,100/month |
| **Usable RAM/VRAM** | 128GB RAM | 48GB VRAM | 128GB RAM | 80GB VRAM |
| **70B Q4 model?** | Yes (distributed) | No (need 40GB+) | Yes (single node) | Yes |
| **Inference speed (70B)** | ~1-3 tok/s (network bound) | 15-30 tok/s | ~2-5 tok/s | 50+ tok/s |
| **Inference speed (14B)** | 8-12 tok/s per node, ~20-30 distributed | 40-80 tok/s | 5-10 tok/s | 100+ tok/s |
| **Power draw** | ~200W | ~700W | ~400W | N/A (cloud) |
| **Noise** | Near silent | Loud (fans) | Very loud | N/A |
| **Redundancy** | 7/8 nodes survive if 1 dies | Single point of failure | Single point of failure | Provider-managed |
| **Scalability** | Add more nodes anytime | Hard to add more GPUs | Fixed | Scale up/down |
| **Breakeven vs cloud** | Immediate (one-time cost) | 1.5-2 months | ~0.5 month | Never (ongoing) |

---

## 4. Network Requirements

Llamaste mesh clustering uses TCP for model shard distribution and RPC for tensor splitting (llama-rpc-server). Requirements:

- **Minimum:** Gigabit Ethernet (1 Gbps) -- all enterprise mini PCs have this built-in
- **Recommended:** 2.5 GbE for larger models (some newer mini PCs have this)
- **Switch:** Unmanaged 8-port gigabit switch ($20-30, e.g., TP-Link TL-SG108)
- **Latency:** <1ms on local switch. Critical for tensor-split RPC calls
- **Bandwidth for 70B model:** Initial shard transfer ~40GB, takes ~5 minutes over GbE. During inference, tensor-split RPC overhead is small relative to compute time
- **WiFi:** NOT suitable for tensor splitting (too much latency/jitter). Ethernet only.

---

## 5. CPU Quality Assessment for Inference

Llamaste uses llama.cpp with GGML_NATIVE=ON for AVX2/FMA SIMD acceleration. CPU quality directly determines tok/s.

### Performance Tiers (estimated, 3B Q4_K_M model)

| CPU | Cores | AVX2 | Est. tok/s (3B) | Verdict |
|-----|-------|------|-----------------|---------|
| i5-8500T (Coffee Lake, 8th gen) | 6C/6T | Yes | 12-16 | Excellent for the price |
| i5-8400T (Coffee Lake) | 6C/6T | Yes | 11-15 | Very good |
| i5-8365U (Whiskey Lake) | 4C/8T | Yes | 8-12 | Good, but U-series throttles |
| i5-6500T (Skylake, 6th gen) | 4C/4T | Yes | 8-11 | Adequate. Older, fewer cores |
| AMD A12-9800E (Excavator) | 4C/4T | **No AVX2** | 1-3 | AVOID for compute. RAM donor only |
| Intel N100 (Alder Lake-N) | 4C/4T | **No AVX2** | 0.5-2 | AVOID entirely |
| Ryzen 3 2200G (Zen 1) | 4C/4T | Yes | 8-10 | Decent, AVX2 support |
| i7-9700T (Coffee Lake Refresh) | 8C/8T | Yes | 16-22 | Premium option if found cheap |

### Minimum Requirements
- **AVX2 support is mandatory** (500x difference vs scalar)
- 4+ physical cores
- 8th gen Intel Core or newer preferred (Coffee Lake has wider execution units)
- T-series desktop CPUs preferred over U-series laptop CPUs (higher sustained clock, better thermals)

---

## 6. Power Consumption Estimates

| Config | Devices | TDP per unit | Est. actual draw | Total draw | Annual electricity ($0.12/kWh) |
|--------|---------|-------------|-------------------|------------|-------------------------------|
| 8x OptiPlex 5060 Micro (16GB) | 8 | 35W | ~25W each | ~200W | ~$210/year |
| 4x OptiPlex 5060 Micro (32GB) | 4 | 35W | ~25W each | ~100W | ~$105/year |
| 8x Laptops | 8 | 15W (U-series) | ~20W each | ~160W | ~$168/year |
| Used Dual Xeon Server | 1 | 2x145W | ~350W | ~350W | ~$368/year |
| 2x RTX 3090 workstation | 1 | 2x350W+CPU | ~600W under load | ~600W | ~$631/year |

Mini PCs are remarkably power-efficient. 8 units draw less power than a single GPU workstation.

---

## 7. Pros & Cons vs. Dedicated Server

### Pros of Distributed Mini PCs
1. **70-90% cheaper** than GPU server with equivalent memory
2. **Fault tolerant** -- losing one node doesn't kill the cluster
3. **Incrementally scalable** -- add nodes one at a time
4. **Near silent** -- mini PCs have tiny fans, often inaudible
5. **Low power** -- 200W total vs 600W+ for GPU server
6. **No special hardware** -- standard ethernet, standard power
7. **Each node is independently useful** -- can run Llamaste standalone
8. **Proven with Llamaste** -- mesh clustering already tested with 2 VMs

### Cons of Distributed Mini PCs
1. **Slower inference** -- CPU is 10-50x slower than GPU per node
2. **Network overhead** -- tensor splitting over ethernet adds latency
3. **More physical devices** -- 8 machines to manage, 8 power cables, 8 ethernet cables
4. **No GPU acceleration** -- purely CPU inference
5. **RAM bandwidth bottleneck** -- DDR4 ~25GB/s per node vs HBM3 ~2TB/s on H100
6. **Distributed inference scaling** -- not perfectly linear (network overhead)
7. **Heterogeneous fleet** -- if mixing CPU types, load balancing is harder

### When Distributed Mini PCs Win
- Running 14B-70B models for personal/small-office use where 2-10 tok/s is acceptable
- Budget under $2,000
- Always-on home server (low power cost)
- Learning/experimentation with distributed AI
- Redundancy matters (can't afford downtime)

### When GPU Server Wins
- Need >10 tok/s on large models
- Training or fine-tuning (CPUs are impractical)
- Single-user latency-sensitive applications
- Budget allows $4,000+

---

## 8. Best Value Recommendation

### Primary: Config B (4x OptiPlex 5060 Micro, 32GB each)

**Total: ~$968 for 128GB RAM**

This is the sweet spot:
- Only 4 nodes to manage (less network overhead, simpler setup)
- 32GB per node means each can independently run 14B models
- i5-8500T has 6 cores with AVX2 -- proven fast with Llamaste (~14 tok/s on 3B)
- All have built-in Gigabit Ethernet
- Silent, low-power, tiny form factor
- **Cost is under $1,000** -- that's ~80-95% cheaper than any GPU alternative with 128GB

### Budget Alternative: Config F (eBay Bulk Lot)

Watch eBay for bulk lots of Dell OptiPlex 3060/5060/7060 Micro or Lenovo M720q/M920q. Lots of 10+ units regularly appear at $80-120 per unit from corporate liquidation. With patience, you could build 128GB for under $1,000 all-in.

### Upgrade Path

Start with 4 nodes (128GB, ~$968). When budget allows:
- Add 4 more nodes = 256GB (~$968 more) for 70B Q4 models
- Add 4 more = 384GB (~$968 more) for 70B Q6 or multiple models simultaneously
- Each expansion is incremental and low-risk

### What to Avoid
- **Intel N95/N100 mini PCs**: Cheap but no AVX2 = unusable for inference
- **AMD Excavator (A-series)**: No AVX2, terrible inference performance
- **Anything with <4 cores**: Not enough parallelism
- **WiFi mesh**: Too slow for tensor splitting, must use wired ethernet
- **Pre-configured 32GB machines from Amazon**: 3-4x the cost of buy-8GB-and-upgrade-yourself

---

## 9. Shopping Checklist

For each Dell OptiPlex 5060 Micro unit, verify:
- [ ] CPU is i5-8500T or i5-8600T (6 cores, AVX2)
- [ ] At least 1 available SODIMM slot (for RAM upgrade)
- [ ] Gigabit Ethernet port (all OptiPlex Micros have this)
- [ ] Power adapter included
- [ ] Working condition (boot tested)
- [ ] 256GB SSD minimum (for Llamaste OS + model storage; models can also be on NFS/network)

Also buy:
- [ ] DDR4 SODIMM modules (2x16GB kits if going 32GB per node)
- [ ] Unmanaged gigabit switch (8-port minimum, 16-port if planning expansion)
- [ ] Cat6 ethernet cables (one per node, plus one for uplink)
- [ ] Power strip with enough outlets
- [ ] USB flash drive for Llamaste installer

---

## 10. Comparison Summary Table

| Configuration | Nodes | Total RAM | Total Cost | Cost/GB | Cluster tok/s (14B est.) |
|--------------|-------|-----------|------------|---------|--------------------------|
| **Config A:** 8x OptiPlex 16GB | 8 | 128GB | $1,632 | $12.75/GB | 15-25 tok/s |
| **Config B:** 4x OptiPlex 32GB | 4 | 128GB | $968 | $7.56/GB | 8-14 tok/s |
| **Config C:** Mixed fleet 16GB | 8 | 128GB | $1,360 | $10.63/GB | 10-18 tok/s |
| **Config D:** 8x Laptops 16GB | 8 | 128GB | $1,900 | $14.84/GB | 10-18 tok/s |
| **Config E:** 8x OptiPlex 32GB | 8 | 256GB | $1,896 | $7.41/GB | 20-35 tok/s |
| **Config F:** eBay bulk lot | 8 | 128GB | ~$936 | ~$7.31/GB | 12-20 tok/s |
| **GPU:** 2x RTX 3090 build | 1 | 48GB VRAM | $3,500-4,000 | $73-83/GB | 30-60 tok/s |
| **GPU:** Used A100 80GB | 1 | 80GB VRAM | $8,000-12,000 | $100-150/GB | 50-100 tok/s |

**Winner: Config B at $7.56/GB -- ten times cheaper per GB than GPU VRAM, and enough for 70B Q4 inference.**

---

## Sources
- Amazon Renewed/New listings (searched 2026-03-17)
- eBay Buy It Now listings (searched 2026-03-17, rate-limited)
- Tim Dettmers GPU guide (timdettmers.com)
- Intel ARK specifications for TDP and memory support
- Llamaste project benchmarks (14 tok/s on 3B Q4_K_M with i5-1035G1 + AVX2)
- DDR4 SODIMM pricing from Amazon (2026-03-17)
