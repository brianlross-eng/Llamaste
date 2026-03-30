# 128GB+ VRAM Server Research — Used/Refurbished Options

**Date**: 2026-03-17
**Purpose**: Find the cheapest way to assemble ~128GB of GPU VRAM for local LLM inference
**Sources**: Newegg, eBay, Amazon, getdeploying.com, Tim Dettmers GPU guide, r/LocalLLaMA

---

## Executive Summary

The cheapest path to 128GB+ VRAM is **6x Tesla P40 24GB** (~$2,100 for GPUs alone) in a used Dell/Supermicro multi-GPU server chassis. The best *value* option balancing price, performance, and power is **3x RTX A6000 48GB** (144GB, ~$10,000-$12,000) or **4x RTX 3090 24GB** (96GB, ~$3,200) if 96GB suffices. For maximum performance, **2x A100 80GB** (160GB, ~$12,000-$16,000 used) with NVLink is hard to beat.

---

## GPU Configuration Options (Cheapest to Most Expensive)

### 1. 6x Tesla P40 24GB = 144GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 144 GB (6 x 24GB GDDR5) |
| **GPU Cost (used)** | ~$200-350 each = **$1,200-$2,100 total** |
| **Architecture** | Pascal (2016) |
| **FP16** | No native FP16 (must use FP32) |
| **TDP per GPU** | 250W |
| **Total GPU Power** | 1,500W |
| **Memory Bandwidth** | 346 GB/s per GPU |
| **PCIe** | 3.0 x16 |
| **Where to Buy** | Newegg ($353-$988), eBay ($150-$300 used) |

**Server chassis needed**: Used Dell PowerEdge R740 or Supermicro 4U with 6+ PCIe slots. Chassis: $500-$1,500 used on eBay.

**Pros**:
- Absolute cheapest path to 128GB+ VRAM
- Massive supply on secondary market (ex-datacenter)
- No video output needed (headless compute card)

**Cons**:
- Pascal architecture: no FP16, no INT8 tensor cores — very slow inference per VRAM-GB
- GDDR5 (346 GB/s) — bottleneck for LLM inference which is memory-bandwidth-bound
- 1,500W GPU power alone — needs 2,000W+ PSU and serious cooling
- PCIe 3.0 only — cross-GPU communication slower
- No NVLink support
- Noisy datacenter fans

**Total System Cost**: ~$2,000-$4,000

---

### 2. 6x RTX 3090 24GB = 144GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 144 GB (6 x 24GB GDDR6X) |
| **GPU Cost (used)** | ~$550-$800 each = **$3,300-$4,800 total** |
| **Architecture** | Ampere (2020) |
| **FP16** | Yes (native + tensor cores) |
| **TDP per GPU** | 350W |
| **Total GPU Power** | 2,100W |
| **Memory Bandwidth** | 936 GB/s per GPU |
| **PCIe** | 4.0 x16 |
| **Where to Buy** | Newegg ($789-$1,775 new/refurb), eBay ($550-$750 used) |

**Server/Workstation needed**: Custom open-frame rig or mining frame. Standard ATX only fits 2-3 triple-slot cards. Need server board (ASRock Rack, Supermicro) with 6+ x16 slots, or multiple risers.

**Pros**:
- Excellent price/performance ratio for inference
- GDDR6X at 936 GB/s — 2.7x the bandwidth of P40
- Ampere tensor cores (FP16, INT8) dramatically faster than Pascal
- Huge used market (ex-mining, ex-gaming)
- llama.cpp works great with multi-GPU RTX 3090 setups

**Cons**:
- 2,100W GPU power — needs 2x 1600W PSUs or single 3000W
- Triple-slot coolers — physical fit is the #1 challenge
- Consumer cards: no ECC, may not be as reliable 24/7
- No NVLink (SLI bridge ≠ NVLink for compute)
- Loud under load (blower cards quieter but throttle)

**Total System Cost**: ~$4,500-$7,000 (GPUs + motherboard + dual PSU + frame)

---

