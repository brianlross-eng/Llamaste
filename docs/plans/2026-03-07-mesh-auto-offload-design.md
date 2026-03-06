# Mesh Auto-Offload Design — Phase 5

**Date**: 2026-03-07
**Status**: Design
**Depends on**: Phase 3b mesh clustering (complete)

---

## Goal

When multiple Llamaste nodes discover each other on the LAN, automatically:
1. Pool available RAM across the cluster
2. Select the largest model that fits in pooled RAM
3. Distribute model layers via llama.cpp RPC
4. Handle node join/leave with automatic redistribution
5. Provide monitoring and manual override

## Current State

Phase 3b gives us:
- **Peer discovery** via mDNS (`_llama-rpc._tcp.local`) + UDP multicast
- **Coordinator election** (RAM × 10 + CPU cores, lowest IP tiebreak)
- **llama-rpc-server** on all nodes (port 50052)
- **Topology change callback** (triggers model reload)
- **3 cluster tools**: `cluster.status`, `cluster.peers`, `cluster.reload`

What's missing:
- **Automatic model selection** based on cluster capacity
- **Explicit layer distribution** (currently relies on llama.cpp defaults)
- **`--tensor-split` calculation** based on per-node RAM
- **Model tier table** mapping RAM to best GGUF variant
- **Failover** on node departure
- **Monitoring** of per-node layer utilization

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│  COORDINATOR (highest election score)                            │
│                                                                   │
│  ┌──────────────┐  ┌──────────────────┐  ┌──────────────────┐   │
│  │ ModelSelector │  │ LayerDistributor │  │ ClusterMonitor   │   │
│  │              │  │                  │  │                  │   │
│  │ Reads pooled │  │ Calculates       │  │ Tracks per-node  │   │
│  │ RAM, picks   │  │ --tensor-split   │  │ latency, status  │   │
│  │ best GGUF    │  │ ratios per node  │  │ health checks    │   │
│  └──────┬───────┘  └────────┬─────────┘  └────────┬─────────┘   │
│         │                   │                     │               │
│         └───────────┬───────┘                     │               │
│                     ▼                             │               │
│  ┌─────────────────────────────┐                  │               │
│  │ llama-server --rpc a:p,b:p  │◄─────────────────┘               │
│  │   --tensor-split 0.4,0.6   │                                  │
│  │   -m best-model.gguf       │                                  │
│  └─────────────────────────────┘                                  │
│                                                                   │
│  ┌─────────────────────────────┐                                  │
│  │ llama-rpc-server :50052     │ ◄── local tensor execution       │
│  └─────────────────────────────┘                                  │
└───────────────────────────────────────────────────────────────────┘
          ▲                    ▲
          │ RPC (TCP binary)   │ RPC (TCP binary)
          ▼                    ▼
┌──────────────────┐  ┌──────────────────┐
│  WORKER A         │  │  WORKER B         │
│  rpc-server :50052│  │  rpc-server :50052│
│  RAM: 8GB         │  │  RAM: 16GB        │
└──────────────────┘  └──────────────────┘
```

## Key Design Decisions

### 1. Model Selection Strategy

**Model Tier Table** (Qwen2.5-Instruct, Q4_K_M quantization):

| Model | GGUF Size | Min RAM | Layers | Tokens/s (est.) |
|-------|----------|---------|--------|-----------------|
| 0.5B  | ~0.4 GB  | 1 GB   | 24     | 30-50           |
| 1.5B  | ~1.1 GB  | 2 GB   | 28     | 15-25           |
| 3B    | ~2.0 GB  | 3 GB   | 36     | 8-15            |
| 7B    | ~4.4 GB  | 6 GB   | 32     | 3-8             |
| 14B   | ~8.5 GB  | 12 GB  | 40     | 1.5-4           |
| 32B   | ~19 GB   | 24 GB  | 64     | 0.5-2           |
| 72B   | ~42 GB   | 52 GB  | 80     | 0.3-1           |

**Selection algorithm**:
```
usable_ram = sum(peer.ram_mb * 0.70 for peer in cluster)  # 70% safety margin
model = largest model where gguf_size < usable_ram
```

The 70% safety factor accounts for:
- OS/init overhead (~100-200 MB per node)
- KV cache allocation (~500 MB for 4096 context)
- llama.cpp RPC crash at >75% RAM utilization (known issue #15055)

### 2. Layer Distribution (--tensor-split)

llama.cpp's `--tensor-split` accepts ratios, not absolute values:

```
# 3 nodes: 4GB, 8GB, 16GB → ratios 1:2:4
--tensor-split 1,2,4
```

**Algorithm**:
```python
for each peer in cluster:
    peer.usable = peer.ram_mb * 0.70 - overhead_mb
    peer.ratio = peer.usable / min_usable  # normalize to smallest
