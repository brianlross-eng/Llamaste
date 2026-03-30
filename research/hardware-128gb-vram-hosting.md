# GPU Cloud Hosting: 128GB+ VRAM for AI Inference

**Research Date**: 2026-03-17
**Purpose**: Compare cloud/hosting options providing 128GB+ total GPU VRAM for LLM inference workloads

---

## GPU Options That Reach 128GB+ VRAM

| GPU | VRAM per GPU | GPUs Needed for 128GB+ | Total VRAM | Notes |
|-----|-------------|----------------------|------------|-------|
| NVIDIA B200 | 180-192GB | 1 | 180-192GB | Blackwell arch, 8TB/s bandwidth. Best single-GPU option |
| NVIDIA H200 | 141GB | 1 | 141GB | Hopper arch, 4.8TB/s bandwidth. Sweet spot for 128GB+ |
| NVIDIA H100 80GB | 80GB | 2 | 160GB | Hopper arch, NVLink for multi-GPU. Most available |
| NVIDIA A100 80GB | 80GB | 2 | 160GB | Ampere arch, cheapest multi-GPU option |
| NVIDIA L40S | 48GB | 3 | 144GB | Ada Lovelace, PCIe only. Cheaper but slower interconnect |

---

## Provider Comparison

### Tier 1: Single-GPU 128GB+ (Best for Simplicity)

These configurations provide 128GB+ VRAM from a SINGLE GPU — no tensor parallelism needed.

#### NVIDIA H200 (141GB VRAM per GPU)

| Provider | $/GPU/hr (On-Demand) | $/month (730hr) | Spot/Interruptible | Regions | Notes |
|----------|---------------------|-----------------|-------------------|---------|-------|
| **Packet.ai** | $1.50 | ~$1,095 | N/A | US | Cheapest H200 available (sponsored listing) |
| **DigitalOcean** | $3.44 | ~$2,511 | N/A | Global | 24 vCPUs, 240GB RAM included |
| **RunPod** | $3.59 | ~$2,621 | Community cloud cheaper | US, EU | 26 vCPUs, 120GB RAM. Community ~$2.50-3.00 |
| **Together** | $4.99 | ~$3,643 | N/A | US | Serverless inference API also available |
| **Hyperstack** | $3.50 | ~$2,555 | N/A | US, EU | SXM variant |
| **GCP (A3)** | ~$3.72 (spot) | ~$2,716 (spot) | Yes, $3.72/hr | Global | Spot = preemptible, can be interrupted |
| **Koyeb** | ~$4.50 | ~$3,285 | N/A | EU, US | Managed platform |

#### NVIDIA B200 (180-192GB VRAM per GPU)

| Provider | $/GPU/hr (On-Demand) | $/month (730hr) | Spot | Regions | Notes |
|----------|---------------------|-----------------|------|---------|-------|
| **Packet.ai** | $2.25 | ~$1,643 | N/A | US | 180GB NVL variant, cheapest B200 |
| **Lyceum** | $4.29 | ~$3,132 | N/A | EU | 192GB variant, EU-sovereign cloud |
| **RunPod** | $4.99 | ~$3,643 | Community cheaper | US, EU | 180GB SXM |
| **Lambda Labs** | $6.08 | ~$4,438 | N/A | US | 180GB SXM, 360GB system RAM, no egress fees |
| **CoreWeave** | ~$6.16 | ~$4,497 | Reserved -60% | US | 8-GPU nodes available. Reserved: ~$2.46/hr |
| **Verda** | ~$3.50 | ~$2,555 | N/A | US, EU | Newer provider |

---

### Tier 2: Multi-GPU 128GB+ (2x H100/A100 80GB = 160GB)

#### 2x NVIDIA H100 80GB (160GB total VRAM)