### 3. 8x Tesla P40 24GB = 192GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 192 GB (8 x 24GB GDDR5) |
| **GPU Cost (used)** | ~$200-350 each = **$1,600-$2,800 total** |
| **Architecture** | Pascal (2016) |
| **TDP per GPU** | 250W |
| **Total GPU Power** | 2,000W |
| **Where to Buy** | Newegg, eBay |

**Server chassis needed**: Dell PowerEdge C4130/C4140, Supermicro 4028GR, or similar 8-GPU chassis. $1,000-$3,000 used.

**Pros**: Maximum VRAM at minimum cost. 192GB can run 70B Q4 models.
**Cons**: Same Pascal limitations as option 1, plus 2,000W GPU power draw. 8-GPU servers are loud and heavy.

**Total System Cost**: ~$3,000-$6,000

---

### 4. 4x RTX 3090 24GB = 96GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 96 GB (4 x 24GB GDDR6X) |
| **GPU Cost (used)** | ~$550-$800 each = **$2,200-$3,200 total** |
| **Architecture** | Ampere (2020) |
| **TDP per GPU** | 350W |
| **Total GPU Power** | 1,400W |
| **Where to Buy** | Newegg, eBay, Amazon |

**Note**: 96GB is under the 128GB target but sufficient for most 70B Q4_K_M models (requires ~42GB) and some Q5/Q6 quants.

**Pros**: Best bang-for-buck if 96GB is enough. Only needs 1x 1600W PSU. Fits in a large ATX case with risers.
**Cons**: Under 128GB target. Still tight on physical space.

**Total System Cost**: ~$3,500-$5,000

---

### 5. 3x NVIDIA A40 48GB = 144GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 144 GB (3 x 48GB GDDR6) |
| **GPU Cost (used/refurb)** | ~$3,500-$4,500 each = **$10,500-$13,500 total** |
| **Architecture** | Ampere (2021) |
| **FP16** | Yes (tensor cores, TF32, BF16, INT8) |
| **TDP per GPU** | 300W |
| **Total GPU Power** | 900W |
| **Memory Bandwidth** | 696 GB/s per GPU |
| **PCIe** | 4.0 x16 |
| **Where to Buy** | Newegg (Dell refurb $6,950, new $11,837), eBay ($3,500-$4,500 used) |

**Pros**:
- Only 3 GPUs needed → simpler chassis, fewer PCIe slots
- Passive cooling (datacenter design) — works great in rack servers
- ECC memory — reliable 24/7 operation
- 900W total GPU power — manageable with single 1600W PSU
- Dual-slot form factor — easy physical fit