# Pass ratios to --tensor-split
```

Note: The coordinator's own rpc-server is included in the endpoint list. Self-offloading is valid — llama-server talks to its own rpc-server for local tensor execution.

### 3. Topology Change Handling

**Node joins**:
1. New peer discovered via mDNS
2. Coordinator re-runs election (may change coordinator)
3. If model can be upgraded (more RAM available), pick bigger model
4. Kill current llama-server, respawn with new --rpc endpoints + --tensor-split
5. Weight transfer happens automatically (rpc-server caching helps)

**Node leaves** (detected by 60s heartbeat timeout):
1. Peer expires from ClusterManager
2. Coordinator detects topology change
3. Check if current model still fits in reduced cluster RAM
4. If yes: respawn llama-server with updated endpoints (subset of nodes)
5. If no: downgrade to smaller model that fits

**Coordinator failure**:
1. Workers detect coordinator gone (heartbeat timeout)
2. Re-election picks new coordinator
3. New coordinator spawns llama-server with --rpc endpoints
4. Workers keep running rpc-server (no restart needed)

### 4. Model File Management

Models must exist on the coordinator's filesystem. Options:

**Option A: Pre-installed models** (recommended for v1)
- Ship a model selector that picks from `/data/models/` based on available sizes
- Each node has its own model(s) downloaded
- Coordinator picks the model it has that matches cluster capacity

**Option B: Network model distribution** (future)
- Coordinator downloads model if not present
- Or streams model to rpc-servers (rpc-server caching handles this)
- More complex but enables zero-config model upgrades

### 5. RPC Server Caching

Pass `-c` to `llama-rpc-server` to enable tensor caching:
```
llama-rpc-server --host 0.0.0.0 --port 50052 -c
```

Benefits:
- First model load: ~5 min for 30GB over 1Gbps
- Subsequent loads: near-instant (hash-based cache at `$HOME/.cache/llama.cpp/rpc`)
- Critical for topology changes that trigger model reload

## New Code

### cluster.h — Extensions

```cpp
// Model tier for automatic selection
struct ModelTier {
    std::string name;        // "qwen2.5-0.5b", "qwen2.5-7b", etc.
    std::string gguf_path;   // "/data/models/qwen2.5-7b-instruct-q4_k_m.gguf"
    uint32_t size_mb;        // GGUF file size in MB
    uint32_t min_ram_mb;     // Minimum cluster RAM needed
    uint32_t layers;         // Number of transformer layers
};

// Cluster capacity analysis
struct ClusterCapacity {
    uint32_t total_ram_mb;      // Raw total across all nodes
    uint32_t usable_ram_mb;     // After 70% safety factor
    size_t node_count;
    std::string recommended_model;
    std::string tensor_split;    // "1,2,4" format
};

class ClusterManager {
    // ... existing ...

    // NEW: Analyze cluster capacity and recommend model
    ClusterCapacity analyze_capacity() const;