| Provider | $/hr (2 GPUs) | $/month (730hr) | Spot/Interruptible | Regions | Notes |
|----------|--------------|-----------------|-------------------|---------|-------|
| **Thunder Compute** | ~$2.50 ($1.25x2) | ~$1,825 | N/A | US | Absolute cheapest H100 found |
| **Vast.ai** | ~$2.98-$3.74 ($1.49-$1.87x2) | ~$2,175-$2,730 | Yes, marketplace driven | Global | Prices fluctuate. Interruptible cheaper |
| **TensorDock** | ~$3.80 ($1.90x2) | ~$2,774 | Spot: ~$2.60 ($1.30x2) | Global | Spot at $1.30/GPU/hr |
| **RunPod** | ~$3.98-$4.78 ($1.99-$2.39x2) | ~$2,905-$3,489 | Community cloud cheaper | US, EU | Community $1.99, Secure $2.39 |
| **Lambda Labs** | ~$5.98 ($2.99x2) | ~$4,365 | N/A | US | No egress fees. Reserved pricing via sales |
| **Hyperstack** | ~$3.80-$4.80 ($1.90-$2.40x2) | ~$2,774-$3,504 | N/A | US, EU | NVLink $1.95/GPU, SXM $2.40/GPU |
| **AWS (P5)** | ~$7.80 ($3.90x2) | ~$5,694 | Spot: ~$5.00 ($2.50x2) | Global | Savings Plan 1-3yr: ~$3.80-$4.20 total |
| **GCP (A3)** | ~$6.00 ($3.00x2) | ~$4,380 | Spot: ~$4.50 ($2.25x2) | Global | CUD (1-3yr) discounts available |
| **Azure** | ~$13.96 ($6.98x2) | ~$10,191 | N/A listed | Global | Most expensive option |
| **CoreWeave** | ~$9.52 ($4.76x2) | ~$6,950 | Reserved -60%: ~$3.81 | US | GPU-only price; CPU/RAM extra |

#### 2x NVIDIA A100 80GB (160GB total VRAM)

| Provider | $/hr (2 GPUs) | $/month (730hr) | Spot/Interruptible | Regions | Notes |
|----------|--------------|-----------------|-------------------|---------|-------|
| **Vast.ai** | ~$2.78 ($1.39x2) | ~$2,029 | Yes, marketplace | Global | Cheapest A100 option |
| **TensorDock** | ~$3.26 ($1.63x2) | ~$2,380 | Spot: ~$1.34 ($0.67x2) | Global | **Best spot pricing** |
| **Lambda Labs** | ~$2.20 ($1.10x2) | ~$1,606 | N/A | US | Very competitive, no egress |
| **Hyperstack** | ~$2.70-$3.20 ($1.35-$1.60x2) | ~$1,971-$2,336 | N/A | US, EU | NVLink $1.40, SXM $1.60 |
| **RunPod** | ~$2.78-$3.78 ($1.39-$1.89x2) | ~$2,029-$2,759 | Community cheaper | US, EU | Community $1.39, Secure $1.89 |
| **CoreWeave** | ~$4.42 ($2.21x2) | ~$3,227 | Reserved -60%: ~$1.77 | US | A100 80GB NVLink |
| **Paperspace/DO** | ~$6.36 ($3.18x2) | ~$4,643 | 36-mo: ~$2.30 ($1.15x2) | US, EU | Requires $39/mo Growth plan |

---

### Tier 3: Dedicated Servers (Monthly Fixed Cost)

| Provider | Config | Total VRAM | Monthly Price | $/hr Equiv. | Location | Notes |
|----------|--------|-----------|---------------|-------------|----------|-------|
| **Hetzner GEX131** | 1x RTX PRO 6000 Blackwell Max-Q | 96GB | €889 (~$989) | ~$1.35 | Germany (NBG1, FSN1) | Only 96GB — below 128GB threshold |
| **Hetzner GEX44** | 1x RTX 4000 SFF Ada | 20GB | €184 (~$191) | ~$0.26 | Germany | Too small for 128GB+ |
| **OVH HGR-AI** | L40S (varies) | 48-192GB | Custom quote | Varies | EU, NA, APAC | Contact sales for multi-GPU configs |
| **OVH Scale-GPU** | NVIDIA L4 | 24GB per GPU | From €2.75/hr | ~$3.00/hr | EU, NA | Need 6+ GPUs for 128GB |