**Cons**:
- Expensive per GPU
- GDDR6 (not HBM) — 696 GB/s is less than A100's 2 TB/s
- No NVLink (A40 doesn't support it)

**Total System Cost**: ~$12,000-$16,000

---

### 6. 3x RTX A6000 48GB = 144GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 144 GB (3 x 48GB GDDR6) |
| **GPU Cost (used/refurb)** | ~$3,300-$4,500 each = **$10,000-$13,500 total** |
| **Architecture** | Ampere (2020) |
| **FP16** | Yes (tensor cores) |
| **TDP per GPU** | 300W |
| **Total GPU Power** | 900W |
| **Memory Bandwidth** | 768 GB/s per GPU |
| **PCIe** | 4.0 x16 |
| **Where to Buy** | Newegg ($6,300-$6,900), eBay ($3,300-$4,000 used) |

**Pros**:
- NVLink bridge support (2-way) — faster cross-GPU communication
- Active cooling with blower fan — works in workstations (not just servers)
- Display outputs — can double as workstation
- Slightly higher bandwidth than A40 (768 vs 696 GB/s)
- 48GB per card: only 3 needed for 144GB

**Cons**:
- Similar price to A40 but with active cooler (louder)
- Pro card pricing premium

**Total System Cost**: ~$11,500-$16,000

---

### 7. 2x NVIDIA A100 80GB PCIe = 160GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 160 GB (2 x 80GB HBM2e) |
| **GPU Cost (used/refurb)** | ~$6,000-$8,000 each = **$12,000-$16,000 total** |
| **Architecture** | Ampere (2020) |
| **FP16/BF16** | Yes (3rd-gen tensor cores) |
| **TDP per GPU** | 300W (PCIe) / 400W (SXM4) |
| **Total GPU Power** | 600-800W |
| **Memory Bandwidth** | 2,039 GB/s per GPU (HBM2e) |
| **PCIe** | 4.0 x16 |
| **Where to Buy** | Amazon ($7,950 SXM4), Newegg ($24,979 new PCIe PNY), eBay ($5,500-$8,000 used PCIe) |

**Note**: SXM4 modules ($6,000-$8,000 on Amazon/eBay) require an SXM4 baseboard (DGX/HGX), which is expensive. PCIe versions are simpler but pricier new.

**Pros**:
- Only 2 GPUs — simplest setup
- HBM2e at 2 TB/s — 2-3x bandwidth of GDDR6 cards → fastest inference per GPU
- NVLink 3.0 (600 GB/s bidirectional) — real GPU-to-GPU interconnect
- 600W total — runs on a standard workstation PSU
- ECC, datacenter-grade reliability
- Best tok/s per GPU for LLM inference (memory-bandwidth-bound)

**Cons**:
- Most expensive option per-VRAM-GB
- SXM4 requires special baseboard ($2,000-$5,000 used)
- PCIe A100 80GB new is $25,000 — used/refurb market is $6,000-$8,000 each
- Fewer GPUs means less total compute parallelism

**Total System Cost**: ~$14,000-$22,000 (PCIe in workstation) or ~$15,000-$25,000 (SXM4 with baseboard)

---

### 8. 6x RTX 4090 24GB = 144GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 144 GB (6 x 24GB GDDR6X) |
| **GPU Cost (new/used)** | ~$1,800-$2,500 each = **$10,800-$15,000 total** |
| **Architecture** | Ada Lovelace (2022) |
| **FP16** | Yes (4th-gen tensor cores) |
| **TDP per GPU** | 450W |
| **Total GPU Power** | 2,700W |
| **Memory Bandwidth** | 1,010 GB/s per GPU |
| **PCIe** | 4.0 x16 |
| **Where to Buy** | Newegg ($2,000-$3,200 refurb, $3,765+ new), eBay ($1,800-$2,200 used) |

**Pros**:
- Fastest consumer GPU — Ada Lovelace tensor cores
- 1,010 GB/s bandwidth — best of any GDDR card
- FP8 support for quantized inference
- Available new (not EOL)

**Cons**:
- 2,700W GPU power — needs 2-3x 1600W PSUs
- Triple+ slot coolers, physically massive
- Very hard to fit 6 in any standard chassis
- NVIDIA driver limits: consumer cards limited to max 2 in some enterprise configs (can be worked around in Linux)
- Price premium vs RTX 3090 for only ~8% more bandwidth

**Total System Cost**: ~$13,000-$18,000

---

### 9. 3x NVIDIA L40S 48GB = 144GB VRAM

| Spec | Value |
|------|-------|
| **Total VRAM** | 144 GB (3 x 48GB GDDR6) |
| **GPU Cost (refurb/new)** | ~$8,000-$10,000 each = **$24,000-$30,000 total** |
| **Architecture** | Ada Lovelace (2023) |
| **FP16/BF16/FP8** | Yes (4th-gen tensor cores) |
| **TDP per GPU** | 350W |
| **Total GPU Power** | 1,050W |
| **Memory Bandwidth** | 864 GB/s per GPU |
| **PCIe** | 4.0 x16 |
| **Where to Buy** | Newegg (Dell refurb $8,999, new $9,970-$12,437) |

**Pros**: Latest Ada architecture, datacenter-grade, FP8 support, passive cooling, ECC.
**Cons**: Very expensive — newest generation hasn't depreciated much yet. Better to wait 1-2 years.

**Total System Cost**: ~$26,000-$34,000

---

## Quick Comparison Table

| Config | Total VRAM | GPU Cost (used) | Total System | $/GB VRAM | Power (GPU) | Inference Speed |
|--------|-----------|----------------|-------------|-----------|-------------|----------------|
| 6x P40 24GB | 144 GB | $1,200-$2,100 | $2,000-$4,000 | **$8-$15** | 1,500W | Slowest |
| 8x P40 24GB | 192 GB | $1,600-$2,800 | $3,000-$6,000 | **$8-$15** | 2,000W | Slowest |
| 4x RTX 3090 | 96 GB | $2,200-$3,200 | $3,500-$5,000 | $23-$33 | 1,400W | Fast |
| 6x RTX 3090 | 144 GB | $3,300-$4,800 | $4,500-$7,000 | $23-$33 | 2,100W | Fast |
| 3x A40 48GB | 144 GB | $10,500-$13,500 | $12,000-$16,000 | $73-$94 | 900W | Medium |
| 3x A6000 48GB | 144 GB | $10,000-$13,500 | $11,500-$16,000 | $69-$94 | 900W | Medium |
| 2x A100 80GB | 160 GB | $12,000-$16,000 | $14,000-$22,000 | $75-$100 | 600W | **Fastest** |
| 6x RTX 4090 | 144 GB | $10,800-$15,000 | $13,000-$18,000 | $75-$104 | 2,700W | Very Fast |
| 3x L40S 48GB | 144 GB | $24,000-$30,000 | $26,000-$34,000 | $167-$208 | 1,050W | Very Fast |

---

## Power Requirements

| Config | GPU Power | System Total (est.) | PSU Needed | Monthly Electricity (24/7, $0.12/kWh) |
|--------|----------|--------------------|-----------|-----------------------------------------|
| 6x P40 | 1,500W | ~1,800W | 2x 1200W | ~$156 |
| 8x P40 | 2,000W | ~2,400W | 2x 1600W | ~$207 |
| 4x RTX 3090 | 1,400W | ~1,700W | 1x 1600W | ~$147 |
| 6x RTX 3090 | 2,100W | ~2,500W | 2x 1600W | ~$216 |
| 3x A40 | 900W | ~1,200W | 1x 1600W | ~$104 |
| 3x A6000 | 900W | ~1,200W | 1x 1600W | ~$104 |
| 2x A100 PCIe | 600W | ~900W | 1x 1200W | ~$78 |
| 6x RTX 4090 | 2,700W | ~3,200W | 2-3x 1600W | ~$276 |
| 3x L40S | 1,050W | ~1,400W | 1x 1600W | ~$121 |

**Note**: US residential circuits are typically 15A/120V (1,800W max) or 20A/120V (2,400W max). Configs drawing >2,000W likely need a 240V circuit or dedicated 30A outlet.

---

## Where to Buy

### GPUs (Individual Cards)

| Source | Best For | Notes |
|--------|---------|-------|
| **eBay** | Used P40, RTX 3090 | Best prices, buyer protection. Search "Buy It Now" + "Price: lowest first" |
| **Newegg** | Refurbished A40, A6000, P40 | "Recertified" section has good deals. Dell pulls common |
| **Amazon** | A100 SXM4 modules | A100 SXM4 80GB at ~$7,950 (third-party sellers) |
| **r/hardwareswap** | Used RTX 3090/4090 | Reddit marketplace, PayPal G&S for protection |

### Complete GPU Servers

| Source | URL | Notes |
|--------|-----|-------|
| **eBay Servers** | ebay.com → search "8 gpu server" in Servers category | Dell C4130, Supermicro 4028GR with GPUs pre-installed |
| **ServerMonkey** | servermonkey.com | Refurbished enterprise. Call for GPU server configs |
| **SaveMyServer** | savemyserver.com | Used Dell/HP enterprise servers |
| **TechMikeNY** | techmikeNY.com | Refurbished Dell PowerEdge specialists |
| **IT Creations** | itcreations.com | Used/refurb enterprise GPU servers |

### Chassis/Motherboard for DIY Multi-GPU

| Component | Options | Est. Price |
|-----------|---------|-----------|
| Motherboard | ASRock Rack ROMED8-2T (8x PCIe 4.0), Supermicro X12SPA-TF | $400-$800 |
| CPU | AMD EPYC 7302 (16c), Intel Xeon W-3245 | $200-$600 used |
| RAM | 128GB DDR4 ECC (8x16GB) | $150-$300 used |
| Case | Open-frame mining rig, Rosewill RSV-L4500U 4U | $100-$300 |
| PSU | EVGA 1600 T2, Corsair AX1600i, or HP server PSU + breakout board | $200-$400 |

---

## Recommendations

### Best Budget: 6x Tesla P40 (144GB, ~$2,500 total)
If you need maximum VRAM at minimum cost and can tolerate slow inference. P40s are $150-$350 each on eBay. Pair with a used Dell R740 ($500-$1,000). Total: ~$2,500. Inference will be slow (Pascal, no FP16, GDDR5) but the model FITS, and llama.cpp supports multi-GPU P40 setups. Good for: batch processing, experimentation, running large models where speed isn't critical.

### Best Value: 4-6x RTX 3090 (96-144GB, ~$4,000-$7,000 total)
The sweet spot. RTX 3090 used prices have crashed to $550-$750. Ampere tensor cores + 936 GB/s GDDR6X bandwidth. 4x gives 96GB (enough for most 70B Q4 models). 6x gives 144GB. Community favorite on r/LocalLLaMA. Main challenge is physical fit and power.

### Best Performance: 2x A100 80GB PCIe (160GB, ~$14,000-$20,000 total)
If budget allows. HBM2e at 2 TB/s bandwidth is unmatched — LLM inference is memory-bandwidth-bound, so A100 gives the best tok/s per GPU by far. Only 2 cards, 600W total, fits in a standard workstation. NVLink for fast cross-GPU transfers. Watch eBay for used PCIe A100 80GB at $5,500-$7,000.

### Best Balanced: 3x RTX A6000 48GB (144GB, ~$12,000 total)
If you want 48GB-per-card simplicity with NVLink support, workstation-friendly form factor, and ECC reliability. Only 3 cards at 300W each. Works in any dual-CPU workstation with 3+ PCIe 4.0 x16 slots.

---

## Llamaste-Specific Notes

For the Llamaste project (LLM-as-OS, llama.cpp inference):

1. **llama.cpp supports multi-GPU via tensor splitting** — all configs above work with `--tensor-split`
2. **Memory bandwidth is king for inference** — A100 >> RTX 4090 > RTX 3090 >> A40 >> P40 for tok/s
3. **128GB VRAM target lets you run**: Llama 3 70B Q4_K_M (~42GB), Llama 3 70B Q6_K (~55GB), Qwen2.5 72B Q4_K_M (~42GB), Mixtral 8x22B Q4 (~87GB), or even 120B+ models at lower quants
4. **For a Llamaste cluster** — buying 2-3 cheaper machines (4x RTX 3090 each) and using mesh networking may give better price/performance than one big server
5. **Power matters for appliance use** — the 2x A100 option at 600W GPU power is most appliance-friendly

---

## Price Trends (March 2026)

- **P40 24GB**: Bottomed out at $150-$200 used. Not going lower — floor price for 24GB VRAM.
- **RTX 3090 24GB**: $550-$750 used, down from $2,000+ at launch. May drop further as RTX 5090 lands.
- **RTX 4090 24GB**: $1,800-$2,200 used, $3,000+ refurb. Still relatively new, prices dropping slowly.
- **A100 80GB PCIe**: $5,500-$8,000 used, down from $15,000-$20,000. Expect continued decline as H100/H200 replace in datacenters.
- **A40 48GB**: $3,500-$4,500 used. Decent value but watch for A6000 price parity.
- **A6000 48GB**: $3,300-$4,500 used. Good value at this range.
- **L40S 48GB**: $8,000-$10,000 refurb. Too new to be good value — wait 1-2 years.

---

## Data Sources

- Newegg.com (fetched 2026-03-17) — A100, A40, P40, A6000, A30, L40S, RTX 3090, RTX 4090 listings
- eBay.com (fetched 2026-03-17) — A100 80GB (55 listings), A40 48GB (44 listings), sorted by price
- Amazon.com (fetched 2026-03-17) — A100 SXM4 80GB at $7,950
- getdeploying.com (fetched 2026-03-17) — Cloud GPU pricing comparison (A100 $0.40-$5.04/hr, RTX 4090 $0.18-$1.61/hr)
- Tim Dettmers GPU guide (timdettmers.com) — Performance/dollar analysis, multi-GPU recommendations
- r/LocalLLaMA community knowledge — Multi-GPU inference configurations, real-world experiences
