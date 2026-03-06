# Phase 3: Mesh Clustering Design

## Goal

Enable multiple Llamaste nodes on the same LAN to automatically discover each other, pool their RAM, and run models too large for any single machine using llama.cpp's built-in RPC backend.

## Architecture

Each Llamaste node runs two processes:

1. **llama-rpc-server** (port 50052) — exposes local compute/RAM to the cluster via llama.cpp's ggml-rpc binary protocol
2. **llama-server** — on the coordinator node only, started with `--rpc node1:50052,node2:50052,...` to distribute tensor operations across all workers

Discovery reuses the existing `MdnsResponder` (already built in Phase 3a). Each node advertises `_llama-rpc._tcp.local` with TXT records describing its capabilities. Nodes find peers by sending mDNS PTR queries for that service type.

## Components

### 1. Cluster Manager (`cluster.h` / `cluster.cpp`, ~400 LOC)

**State machine:**
```
STANDALONE → DISCOVERING → WORKER
                         → COORDINATOR
WORKER → RE_ELECTING → WORKER | COORDINATOR
COORDINATOR → RE_ELECTING → WORKER | COORDINATOR
```

**Lifecycle:**
- On boot: start llama-rpc-server, advertise service, query for peers (2s window)
- Election: composite score = `free_ram_gb * 10 + cpu_cores`. Highest wins, lowest IP breaks ties.
- Heartbeat: mDNS re-announcement every 30s, peer timeout after 90s of silence
- On topology change: re-election, coordinator reloads llama-server with updated `--rpc` list

**Peer tracking:**
```cpp
struct PeerInfo {
    std::string hostname;
    std::string ip;
    uint16_t rpc_port;      // 50052
    uint32_t ram_mb;         // free RAM in MB
    uint32_t cpu_cores;
    std::string model;       // currently loaded model or "none"
    std::chrono::steady_clock::time_point last_seen;
};
```

**Election score:**
```cpp
int election_score(const PeerInfo& p) {
    return (p.ram_mb / 1024) * 10 + p.cpu_cores;
}
// Ties broken by lowest IP (lexicographic)
```

### 2. llama-rpc-server Binary

Built from the existing `llama-server` Buildroot package by enabling `-DGGML_RPC=ON`.

**Changes to `llama-server.mk`:**
- Set `DGGML_RPC=ON`
- Add `llama-rpc-server` to install commands alongside `llama-server`

**Runtime:**
- Always started on boot (port 50052), even on standalone nodes
- Managed by child_main.cpp alongside llama-server (fork/exec, PID tracking, crash recovery)
- Standalone node = "cluster of one" — coordinator talks to its own local RPC server

### 3. Coordinator Duties

The elected coordinator:
- Kills any existing llama-server process
- Restarts llama-server with `--rpc worker1:50052,worker2:50052,...`
- On node join: lazy — new worker used on next model load (no disruption)
- On node leave: if worker was active in current model, force reload with remaining workers
- Passes all existing llama-server flags (context size, thread count, etc.)

### 4. mDNS Extensions

Add `discover_services()` method to existing `MdnsResponder`:
- Sends mDNS PTR query for `_llama-rpc._tcp.local`
- Collects SRV+TXT responses for 2 seconds
- Returns vector of discovered peers
- Called on boot and periodically (every 30s) for peer refresh

**TXT records advertised:**
```
ram=16384          // free RAM in MB
cores=4            // CPU core count
rpc_port=50052     // RPC listen port
model=none         // or "qwen2.5-1.5b-q4_k_m"
```

### 5. Cluster Tools (`tools_cluster.cpp`)

| Tool | Description |
|------|-------------|
| `cluster.status` | Role, peer count, coordinator IP, total pooled RAM, model distribution |
| `cluster.peers` | Detailed peer list: hostname, IP, RAM, CPU, model, last_seen, latency |
| `cluster.reload` | Force re-election and model redistribution |

### 6. Web UI Dashboard Card

New "Cluster" card in system panel:
- Node count and role (Coordinator / Worker / Standalone)
- Peer table: hostname, IP, RAM, status
- Total pooled RAM
- Currently distributed model (if any)

## Data Flow

```
Boot
  ├─ fork/exec llama-rpc-server --host 0.0.0.0 --port 50052
  ├─ mdns.advertise_service("_llama-rpc._tcp", 50052, {ram, cores, ...})
  ├─ peers = mdns.discover_services("_llama-rpc._tcp")  // 2s query window
  ├─ run_election(self, peers)
  │   ├─ winner == self → become COORDINATOR
  │   │   └─ spawn llama-server --rpc self:50052,peer1:50052,...
  │   └─ winner != self → become WORKER
  │       └─ llama-rpc-server already running, nothing else needed
  └─ start heartbeat thread (re-announce every 30s, expire peers after 90s)

Model load (coordinator only):
  ├─ gather current peer list
  ├─ build --rpc flag string
  ├─ restart llama-server with --rpc + --model
  └─ inference requests flow through coordinator's llama-server
      which distributes tensor ops to workers via RPC

Node join:
  ├─ new node announces _llama-rpc._tcp
  ├─ existing nodes see announcement, add to peer list
  ├─ re-election (new node may have more RAM)
  └─ next model load uses new worker (lazy)

Node leave (graceful):
  ├─ node sends mDNS goodbye (TTL=0)
  ├─ peers remove it from list
  ├─ re-election if coordinator left
  └─ if worker was active → coordinator reloads llama-server

Node leave (crash):
  ├─ heartbeat timeout (90s)
  ├─ peer removed from list
  ├─ re-election if coordinator
  └─ coordinator reloads llama-server without crashed worker
```

## What We're NOT Building

- **No RPC encryption/auth** — LAN-only threat model per research doc
- **No WiFi mesh** — GbE required (WiFi adds 30ms/token overhead)
- **No manual layer assignment** — llama.cpp RPC handles distribution automatically
- **No persistent cluster state** — fully dynamic, re-discovered on boot
- **No cross-subnet discovery** — mDNS is link-local only

## Testing

- Unit tests: election algorithm, peer tracking, state machine transitions, score calculation
- Integration tests: mock mDNS discovery responses, coordinator restart logic
- Multi-node E2E requires multiple QEMU instances (stretch goal, not required)

## Network Requirements

Per research doc (05-mesh-clustering.md):
- Gigabit Ethernet minimum
- Sub-1ms LAN latency
- Per-token activation: negligible cross-node traffic (10-100 KB)
- 7B model weights: ~4 GB transfer on first load (32s on GbE)
- 70B model weights: ~13 GB per node (104s on GbE)

## Performance Expectations

- 7B Q4_K_M across 2 nodes: 1.2-1.5x speedup
- 70B Q4_K_M across 4x 16GB nodes: 1-3 tok/s (enables models impossible on single node)
- Standalone node: zero overhead (talks to its own local RPC server)

## Dependencies

- llama.cpp b5460 (already in Buildroot) with `-DGGML_RPC=ON`
- Existing MdnsResponder (net_mdns.h/cpp)
- Existing llama-server lifecycle management (child_main.cpp)