**Note**: Hetzner's current dedicated GPU servers max at 96GB VRAM (single RTX PRO 6000). They do not yet offer multi-GPU or 128GB+ VRAM dedicated configurations. For 128GB+ VRAM dedicated bare metal, OVH HGR-AI or colocation is needed.

---

### Tier 4: Hyperscalers (AWS, GCP, Azure)

| Provider | Config | Total VRAM | On-Demand $/hr | Reserved (1yr) | Spot | Regions |
|----------|--------|-----------|---------------|----------------|------|---------|
| **AWS P5** | 8x H100 80GB | 640GB | ~$31.20/node | ~$15-22/node | ~$20/node | us-east-1, us-west-2, eu-west-1 |
| **AWS P5** | 2x H100 (if available) | 160GB | ~$7.80 | ~$3.80-4.20 | ~$5.00 | Limited availability |
| **GCP A3-High** | 1x H100 80GB | 80GB | $3.00 | CUD discounts | $2.25 | us-central1, europe-west4 |
| **GCP A3-Mega** | 4x H100 80GB | 320GB | ~$12.00 | CUD discounts | $9.52 | us-central1 |
| **Azure NC H100** | 1x H100 80GB | 80GB | $6.98 | 1yr RI ~$4.50 | N/A | East US, West Europe |

**Note**: AWS/GCP/Azure usually sell H100s in 1-GPU or 8-GPU increments. Getting exactly 2x H100 may require special instance types or isn't always available.

---

## Spot/Interruptible Pricing Summary

Spot instances offer 30-70% savings but can be interrupted with short notice.

| Provider | GPU | Spot $/GPU/hr | vs On-Demand Savings | Interruption Risk |
|----------|-----|--------------|---------------------|-------------------|
| **TensorDock** | A100 80GB | $0.67 | ~59% off | Medium |
| **TensorDock** | H100 80GB | $1.30 | ~32% off | Medium |
| **Vast.ai** | H100 80GB | $1.49-$1.87 | ~20-40% off | High (marketplace) |
| **GCP Spot** | H100 80GB | $2.25 | ~25% off | Medium |
| **AWS Spot** | H100 80GB | $2.50 | ~36% off | Medium-High |

---

## Reserved/Committed Pricing Summary

Long-term commitments offer 30-60% savings.

| Provider | Commitment | Discount | Notes |
|----------|-----------|----------|-------|
| **CoreWeave** | Custom term | Up to 60% off | Contact sales |
| **AWS Savings Plans** | 1-3 year | 25-45% off | Flexible across instance types |
| **GCP CUDs** | 1-3 year | 25-40% off | Committed use discounts |
| **Paperspace** | 36-month | ~64% off A100 ($3.18→$1.15) | Requires Growth plan |
| **Lambda Labs** | Custom | Contact sales | Long-term capacity available |

---

## Best Value Recommendations

### For 24/7 Production Inference (Always-On)

**Best overall value**: 1x H200 on Packet.ai at $1.50/hr = **$1,095/month** for 141GB VRAM
- Single GPU = no tensor parallelism overhead
- 141GB handles most 70B-parameter models (Q4/Q5 quantized)

**Runner-up**: 2x A100 80GB on Lambda Labs at $2.20/hr = **$1,606/month** for 160GB VRAM
- More VRAM (160GB) for larger models
- No egress fees — important if serving many users
- Lambda has excellent uptime SLAs