    // NEW: Get tensor-split string based on peer RAM ratios
    std::string compute_tensor_split() const;

    // NEW: Get list of available models on this node
    std::vector<ModelTier> available_models() const;

    // NEW: Select best model for current cluster
    ModelTier select_model() const;
};
```

### New Tools (3)

1. **cluster.capacity** — Show pooled RAM, recommended model, tensor split
2. **cluster.models** — List available models and which fits current cluster
3. **cluster.offload** — Manual override: force specific model + distribution

### Web UI — Cluster Dashboard Card

- Node list with RAM bars and layer assignments
- Current model name and size
- "Upgrade Available" badge when bigger model could fit
- Per-node latency (from rpc-server ping)

## Implementation Steps

### Phase 5a: Core Auto-Offload (1-2 days)
1. Add `ModelTier` struct and tier table to `cluster.h`
2. Implement `analyze_capacity()` — sum RAM, apply safety factor
3. Implement `compute_tensor_split()` — RAM-proportional ratios
4. Implement `select_model()` — pick largest fitting model
5. Modify `spawn_llama_server()` to use computed tensor-split and model
6. Enable rpc-server caching (`-c` flag)
7. Test with 2-node cluster on VirtualBox

### Phase 5b: Topology Change Handling (1 day)
8. Implement model upgrade on node join (larger cluster → bigger model)
9. Implement model downgrade on node leave (smaller cluster → smaller model)
10. Handle coordinator failover (re-election + model reload)
11. Add debounce timer to prevent thrashing (5s cooldown)

### Phase 5c: Monitoring + Tools (1 day)
12. Add `cluster.capacity` tool
13. Add `cluster.models` tool
14. Add `cluster.offload` tool (manual override)
15. Web UI cluster card with node list, RAM bars, model info
16. HTTP endpoints: GET `/llamaste/cluster/capacity`, GET `/llamaste/cluster/models`

### Phase 5d: Testing + Polish (1 day)
17. Unit tests for capacity analysis, model selection, tensor-split
18. Integration test with 2+ VMs
19. Stress test: node join/leave during inference
20. Documentation updates

## Performance Expectations

### Single Node (4GB, 2 cores)
- Model: 1.5B Q4_K_M
- Speed: ~15-25 tok/s

### Two Nodes (4GB + 4GB = 8GB pooled → 5.6GB usable)
- Model: 3B Q4_K_M (upgrade from 1.5B!)
- Speed: ~5-10 tok/s (slower per-token due to RPC, but smarter model)
- Tradeoff: Better quality responses at lower speed

### Four Nodes (4x4GB = 16GB pooled → 11.2GB usable)
- Model: 7B Q4_K_M
- Speed: ~2-5 tok/s
- Quality: Substantially better reasoning and instruction following

### Key Insight
The value of mesh isn't speed — it's **running models that don't fit in one machine's RAM**. A 7B model on 4 nodes will be slower than a 1.5B model on 1 node, but the 7B model is dramatically better at reasoning, following instructions, and producing useful output.

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| RPC latency makes large cluster slow | Document expectations; speed isn't the goal, model size is |
| Model reload disrupts active inference | Queue requests during reload (~30s window) |
| Node flapping causes thrashing | 5-second debounce timer on topology changes |
| Model files not present on coordinator | Scan `/data/models/` at boot, report available tiers |
| >75% RAM crash (known ORT issue) | 70% safety factor in capacity calculation |
| Coordinator migration loses KV cache | KV cache is per-session anyway; clients reconnect |

## Success Criteria

- [ ] 2+ nodes auto-discover and pool RAM
- [ ] Automatic model upgrade when nodes join
- [ ] Automatic model downgrade when nodes leave
- [ ] 7B model runs across 2x 4GB machines
- [ ] `cluster.capacity` shows accurate pooled RAM and recommendation
- [ ] Web UI shows cluster topology with per-node info
- [ ] No regression in single-node operation
