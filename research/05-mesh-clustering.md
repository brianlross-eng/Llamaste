# Mesh Clustering and Distributed Inference

## Discovery: mDNS/Avahi

### How mDNS Works
- Multicast UDP on port 5353, group 224.0.0.251
- Devices claim `.local` hostnames, respond to queries
- Same wire format as standard DNS
- Built-in conflict detection

### Avahi (Linux Implementation)

Minimal daemon config (`/etc/avahi/avahi-daemon.conf`):
```ini
[server]
host-name=llamaste-node1
domain-name=local
use-ipv4=yes
use-ipv6=no

[publish]
publish-addresses=yes
publish-hinfo=no
publish-workstation=no
```

Service file (`/etc/avahi/services/llm-rpc.service`):
```xml
<?xml version="1.0" standalone='no'?>
<service-group>
  <name replace-wildcards="yes">LLM-RPC on %h</name>
  <service>
    <type>_llm-rpc._tcp</type>
    <port>50052</port>
    <txt-record>ram=16384</txt-record>
    <txt-record>cores=8</txt-record>
    <txt-record>model=llama-7b</txt-record>
  </service>
</service-group>
```

Footprint: avahi-daemon ~2-5 MB RSS. Requires D-Bus (~800 KB).

### Alternative: Custom UDP Multicast (Zero Dependencies)

```python
MULTICAST_GROUP = '239.255.77.77'
MULTICAST_PORT = 50051

# Sender
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)  # LAN only
sock.sendto(payload, (MULTICAST_GROUP, MULTICAST_PORT))

# Receiver
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('', MULTICAST_PORT))
mreq = socket.inet_aton(MULTICAST_GROUP) + socket.inet_aton('0.0.0.0')
sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)
```

### Discovery Mechanism Comparison

| Mechanism | Dependencies | Metadata | Complexity |
|-----------|-------------|----------|------------|
| Avahi mDNS | avahi, dbus | TXT records | Medium |
| Custom UDP Multicast | None | Custom JSON | Low |
| Custom UDP Broadcast | None | Custom | Low |

**Recommendation:** Start with custom UDP multicast for zero dependencies. Migrate to Avahi later if interoperability needed.

## llama.cpp RPC Backend

### Architecture
- Pipeline parallelism: different layers on different machines
- Custom binary protocol over raw TCP (port 50052)
- No HTTP, no protobuf -- minimal overhead
- Operations: ALLOC_BUFFER, SET_TENSOR, GET_TENSOR, GRAPH_COMPUTE

### Setup

```bash
# Build with RPC
cmake -B build -DGGML_RPC=ON

# Worker nodes:
./llama-rpc-server --host 0.0.0.0 --port 50052

# Coordinator:
./llama-server -m model.gguf \
  --rpc 192.168.1.11:50052,192.168.1.12:50052 \
  -ngl 99
```

### Layer Distribution
- Layers assigned proportionally to available RAM
- Weight tensors transferred at startup via SET_TENSOR
- During inference, computation graphs dispatched to appropriate node
- Results flow back, activations passed between backends

## Network Requirements

### Bandwidth

| Phase | Data Volume | On 1 GbE | On 10 GbE |
|-------|------------|----------|-----------|
| 7B weight transfer | ~4 GB | ~32s | ~3.2s |
| 70B weight transfer (3 nodes) | ~13 GB each | ~104s | ~10s |
| Per-token activation | 10-100 KB | Negligible | Negligible |

### Latency (Critical Factor)
- Each cross-node transition adds a round trip
- 32-layer model on 4 nodes = 3 transitions per token
- At 0.1ms LAN: 0.3ms/token (negligible)
- At 1ms LAN: 3ms/token (acceptable)
- At 10ms WiFi: 30ms/token (significant bottleneck)

**Requirements:** Gigabit Ethernet minimum, same subnet, wired, sub-1ms latency.

### TCP Tuning
```bash
sysctl -w net.core.rmem_max=16777216
sysctl -w net.core.wmem_max=16777216
sysctl -w net.ipv4.tcp_rmem="4096 87380 16777216"
sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"
ip link set eth0 mtu 9000  # Jumbo frames
```

## Zero-Config Clustering Architecture

```
  Node A (16GB)  <--mDNS/UDP-->  Node B (32GB)
     |                               |
     +----------+--------------------+
                |
          Node C (8GB)

  Election: Node B (most RAM) becomes coordinator
  Coordinator runs llama-server with --rpc pointing to workers
```

### Coordinator Election
- Composite score: RAM + CPU cores + uptime
- Highest score wins; ties broken by lowest IP
- Re-election on topology change

### Layer Distribution
```
total_layers = model.layers
for each node sorted by RAM (descending):
    share = node.free_ram / total_ram
    assigned_layers = round(share * total_layers)
```

### Node State Machine
```
INIT -> JOINING -> COORDINATOR (if elected)
INIT -> JOINING -> WORKER (if not elected)
WORKER -> RE-ELECT (if coordinator lost) -> JOINING
```

## Dynamic Membership

### Node Join: Lazy approach (recommended)
- New node added to pool, used on next model load
- No disruption to active inference

### Node Leave
- **Graceful:** Announce departure, coordinator redistributes
- **Ungraceful:** Heartbeat timeout, full model reload required

### Heartbeat
```
HEARTBEAT_INTERVAL = 2 seconds
FAILURE_THRESHOLD = 3 missed heartbeats (6 seconds)
```

## Security

### Threat Model (LAN only)
- Rogue nodes, ARP spoofing, lateral movement, plaintext data exposure

### llama.cpp RPC: No built-in auth or encryption

### Mitigations

| Level | Measures | Overhead |
|-------|----------|----------|
| Basic | Firewall rules, bind to specific IP | None |
| Medium | Dedicated VLAN + firewall | None |
| High | WireGuard tunnel + VLAN | 2-5% |

```bash
# Firewall: restrict RPC to cluster interface
iptables -A INPUT -p tcp --dport 50052 ! -i eth0.100 -j DROP

# WireGuard overlay
wg-quick up cluster  # ~60KB kernel module
llama-rpc-server --host 10.0.100.1 --port 50052  # Bind to WG interface
```

## Distributed Performance

### 7B Q4_K_M
- Single machine (16 cores): ~10-15 tok/s
- 2 machines (GbE): ~12-18 tok/s (1.2-1.5x speedup)

### 70B Q4_K_M
- Single 16GB machine: **impossible**
- 4x 16GB machines (GbE): ~1-3 tok/s (but it RUNS)

### Key Value: Running models that don't fit in one machine's RAM

## Minimal Networking Stack

### Kernel Config
```
CONFIG_NET=y
CONFIG_INET=y
CONFIG_IP_MULTICAST=y
CONFIG_PACKET=y
CONFIG_UNIX=y  # For D-Bus if using Avahi
CONFIG_E1000E=m  # Intel NICs
CONFIG_R8169=m   # Realtek NICs
# Optional: CONFIG_WIREGUARD=m (~60KB)
```

### Userspace (custom discovery, no Avahi)
- libc
- llama-rpc-server
- discovery agent (statically linked)
- Total: libc + 2 binaries

## Existing Similar Projects

| Project | Auto-Discovery | CPU Support | Dependencies |
|---------|---------------|-------------|-------------|
| **Exo** | Yes | Yes | Python |
| **Petals** | Yes (DHT) | GPU-focused | Python |
| **llama.cpp RPC** | No | Yes | Minimal (C++) |

Gap: No minimal, zero-config, LAN-optimized clustering for llama.cpp CPU inference exists.