**Budget option**: 2x A100 80GB on TensorDock spot at ~$1.34/hr = **$978/month** for 160GB
- Risk of interruption — need checkpoint/restart logic
- Best $/VRAM ratio available

### For Burst/Development Workloads (Not 24/7)

**Best flexibility**: Vast.ai marketplace
- H100 from $1.49-$1.87/GPU/hr, A100 from $1.39/GPU/hr
- No commitments, pay only when used
- Dynamic pricing means checking for deals

**Best managed experience**: RunPod
- H200 at $3.59/hr, H100 at $1.99/hr (Community Cloud)
- Serverless inference endpoints available
- Good tooling and DX

### For Maximum VRAM (192GB Single GPU)

**Best option**: 1x B200 on Packet.ai at $2.25/hr = **$1,643/month** for 180GB VRAM
- Blackwell architecture, 8TB/s memory bandwidth
- Future-proof, handles 120B+ parameter models

**Premium option**: 1x B200 on Lyceum at $4.29/hr = **$3,132/month** for 192GB VRAM
- EU-sovereign cloud (GDPR compliance)
- Full 192GB variant

### For Cost-Sensitive 24/7 (Minimum Viable 128GB+)

**Cheapest path to 128GB+ VRAM**:
1. **TensorDock spot 2x A100 80GB**: ~$978/month (interruptible)
2. **Packet.ai 1x H200**: ~$1,095/month (on-demand, no interruption)
3. **Lambda Labs 2x A100 80GB**: ~$1,606/month (reliable, no egress)

### For Enterprise (SLA, Support, Compliance)

1. **CoreWeave** — reserved H100/B200, up to 60% off, HPC-grade
2. **AWS/GCP** — savings plans, global regions, compliance certifications
3. **Lyceum** — EU-sovereign B200 cloud

---

## Key Considerations for Llamaste

For running large LLMs (70B+ parameter models) on Llamaste:

1. **70B Q4_K_M** needs ~40GB VRAM — a single H100 80GB suffices (~$1.25-$3.00/hr)
2. **70B FP16** needs ~140GB VRAM — 1x H200 ($1.50/hr) or 2x H100 ($2.50-$6.00/hr)
3. **120B+ models** need 180GB+ — 1x B200 ($2.25-$6.08/hr) or 2x H200 ($3.00-$7.18/hr)
4. **Cluster offloading**: Llamaste's multi-node mesh could split across cheaper GPUs (e.g., 4x L40S at $0.26/hr each = ~$1.04/hr for 192GB total, but slower interconnect)

**Recommendation for Llamaste cloud demo/testing**: Packet.ai 1x H200 at $1.50/hr is the sweet spot — 141GB VRAM, single GPU simplicity, affordable for demo purposes.

---

## Sources

- [GPU Price Comparison 2026](https://getdeploying.com/gpus)
- [H100 Rental Prices Compared (IntuitionLabs)](https://intuitionlabs.ai/articles/h100-rental-prices-cloud-comparison)
- [RunPod Pricing](https://www.runpod.io/pricing)
- [CoreWeave Pricing](https://www.coreweave.com/pricing)
- [Lambda AI Pricing](https://lambda.ai/pricing)
- [Vast.ai Pricing](https://vast.ai/pricing)
- [TensorDock H100](https://www.tensordock.com/gpu-h100.html)
- [TensorDock A100](https://www.tensordock.com/gpu-a100.html)
- [Hetzner GEX131](https://www.hetzner.com/dedicated-rootserver/gex131/)
- [Hyperstack Pricing](https://www.hyperstack.cloud/gpu-pricing)
- [Paperspace/DigitalOcean Pricing](https://www.paperspace.com/pricing)
- [OVHcloud GPU](https://us.ovhcloud.com/public-cloud/gpu/)
- [H200 Provider Comparison](https://getdeploying.com/gpus/nvidia-h200)
- [B200 Provider Comparison](https://getdeploying.com/gpus/nvidia-b200)
