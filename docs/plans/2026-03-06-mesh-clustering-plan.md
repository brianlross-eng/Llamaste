# Phase 3: Mesh Clustering Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable multiple Llamaste nodes on the same LAN to auto-discover each other, elect a coordinator, and run distributed inference via llama.cpp's built-in RPC backend.

**Architecture:** Each node runs `llama-rpc-server` (port 50052) to expose local compute. Nodes discover each other via mDNS DNS-SD (`_llama-rpc._tcp.local`). The elected coordinator spawns `llama-server --rpc <all-workers>` to distribute tensor ops. The existing `MdnsResponder` is extended with service discovery (query, not just respond).

**Tech Stack:** C++17, llama.cpp ggml-rpc, mDNS/DNS-SD (existing net_mdns), Buildroot/musl, nlohmann/json

**Design doc:** `docs/plans/2026-03-06-mesh-clustering-design.md`

---

## Task 1: Enable GGML_RPC in llama-server Buildroot Package

**Files:**
- Modify: `br2-external/package/llama-server/llama-server.mk`

**Context:** The llama-server Buildroot package already builds llama.cpp at tag b5460. It has `DGGML_RPC=OFF`. We need to flip it ON and install the `llama-rpc-server` binary alongside `llama-server`.

**Step 1: Update llama-server.mk**

Change `DGGML_RPC=OFF` to `DGGML_RPC=ON` and add `llama-rpc-server` to install commands:

```makefile
LLAMA_SERVER_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DGGML_STATIC=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_CPU=ON \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_METAL=OFF \
	-DGGML_RPC=ON \
	-DGGML_BLAS=OFF \
	-DLLAMA_CURL=OFF \
	-DLLAMA_BUILD_TESTS=OFF \
	-DLLAMA_BUILD_EXAMPLES=OFF \
	-DLLAMA_BUILD_SERVER=ON

define LLAMA_SERVER_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/bin/llama-server \
		$(TARGET_DIR)/opt/llamaste/llama-server
	$(INSTALL) -D -m 0755 $(@D)/bin/llama-rpc-server \
		$(TARGET_DIR)/opt/llamaste/llama-rpc-server
endef
```

**Step 2: Rebuild llama-server package**

```bash
cd /root/llamaste-build/output && make llama-server-dirclean && make llama-server
```

Expected: both `llama-server` and `llama-rpc-server` appear in `output/target/opt/llamaste/`.

**Step 3: Verify binary exists**

```bash
ls -la /root/llamaste-build/output/target/opt/llamaste/llama-rpc-server
file /root/llamaste-build/output/target/opt/llamaste/llama-rpc-server
```

Expected: ELF 64-bit x86-64 static binary.

**Step 4: Commit**

```bash
git add br2-external/package/llama-server/llama-server.mk
git commit -m "feat: enable GGML_RPC and install llama-rpc-server binary"
```

---

## Task 2: Add mDNS Service Discovery (Query Method)

**Files:**
- Modify: `src/llamaste/net_mdns.h`
- Modify: `src/llamaste/net_mdns.cpp`
- Modify: `tests/test_net.cpp`

**Context:** The MdnsResponder currently only *responds* to queries and *announces* services. For mesh clustering, nodes need to *discover* other nodes by sending PTR queries and collecting responses. We add a `discover_services()` method.

**Step 1: Write failing tests in `tests/test_net.cpp`**

Add these tests at the end of the file, before the `main()` function:

```cpp
// --- Mesh discovery tests ---

static void test_build_ptr_query() {
    // build_ptr_query should create a valid mDNS PTR query packet
    auto pkt = MdnsResponder::build_ptr_query("_llama-rpc._tcp.local");
    assert(pkt.size() > 12);  // at minimum: 12-byte header + encoded name + 4 bytes (type+class)

    // Header: QR=0 (query), QDCOUNT=1
    assert((pkt[2] & 0x80) == 0);  // QR bit = 0 (query)
    assert(pkt[4] == 0 && pkt[5] == 1);  // QDCOUNT = 1

    // Decode question name
    size_t off = 12;
    std::string name = MdnsResponder::decode_dns_name(pkt.data(), pkt.size(), off);
    assert(name == "_llama-rpc._tcp.local");

    // QTYPE = PTR (12), QCLASS = IN (1) with unicast-response bit
    uint16_t qtype = (pkt[off] << 8) | pkt[off+1];
    assert(qtype == 12);

    printf("  PASS: build_ptr_query\n");
}

static void test_parse_service_response() {
    // Build a service response and parse peer info from it
    MdnsServiceRecord svc;
    svc.browse_name = "_llama-rpc._tcp.local";
    svc.instance_name = "llamaste._llama-rpc._tcp.local";
    svc.port = 50052;
    svc.txt = {"ram=8192", "cores=4", "model=none"};

    uint8_t ip[4] = {192, 168, 1, 42};
    auto pkt = MdnsResponder::build_service_response(svc, "llamaste.local", ip, 0);

    // parse_service_responses should extract peer info
    auto peers = MdnsResponder::parse_service_responses(pkt.data(), pkt.size());
    assert(peers.size() == 1);
    assert(peers[0].hostname == "llamaste");
    assert(peers[0].ip == "192.168.1.42");
    assert(peers[0].port == 50052);
    assert(peers[0].txt.size() == 3);
    assert(peers[0].txt[0] == "ram=8192");

    printf("  PASS: parse_service_response\n");
}
```

Add to the `run_net_tests()` function:
```cpp
test_build_ptr_query();
test_parse_service_response();
```

**Step 2: Run tests to verify they fail**

```bash
cd /d/Llamaste && bash scripts/host-test.sh net
```

Expected: FAIL — `build_ptr_query` and `parse_service_responses` not declared.

**Step 3: Add declarations to `net_mdns.h`**

Add after the existing `build_service_response` declaration (around line 71):

```cpp
    // Build an mDNS PTR query packet for a service type.
    // service_fqdn: e.g. "_llama-rpc._tcp.local"
    static std::vector<uint8_t> build_ptr_query(const std::string& service_fqdn);

    // Parse peer info from a service response packet.
    // Returns discovered services (hostname, IP, port, TXT records).
    struct DiscoveredService {
        std::string hostname;
        std::string ip;
        uint16_t port = 0;
        std::vector<std::string> txt;
    };
    static std::vector<DiscoveredService> parse_service_responses(
        const uint8_t* pkt, size_t pkt_len);
```

**Step 4: Implement in `net_mdns.cpp`**

Add `build_ptr_query()`:

```cpp
std::vector<uint8_t> MdnsResponder::build_ptr_query(const std::string& service_fqdn) {
    std::vector<uint8_t> pkt;
    // Header: ID=0, QR=0 (query), QDCOUNT=1
    pkt.resize(12, 0);
    pkt[5] = 1;  // QDCOUNT = 1

    // Question: encoded service name
    auto name = encode_dns_name(service_fqdn);
    pkt.insert(pkt.end(), name.begin(), name.end());

    // QTYPE = PTR (12)
    pkt.push_back(0); pkt.push_back(12);
    // QCLASS = IN (1) with unicast-response bit (0x8001)
    pkt.push_back(0x80); pkt.push_back(0x01);

    return pkt;
}
```

Add `parse_service_responses()`:

```cpp
std::vector<MdnsResponder::DiscoveredService> MdnsResponder::parse_service_responses(
        const uint8_t* pkt, size_t pkt_len) {
    std::vector<DiscoveredService> result;
    if (pkt_len < 12) return result;

    uint16_t ancount = (pkt[6] << 8) | pkt[7];
    uint16_t arcount = (pkt[10] << 8) | pkt[11];
    uint16_t qdcount = (pkt[4] << 8) | pkt[5];

    // Skip questions
    size_t off = 12;
    for (int i = 0; i < qdcount && off < pkt_len; i++) {
        decode_dns_name(pkt, pkt_len, off);  // skip name
        off += 4;  // skip QTYPE + QCLASS
    }

    DiscoveredService svc;
    uint16_t total_rr = ancount + arcount;

    for (uint16_t i = 0; i < total_rr && off + 10 < pkt_len; i++) {
        // Skip additional section header offset
        if (i == ancount) {
            // Additional section starts — no special handling needed,
            // just keep parsing records
        }

        std::string rr_name = decode_dns_name(pkt, pkt_len, off);
        if (off + 10 > pkt_len) break;

        uint16_t rtype = (pkt[off] << 8) | pkt[off+1];
        off += 2;
        off += 2;  // class
        off += 4;  // TTL
        uint16_t rdlength = (pkt[off] << 8) | pkt[off+1];
        off += 2;
        size_t rdata_start = off;

        if (rtype == 33 && rdlength >= 6) {
            // SRV record: priority(2) + weight(2) + port(2) + target
            off += 4;  // skip priority + weight
            svc.port = (pkt[off] << 8) | pkt[off+1];
            off += 2;
            std::string target = decode_dns_name(pkt, pkt_len, off);
            // Strip ".local" suffix for hostname
            if (target.size() > 6 && target.substr(target.size()-6) == ".local") {
                svc.hostname = target.substr(0, target.size()-6);
            } else {
                svc.hostname = target;
            }
        } else if (rtype == 16 && rdlength > 0) {
            // TXT record: one or more length-prefixed strings
            size_t txt_off = rdata_start;
            size_t txt_end = rdata_start + rdlength;
            while (txt_off < txt_end && txt_off < pkt_len) {
                uint8_t txt_len = pkt[txt_off++];
                if (txt_off + txt_len > txt_end) break;
                svc.txt.emplace_back(reinterpret_cast<const char*>(pkt + txt_off), txt_len);
                txt_off += txt_len;
            }
        } else if (rtype == 1 && rdlength == 4) {
            // A record: 4-byte IPv4
            char ip_buf[32];
            snprintf(ip_buf, sizeof(ip_buf), "%d.%d.%d.%d",
                     pkt[rdata_start], pkt[rdata_start+1],
                     pkt[rdata_start+2], pkt[rdata_start+3]);
            svc.ip = ip_buf;
        }

        off = rdata_start + rdlength;
    }

    if (!svc.hostname.empty() && !svc.ip.empty()) {
        result.push_back(svc);
    }

    return result;
}
```

**Step 5: Run tests to verify they pass**

```bash
cd /d/Llamaste && bash scripts/host-test.sh net
```

Expected: All net tests PASS including new `build_ptr_query` and `parse_service_response`.

**Step 6: Commit**

```bash
git add src/llamaste/net_mdns.h src/llamaste/net_mdns.cpp tests/test_net.cpp
git commit -m "feat: add mDNS service discovery (PTR query + response parser)"
```

---

## Task 3: Add `discover_services()` Method to MdnsResponder

**Files:**
- Modify: `src/llamaste/net_mdns.h`
- Modify: `src/llamaste/net_mdns.cpp`

**Context:** Now that we can build PTR query packets and parse responses, add a high-level `discover_services()` method that sends the query on the multicast socket and collects responses for a timeout period.

**Step 1: Add declaration to `net_mdns.h`**

Add after the `advertise_service()` declaration:

```cpp
    // Discover services of a given type on the LAN.
    // Sends an mDNS PTR query and collects responses for timeout_ms.
    // service_type: e.g. "_llama-rpc._tcp" (no ".local")
    // Returns discovered services.
    std::vector<DiscoveredService> discover_services(
        const std::string& service_type, int timeout_ms = 2000);
```

**Step 2: Implement in `net_mdns.cpp`**

```cpp
std::vector<MdnsResponder::DiscoveredService> MdnsResponder::discover_services(
        const std::string& service_type, int timeout_ms) {
    std::vector<DiscoveredService> results;
    if (sock_ < 0) return results;

    std::string fqdn = service_type + ".local";
    auto query = build_ptr_query(fqdn);

    // Send query twice (UDP loss tolerance)
    send_multicast(query);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_multicast(query);

    // Collect responses
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    uint8_t buf[4096];

    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd = {sock_, POLLIN, 0};
        int remaining_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining_ms <= 0) break;

        int ret = poll(&pfd, 1, std::min(remaining_ms, 500));
        if (ret > 0 && (pfd.revents & POLLIN)) {
            struct sockaddr_in sender;
            socklen_t slen = sizeof(sender);
            ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&sender, &slen);
            if (n > 12) {
                // Only parse responses (QR=1)
                if (buf[2] & 0x80) {
                    auto peers = parse_service_responses(buf, (size_t)n);
                    for (auto& p : peers) {
                        // Deduplicate by IP
                        bool dup = false;
                        for (auto& existing : results) {
                            if (existing.ip == p.ip && existing.port == p.port) {
                                dup = true;
                                break;
                            }
                        }
                        if (!dup) results.push_back(std::move(p));
                    }
                }
            }
        }
    }

    return results;
}
```

**Step 3: Rebuild and run tests**

```bash
cd /d/Llamaste && bash scripts/host-test.sh net
```

Expected: All net tests PASS (discover_services not unit-tested directly — requires network).

**Step 4: Commit**

```bash
git add src/llamaste/net_mdns.h src/llamaste/net_mdns.cpp
git commit -m "feat: add discover_services() for LAN peer discovery via mDNS"
```

---

## Task 4: Cluster Manager — Data Structures and Election Algorithm

**Files:**
- Create: `src/llamaste/cluster.h`
- Create: `src/llamaste/cluster.cpp`
- Create: `tests/test_cluster.cpp`

**Context:** The cluster manager tracks peers, runs elections, and manages the coordinator/worker state machine. This task builds the core data structures and election logic (no networking yet).

**Step 1: Write failing tests in `tests/test_cluster.cpp`**

```cpp
// tests/test_cluster.cpp -- Cluster manager unit tests
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "cluster.h"

static void test_election_score() {
    PeerInfo a;
    a.hostname = "node-a"; a.ip = "192.168.1.10";
    a.ram_mb = 16384; a.cpu_cores = 4;

    PeerInfo b;
    b.hostname = "node-b"; b.ip = "192.168.1.20";
    b.ram_mb = 8192; b.cpu_cores = 8;

    // node-a: (16384/1024)*10 + 4 = 164
    // node-b: (8192/1024)*10 + 8  = 88
    assert(election_score(a) == 164);
    assert(election_score(b) == 88);
    printf("  PASS: election_score\n");
}

static void test_election_winner() {
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 8192; self.cpu_cores = 4;

    PeerInfo peer;
    peer.hostname = "peer"; peer.ip = "192.168.1.20";
    peer.ram_mb = 16384; peer.cpu_cores = 4;

    std::vector<PeerInfo> peers = {peer};
    auto winner = elect_coordinator(self, peers);
    // peer has more RAM, should win
    assert(winner.ip == "192.168.1.20");
    printf("  PASS: election_winner (peer wins)\n");
}

static void test_election_tiebreak() {
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.20";
    self.ram_mb = 8192; self.cpu_cores = 4;

    PeerInfo peer;
    peer.hostname = "peer"; peer.ip = "192.168.1.10";
    peer.ram_mb = 8192; peer.cpu_cores = 4;

    std::vector<PeerInfo> peers = {peer};
    auto winner = elect_coordinator(self, peers);
    // Same score, lowest IP wins: 192.168.1.10
    assert(winner.ip == "192.168.1.10");
    printf("  PASS: election_tiebreak (lowest IP wins)\n");
}

static void test_election_standalone() {
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 8192; self.cpu_cores = 4;

    std::vector<PeerInfo> peers;  // no peers
    auto winner = elect_coordinator(self, peers);
    assert(winner.ip == self.ip);
    printf("  PASS: election_standalone (self wins)\n");
}

static void test_cluster_state_transitions() {
    ClusterManager mgr;
    assert(mgr.state() == ClusterState::STANDALONE);
    assert(mgr.role_name() == "standalone");

    // Adding a peer and running election where self wins
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 16384; self.cpu_cores = 4;
    mgr.set_self_info(self);

    PeerInfo peer;
    peer.hostname = "peer"; peer.ip = "192.168.1.20";
    peer.ram_mb = 8192; peer.cpu_cores = 2;
    mgr.add_peer(peer);

    mgr.run_election();
    assert(mgr.state() == ClusterState::COORDINATOR);
    assert(mgr.role_name() == "coordinator");
    assert(mgr.peer_count() == 1);
    printf("  PASS: cluster_state_transitions\n");
}

static void test_cluster_state_worker() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.20";
    self.ram_mb = 4096; self.cpu_cores = 2;
    mgr.set_self_info(self);

    PeerInfo peer;
    peer.hostname = "big-node"; peer.ip = "192.168.1.10";
    peer.ram_mb = 32768; peer.cpu_cores = 8;
    mgr.add_peer(peer);

    mgr.run_election();
    assert(mgr.state() == ClusterState::WORKER);
    assert(mgr.role_name() == "worker");
    printf("  PASS: cluster_state_worker\n");
}

static void test_peer_expiry() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 8192; self.cpu_cores = 4;
    mgr.set_self_info(self);

    PeerInfo peer;
    peer.hostname = "gone"; peer.ip = "192.168.1.99";
    peer.ram_mb = 4096; peer.cpu_cores = 2;
    mgr.add_peer(peer);
    assert(mgr.peer_count() == 1);

    // Expire peers older than 0 seconds (force expire)
    mgr.expire_peers(0);
    assert(mgr.peer_count() == 0);
    printf("  PASS: peer_expiry\n");
}

static void test_rpc_endpoint_list() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10"; self.rpc_port = 50052;
    self.ram_mb = 16384; self.cpu_cores = 4;
    mgr.set_self_info(self);

    PeerInfo p1;
    p1.hostname = "n1"; p1.ip = "192.168.1.11"; p1.rpc_port = 50052;
    p1.ram_mb = 8192; p1.cpu_cores = 4;
    mgr.add_peer(p1);

    PeerInfo p2;
    p2.hostname = "n2"; p2.ip = "192.168.1.12"; p2.rpc_port = 50052;
    p2.ram_mb = 8192; p2.cpu_cores = 4;
    mgr.add_peer(p2);

    auto endpoints = mgr.rpc_endpoint_list();
    // Should include self + all peers
    assert(endpoints == "192.168.1.10:50052,192.168.1.11:50052,192.168.1.12:50052");
    printf("  PASS: rpc_endpoint_list\n");
}

static void run_cluster_tests() {
    printf("--- Cluster tests ---\n");
    test_election_score();
    test_election_winner();
    test_election_tiebreak();
    test_election_standalone();
    test_cluster_state_transitions();
    test_cluster_state_worker();
    test_peer_expiry();
    test_rpc_endpoint_list();
}

int main() {
    run_cluster_tests();
    printf("\nAll cluster tests passed!\n");
    return 0;
}
```

**Step 2: Create `src/llamaste/cluster.h`**

```cpp
#pragma once
// cluster.h -- Mesh clustering for distributed inference
//
// Manages peer discovery, coordinator election, and llama-rpc-server
// lifecycle for multi-node inference via llama.cpp's RPC backend.

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <functional>

struct PeerInfo {
    std::string hostname;
    std::string ip;
    uint16_t rpc_port = 50052;
    uint32_t ram_mb = 0;
    uint32_t cpu_cores = 0;
    std::string model;  // currently loaded model or "none"
    std::chrono::steady_clock::time_point last_seen;
};

enum class ClusterState {
    STANDALONE,   // no peers discovered
    DISCOVERING,  // looking for peers
    COORDINATOR,  // elected coordinator, runs llama-server --rpc
    WORKER        // worker node, runs llama-rpc-server only
};

// Election score: higher = more likely to be coordinator
int election_score(const PeerInfo& p);

// Elect coordinator from self + peers. Returns the winner.
PeerInfo elect_coordinator(const PeerInfo& self,
                           const std::vector<PeerInfo>& peers);

class ClusterManager {
public:
    ClusterManager() = default;

    // Set this node's info (must be called before run_election)
    void set_self_info(const PeerInfo& self);

    // Add or update a peer (thread-safe)
    void add_peer(const PeerInfo& peer);

    // Remove peers not seen within timeout_seconds
    void expire_peers(int timeout_seconds);

    // Run coordinator election based on current peers
    void run_election();

    // Current cluster state
    ClusterState state() const;
    std::string role_name() const;

    // Number of known peers (excluding self)
    size_t peer_count() const;

    // Get all peers (copy, thread-safe)
    std::vector<PeerInfo> peers() const;

    // Get self info
    PeerInfo self_info() const;

    // Build --rpc argument string: "ip1:port,ip2:port,..."
    // Includes self + all peers
    std::string rpc_endpoint_list() const;

    // Get coordinator info (valid only after election)
    PeerInfo coordinator() const;
    bool is_coordinator() const;

    // Callback: called when coordinator changes (for llama-server restart)
    void set_topology_change_callback(std::function<void()> cb);

private:
    mutable std::mutex mu_;
    PeerInfo self_;
    std::vector<PeerInfo> peers_;
    ClusterState state_ = ClusterState::STANDALONE;
    PeerInfo coordinator_;
    std::function<void()> topology_cb_;
};
```

**Step 3: Create `src/llamaste/cluster.cpp`**

```cpp
#include "cluster.h"
#include <algorithm>
#include <cstdio>

int election_score(const PeerInfo& p) {
    return (int)(p.ram_mb / 1024) * 10 + (int)p.cpu_cores;
}

PeerInfo elect_coordinator(const PeerInfo& self,
                           const std::vector<PeerInfo>& peers) {
    PeerInfo best = self;
    int best_score = election_score(self);

    for (const auto& p : peers) {
        int s = election_score(p);
        if (s > best_score || (s == best_score && p.ip < best.ip)) {
            best = p;
            best_score = s;
        }
    }
    return best;
}

void ClusterManager::set_self_info(const PeerInfo& self) {
    std::lock_guard<std::mutex> lock(mu_);
    self_ = self;
}

void ClusterManager::add_peer(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lock(mu_);
    // Update existing or add new
    for (auto& p : peers_) {
        if (p.ip == peer.ip && p.rpc_port == peer.rpc_port) {
            p = peer;
            p.last_seen = std::chrono::steady_clock::now();
            return;
        }
    }
    PeerInfo p = peer;
    p.last_seen = std::chrono::steady_clock::now();
    peers_.push_back(p);
}

void ClusterManager::expire_peers(int timeout_seconds) {
    std::lock_guard<std::mutex> lock(mu_);
    auto now = std::chrono::steady_clock::now();
    bool changed = false;
    peers_.erase(
        std::remove_if(peers_.begin(), peers_.end(),
            [&](const PeerInfo& p) {
                auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    now - p.last_seen).count();
                if (age > timeout_seconds) {
                    changed = true;
                    return true;
                }
                return false;
            }),
        peers_.end());

    if (changed && topology_cb_) {
        topology_cb_();
    }
}

void ClusterManager::run_election() {
    std::lock_guard<std::mutex> lock(mu_);
    PeerInfo winner = elect_coordinator(self_, peers_);
    coordinator_ = winner;

    ClusterState old_state = state_;
    if (peers_.empty()) {
        state_ = ClusterState::STANDALONE;
    } else if (winner.ip == self_.ip) {
        state_ = ClusterState::COORDINATOR;
    } else {
        state_ = ClusterState::WORKER;
    }

    if (state_ != old_state && topology_cb_) {
        topology_cb_();
    }
}

ClusterState ClusterManager::state() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

std::string ClusterManager::role_name() const {
    switch (state()) {
        case ClusterState::STANDALONE:  return "standalone";
        case ClusterState::DISCOVERING: return "discovering";
        case ClusterState::COORDINATOR: return "coordinator";
        case ClusterState::WORKER:      return "worker";
    }
    return "unknown";
}

size_t ClusterManager::peer_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return peers_.size();
}

std::vector<PeerInfo> ClusterManager::peers() const {
    std::lock_guard<std::mutex> lock(mu_);
    return peers_;
}

PeerInfo ClusterManager::self_info() const {
    std::lock_guard<std::mutex> lock(mu_);
    return self_;
}

std::string ClusterManager::rpc_endpoint_list() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::string result;
    // Self first
    result += self_.ip + ":" + std::to_string(self_.rpc_port);
    for (const auto& p : peers_) {
        result += "," + p.ip + ":" + std::to_string(p.rpc_port);
    }
    return result;
}

PeerInfo ClusterManager::coordinator() const {
    std::lock_guard<std::mutex> lock(mu_);
    return coordinator_;
}

bool ClusterManager::is_coordinator() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_ == ClusterState::COORDINATOR || state_ == ClusterState::STANDALONE;
}

void ClusterManager::set_topology_change_callback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(mu_);
    topology_cb_ = std::move(cb);
}
```

**Step 4: Add test compilation to `scripts/host-test.sh`**

Add a new test suite section (after the audio test section):

```bash
# Suite 11: Cluster tests
echo ""
echo "=== Suite 11: Cluster ==="
g++ -std=c++17 -I"${SRC}" -pthread \
    "${TESTS}/test_cluster.cpp" \
    "${SRC}/cluster.cpp" \
    -o "${BUILD_DIR}/test_cluster" 2>&1
if [ $? -eq 0 ]; then
    "${BUILD_DIR}/test_cluster"
    CLUSTER_RESULT=$?
else
    echo "  FAIL: compilation failed"
    CLUSTER_RESULT=1
fi
```

Update the results summary to include cluster results.

**Step 5: Run tests**

```bash
cd /d/Llamaste && bash scripts/host-test.sh cluster
```

Expected: All 8 cluster tests PASS.

**Step 6: Commit**

```bash
git add src/llamaste/cluster.h src/llamaste/cluster.cpp tests/test_cluster.cpp scripts/host-test.sh
git commit -m "feat: cluster manager with election algorithm and peer tracking"
```

---

## Task 5: Cluster Tools (`tools_cluster.cpp`)

**Files:**
- Create: `src/llamaste/tools_cluster.cpp`
- Modify: `src/llamaste/tools.h` (add `register_cluster_tools` declaration)
- Modify: `src/llamaste/CMakeLists.txt` (add source files)

**Context:** Follow the existing tool registration pattern (see `tools_model.cpp`). Three tools: `cluster.status`, `cluster.peers`, `cluster.reload`.

**Step 1: Add declaration to `tools.h`**

After the `register_audio_tools` declaration, add:

```cpp
class ClusterManager;  // forward declaration
void register_cluster_tools(ToolRegistry& reg, ClusterManager& cluster);
```

**Step 2: Create `src/llamaste/tools_cluster.cpp`**

```cpp
#include "tools.h"
#include "cluster.h"
#include "json.hpp"

using json = nlohmann::json;

static ClusterManager* g_cluster = nullptr;

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

static std::string handle_cluster_status(const std::string& args_json) {
    (void)args_json;
    if (!g_cluster) return json_error("cluster not initialized");

    json result;
    result["role"] = g_cluster->role_name();
    result["peer_count"] = g_cluster->peer_count();

    auto self = g_cluster->self_info();
    result["self"] = {
        {"hostname", self.hostname},
        {"ip", self.ip},
        {"rpc_port", self.rpc_port},
        {"ram_mb", self.ram_mb},
        {"cpu_cores", self.cpu_cores}
    };

    if (g_cluster->peer_count() > 0) {
        auto coord = g_cluster->coordinator();
        result["coordinator"] = {
            {"hostname", coord.hostname},
            {"ip", coord.ip}
        };
        result["rpc_endpoints"] = g_cluster->rpc_endpoint_list();

        // Calculate total pooled RAM
        uint64_t total_ram = self.ram_mb;
        for (const auto& p : g_cluster->peers()) {
            total_ram += p.ram_mb;
        }
        result["total_ram_mb"] = total_ram;
    }

    return result.dump();
}

static std::string handle_cluster_peers(const std::string& args_json) {
    (void)args_json;
    if (!g_cluster) return json_error("cluster not initialized");

    json result;
    json peer_list = json::array();

    for (const auto& p : g_cluster->peers()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - p.last_seen).count();
        peer_list.push_back({
            {"hostname", p.hostname},
            {"ip", p.ip},
            {"rpc_port", p.rpc_port},
            {"ram_mb", p.ram_mb},
            {"cpu_cores", p.cpu_cores},
            {"model", p.model},
            {"last_seen_seconds_ago", age}
        });
    }

    result["peers"] = peer_list;
    result["count"] = peer_list.size();
    return result.dump();
}

static std::string handle_cluster_reload(const std::string& args_json) {
    (void)args_json;
    if (!g_cluster) return json_error("cluster not initialized");

    g_cluster->run_election();
    json result;
    result["success"] = true;
    result["new_role"] = g_cluster->role_name();
    result["coordinator"] = g_cluster->coordinator().hostname;
    return result.dump();
}

void register_cluster_tools(ToolRegistry& reg, ClusterManager& cluster) {
    g_cluster = &cluster;

    reg.register_tool(ToolDef{
        .name = "cluster.status",
        .description = "Get mesh cluster status: role (standalone/coordinator/worker), "
                       "peer count, coordinator info, total pooled RAM, RPC endpoints.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_cluster_status
    });

    reg.register_tool(ToolDef{
        .name = "cluster.peers",
        .description = "List all discovered cluster peers with hostname, IP, RAM, "
                       "CPU cores, loaded model, and time since last seen.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_cluster_peers
    });

    reg.register_tool(ToolDef{
        .name = "cluster.reload",
        .description = "Force a cluster re-election and topology refresh. "
                       "Use after adding/removing nodes or changing configuration.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_cluster_reload
    });
}
```

**Step 3: Add source files to `CMakeLists.txt`**

In the `LLAMASTE_SOURCES` list, add `cluster.cpp` and `tools_cluster.cpp`:

```cmake
set(LLAMASTE_SOURCES
    main.cpp supervisor.cpp init.cpp hwdetect.cpp
    child_main.cpp tools.cpp
    tools_fs.cpp tools_process.cpp tools_network.cpp
    tools_system.cpp tools_config.cpp tools_model.cpp
    tools_install.cpp tools_schedule.cpp tools_auth.cpp
    tools_model_download.cpp tools_audio.cpp tools_cluster.cpp
    voice.cpp mcp_server.cpp agent.cpp prompt_builder.cpp
    net_mdns.cpp scheduler.cpp cluster.cpp bcrypt.cpp auth.cpp
)
```

**Step 4: Run host tests (should still compile)**

```bash
cd /d/Llamaste && bash scripts/host-test.sh cluster
```

Expected: All cluster tests PASS.

**Step 5: Commit**

```bash
git add src/llamaste/tools_cluster.cpp src/llamaste/tools.h src/llamaste/CMakeLists.txt
git commit -m "feat: cluster tools (status, peers, reload)"
```

---

## Task 6: llama-rpc-server Lifecycle in child_main.cpp

**Files:**
- Modify: `src/llamaste/child_main.cpp`

**Context:** We need to fork/exec `llama-rpc-server` on boot (port 50052), manage its PID, and restart it on crash. Pattern follows the existing `spawn_llama_server()` function. The RPC server runs alongside (not instead of) llama-server.

**Step 1: Add RPC server globals after existing llama-server globals (~line 110)**

```cpp
// llama-rpc-server process state (mesh clustering)
#ifndef _WIN32
static std::atomic<pid_t> g_rpc_pid{0};
#endif
static int g_rpc_port = 50052;
```

**Step 2: Add spawn function after `spawn_llama_server()`**

```cpp
#ifndef _WIN32
static bool spawn_rpc_server() {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[child] Failed to fork llama-rpc-server: %s\n", strerror(errno));
        return false;
    }

    if (pid == 0) {
        std::string port_str = std::to_string(g_rpc_port);
        execl("/opt/llamaste/llama-rpc-server", "llama-rpc-server",
              "--host", "0.0.0.0",
              "--port", port_str.c_str(),
              (char*)nullptr);
        fprintf(stderr, "[child] execl llama-rpc-server failed: %s\n", strerror(errno));
        _exit(127);
    }

    g_rpc_pid.store(pid);
    fprintf(stderr, "[child] llama-rpc-server spawned with PID %d on port %d\n",
            pid, g_rpc_port);
    return true;
}
#endif
```

**Step 3: Start RPC server early in `child_main()`, before llama-server spawn**

After the inference backend setup block (around the model loading section), add:

```cpp
    // Start llama-rpc-server for mesh clustering (port 50052)
    // Runs on all nodes — even standalone, so it's ready when peers discover us
#ifndef _WIN32
    if (access("/opt/llamaste/llama-rpc-server", X_OK) == 0) {
        if (spawn_rpc_server()) {
            fprintf(stderr, "[child] RPC server ready for mesh clustering\n");
        } else {
            fprintf(stderr, "[child] RPC server failed to start — mesh clustering unavailable\n");
        }
    } else {
        fprintf(stderr, "[child] llama-rpc-server not found — mesh clustering unavailable\n");
    }
#endif
```

**Step 4: Add RPC server cleanup in shutdown handler**

In the existing signal/cleanup section, add before the `kill(g_llama_pid, ...)` block:

```cpp
    // Stop llama-rpc-server
    pid_t rpc_pid = g_rpc_pid.load();
    if (rpc_pid > 0) {
        fprintf(stderr, "[child] Stopping llama-rpc-server (PID %d)\n", rpc_pid);
        kill(rpc_pid, SIGTERM);
        waitpid(rpc_pid, nullptr, 0);
        g_rpc_pid.store(0);
    }
```

**Step 5: Modify `spawn_llama_server()` to accept --rpc flag**

Update the function signature and exec call to optionally include `--rpc`:

```cpp
static bool spawn_llama_server(const std::string& model_path, int cpu_cores,
                                int free_ram_mb, const std::string& rpc_endpoints = "") {
    // ... existing code ...

    if (pid == 0) {
        // Build args vector for variable-length command
        std::vector<const char*> args;
        args.push_back("llama-server");
        args.push_back("-m"); args.push_back(model_path.c_str());
        args.push_back("--host"); args.push_back("127.0.0.1");
        args.push_back("--port"); args.push_back(port_str.c_str());
        args.push_back("--no-webui");
        args.push_back("-c"); args.push_back(c_str.c_str());
        args.push_back("-t"); args.push_back(t_str.c_str());
        args.push_back("-tb"); args.push_back(tb_str.c_str());
        args.push_back("--mlock");
        args.push_back("-fa");
        args.push_back("--jinja");
        args.push_back("--chat-template"); args.push_back("chatml");
        args.push_back("--log-disable");

        if (!rpc_endpoints.empty()) {
            args.push_back("--rpc");
            args.push_back(rpc_endpoints.c_str());
            fprintf(stderr, "[child] Using RPC endpoints: %s\n", rpc_endpoints.c_str());
        }

        args.push_back(nullptr);
        execv("/opt/llamaste/llama-server", const_cast<char**>(args.data()));
        fprintf(stderr, "[child] execv llama-server failed: %s\n", strerror(errno));
        _exit(127);
    }
    // ... rest unchanged ...
}
```

**Step 6: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: llama-rpc-server lifecycle and --rpc flag for distributed inference"
```

---

## Task 7: Cluster Integration in child_main.cpp

**Files:**
- Modify: `src/llamaste/child_main.cpp`

**Context:** Wire the ClusterManager into the main loop: set self info from hardware detection, discover peers via mDNS, run election, advertise service, start heartbeat/expiry thread.

**Step 1: Add includes and global**

At the top of child_main.cpp (with other includes):
```cpp
#include "cluster.h"
```

Add global (near other globals):
```cpp
static ClusterManager g_cluster;
```

**Step 2: Initialize cluster after mDNS starts**

After the existing `mdns.advertise_service("_mcp._tcp", ...)` call, add:

```cpp
    // --- Mesh clustering ---
    // Advertise RPC service for peer discovery
    {
        std::vector<std::string> rpc_txt;
        rpc_txt.push_back("ram=" + std::to_string(g_hwinfo.ram_mb));
        rpc_txt.push_back("cores=" + std::to_string(g_hwinfo.cpu_cores));
        rpc_txt.push_back("rpc_port=" + std::to_string(g_rpc_port));
        rpc_txt.push_back("model=" + (g_model_name.empty() ? "none" : g_model_name));

        mdns.advertise_service("_llama-rpc._tcp", (uint16_t)g_rpc_port, rpc_txt);

        // Set self info for election
        PeerInfo self;
        self.hostname = config.hostname.empty() ? "llamaste" : config.hostname;
        self.ip = MdnsResponder::get_local_ip();
        self.rpc_port = (uint16_t)g_rpc_port;
        self.ram_mb = g_hwinfo.ram_mb;
        self.cpu_cores = g_hwinfo.cpu_cores;
        self.model = g_model_name.empty() ? "none" : g_model_name;
        g_cluster.set_self_info(self);

        // Discover peers (2-second window)
        auto discovered = mdns.discover_services("_llama-rpc._tcp", 2000);
        for (const auto& d : discovered) {
            if (d.ip == self.ip) continue;  // skip self
            PeerInfo peer;
            peer.hostname = d.hostname;
            peer.ip = d.ip;
            peer.rpc_port = d.port;
            // Parse TXT records
            for (const auto& t : d.txt) {
                auto eq = t.find('=');
                if (eq == std::string::npos) continue;
                std::string key = t.substr(0, eq);
                std::string val = t.substr(eq + 1);
                if (key == "ram") peer.ram_mb = (uint32_t)std::stoul(val);
                else if (key == "cores") peer.cpu_cores = (uint32_t)std::stoul(val);
                else if (key == "model") peer.model = val;
            }
            g_cluster.add_peer(peer);
            fprintf(stderr, "[cluster] Discovered peer: %s (%s) ram=%uMB cores=%u\n",
                    peer.hostname.c_str(), peer.ip.c_str(), peer.ram_mb, peer.cpu_cores);
        }

        // Run election
        g_cluster.run_election();
        fprintf(stderr, "[cluster] Role: %s | Peers: %zu\n",
                g_cluster.role_name().c_str(), g_cluster.peer_count());

        if (g_cluster.is_coordinator() && g_cluster.peer_count() > 0) {
            fprintf(stderr, "[cluster] Coordinator — RPC endpoints: %s\n",
                    g_cluster.rpc_endpoint_list().c_str());
        }
    }
```

**Step 3: Register cluster tools**

In the tool registration section (where `register_all_tools` and other register calls are):

```cpp
    register_cluster_tools(g_tools, g_cluster);
```

**Step 4: Add heartbeat/expiry thread**

After the cluster initialization block, start a background thread:

```cpp
    // Cluster heartbeat: re-announce service and expire stale peers every 30s
    std::thread cluster_heartbeat_thread([&mdns]() {
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            if (!g_running.load()) break;

            // Re-announce our RPC service
            std::vector<std::string> rpc_txt;
            rpc_txt.push_back("ram=" + std::to_string(g_hwinfo.ram_mb));
            rpc_txt.push_back("cores=" + std::to_string(g_hwinfo.cpu_cores));
            rpc_txt.push_back("rpc_port=" + std::to_string(g_rpc_port));
            rpc_txt.push_back("model=" + (g_model_name.empty() ? "none" : g_model_name));
            mdns.advertise_service("_llama-rpc._tcp", (uint16_t)g_rpc_port, rpc_txt);

            // Expire peers not seen in 90 seconds
            g_cluster.expire_peers(90);

            // Re-discover peers
            auto discovered = mdns.discover_services("_llama-rpc._tcp", 1000);
            auto self = g_cluster.self_info();
            for (const auto& d : discovered) {
                if (d.ip == self.ip) continue;
                PeerInfo peer;
                peer.hostname = d.hostname;
                peer.ip = d.ip;
                peer.rpc_port = d.port;
                for (const auto& t : d.txt) {
                    auto eq = t.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = t.substr(0, eq);
                    std::string val = t.substr(eq + 1);
                    if (key == "ram") peer.ram_mb = (uint32_t)std::stoul(val);
                    else if (key == "cores") peer.cpu_cores = (uint32_t)std::stoul(val);
                    else if (key == "model") peer.model = val;
                }
                g_cluster.add_peer(peer);
            }

            // Re-run election (topology may have changed)
            g_cluster.run_election();
        }
    });
    cluster_heartbeat_thread.detach();
```

**Step 5: Add cluster status to `/llamaste/system` endpoint**

In the `gather_system_info()` function, add cluster info:

```cpp
    // Cluster info
    json cluster;
    cluster["role"] = g_cluster.role_name();
    cluster["peer_count"] = g_cluster.peer_count();
    if (g_cluster.peer_count() > 0) {
        cluster["coordinator"] = g_cluster.coordinator().hostname;
        cluster["rpc_endpoints"] = g_cluster.rpc_endpoint_list();
        uint64_t total_ram = g_cluster.self_info().ram_mb;
        for (const auto& p : g_cluster.peers()) total_ram += p.ram_mb;
        cluster["total_ram_mb"] = total_ram;
    }
    info["cluster"] = cluster;
```

**Step 6: Add `/llamaste/cluster/status` HTTP endpoint**

In the route registration section of child_main(), add:

```cpp
    svr.Get("/llamaste/cluster/status", [&](const httplib::Request& req,
                                             httplib::Response& res) {
        if (!require_auth(req, res)) return;
        res.set_content(g_tools.dispatch("cluster.status", "{}"),
                        "application/json");
    });
```

**Step 7: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: cluster integration — discovery, election, heartbeat, HTTP endpoint"
```

---

## Task 8: Coordinator Model Reload with --rpc

**Files:**
- Modify: `src/llamaste/child_main.cpp`

**Context:** When the coordinator loads a model (via model download or manual load), it should pass the `--rpc` flag to llama-server with all worker endpoints. The topology change callback should trigger a model reload when peers change.

**Step 1: Wire topology change callback**

After `g_cluster.run_election()`, set the callback:

```cpp
    g_cluster.set_topology_change_callback([&]() {
        if (!g_cluster.is_coordinator()) return;
        if (!g_model_loaded.load()) return;

        fprintf(stderr, "[cluster] Topology changed — reloading llama-server with new RPC endpoints\n");
        // Kill existing llama-server
        pid_t pid = g_llama_pid.load();
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
            g_llama_pid.store(0);
            g_model_loaded.store(false);
            g_inference_fn = stub_inference;
        }

        // Respawn with updated endpoints
        std::string rpc = g_cluster.rpc_endpoint_list();
        std::string model_path = "/data/models/" + g_model_name;
        if (spawn_llama_server(model_path, g_hwinfo.cpu_cores,
                               (int)g_hwinfo.ram_mb, rpc)) {
            if (wait_for_llama_server(120)) {
                g_model_loaded.store(true);
                g_inference_fn = llama_inference;
                fprintf(stderr, "[cluster] llama-server restarted with RPC endpoints: %s\n",
                        rpc.c_str());
            }
        }
    });
```

**Step 2: Update model load handler to use RPC endpoints**

Find the existing call to `spawn_llama_server(model_path, cpu_cores, free_ram_mb)` in the model download/load handler. Add the RPC endpoint list:

```cpp
    std::string rpc = g_cluster.is_coordinator() && g_cluster.peer_count() > 0
                      ? g_cluster.rpc_endpoint_list() : "";
    if (spawn_llama_server(model_path, g_hwinfo.cpu_cores,
                           (int)g_hwinfo.ram_mb, rpc)) {
```

**Step 3: Commit**

```bash
git add src/llamaste/child_main.cpp
git commit -m "feat: coordinator passes --rpc endpoints when loading models"
```

---

## Task 9: Web UI Cluster Dashboard Card

**Files:**
- Modify: `src/llamaste/web/index.html`
- Modify: `src/llamaste/web/system.js`

**Context:** Add a "Cluster" card to the system panel showing role, peers, and pooled RAM. Follows existing card pattern (see MCP card, Scheduled Tasks card).

**Step 1: Add HTML card to `index.html`**

After the MCP card and before the Power card, add:

```html
          <div class="system-card" id="cluster-card">
            <h3>Cluster</h3>
            <div class="dash-row">
              <span class="label">Role</span>
              <span id="cluster-role" class="value">standalone</span>
            </div>
            <div class="dash-row">
              <span class="label">Peers</span>
              <span id="cluster-peers" class="value">0</span>
            </div>
            <div class="dash-row" id="cluster-ram-row" style="display:none">
              <span class="label">Pooled RAM</span>
              <span id="cluster-total-ram" class="value">-</span>
            </div>
            <div id="cluster-peer-list" style="margin-top:8px"></div>
          </div>
```

**Step 2: Add cluster polling to `system.js`**

Add at the end of the file (or in the existing polling section):

```javascript
// --- Cluster status ---
function updateClusterCard() {
    fetch('/llamaste/cluster/status', {credentials:'include'})
        .then(function(r) { return r.json(); })
        .then(function(data) {
            var roleEl = document.getElementById('cluster-role');
            var peersEl = document.getElementById('cluster-peers');
            var ramRow = document.getElementById('cluster-ram-row');
            var ramEl = document.getElementById('cluster-total-ram');
            var listEl = document.getElementById('cluster-peer-list');

            if (!roleEl) return;

            roleEl.textContent = data.role || 'standalone';
            peersEl.textContent = String(data.peer_count || 0);

            if (data.total_ram_mb) {
                ramRow.style.display = '';
                ramEl.textContent = (data.total_ram_mb / 1024).toFixed(1) + ' GB';
            } else {
                ramRow.style.display = 'none';
            }

            // Role badge color
            roleEl.className = 'value';
            if (data.role === 'coordinator') roleEl.style.color = '#4CAF50';
            else if (data.role === 'worker') roleEl.style.color = '#2196F3';
            else roleEl.style.color = '';
        })
        .catch(function() {});
}

// Poll cluster status every 10 seconds
setInterval(updateClusterCard, 10000);
updateClusterCard();
```

**Step 3: Commit**

```bash
git add src/llamaste/web/index.html src/llamaste/web/system.js
git commit -m "feat: cluster dashboard card showing role, peers, pooled RAM"
```

---

## Task 10: Build, Deploy, and Verify

**Files:** No new files — integration testing.

**Step 1: Full rebuild**

```bash
cd /root/llamaste-build/output && make llama-server-dirclean && make llama-server
make llamaste-dirclean && make llamaste && make
```

**Step 2: Verify binaries**

```bash
ls -la /root/llamaste-build/output/target/opt/llamaste/
# Should show: llamaste, llama-server, llama-rpc-server
```

**Step 3: Deploy to VDI and boot**

Follow the VDI deploy pattern (extract squashfs from image, convert VDI, copy, UUID fix, start VM).

**Step 4: Verify cluster status via HTTP**

```bash
curl -s -b cookies.txt http://localhost:8080/llamaste/cluster/status | python3 -m json.tool
```

Expected:
```json
{
    "role": "standalone",
    "peer_count": 0,
    "self": {
        "hostname": "llamaste",
        "ip": "10.0.2.15",
        "rpc_port": 50052,
        "ram_mb": 4096,
        "cpu_cores": 2
    }
}
```

**Step 5: Verify serial log**

```bash
grep -i "rpc\|cluster" serial.log
```

Expected: `llama-rpc-server spawned`, `Role: standalone`, `Peers: 0`.

**Step 6: Verify web UI**

Open `http://localhost:8080`, go to System tab — Cluster card should show "standalone", "0 peers".

**Step 7: Commit docs update**

```bash
git add SESSION-STATUS.md
git commit -m "docs: Phase 3 mesh clustering complete"
```

---

## Summary

| Task | Description | New Files | LOC (est) |
|------|-------------|-----------|-----------|
| 1 | Enable GGML_RPC in Buildroot | - | 5 |
| 2 | mDNS PTR query + response parser | - | 120 |
| 3 | discover_services() method | - | 50 |
| 4 | ClusterManager + election algorithm | cluster.h, cluster.cpp, test_cluster.cpp | 300 |
| 5 | Cluster tools (status, peers, reload) | tools_cluster.cpp | 120 |
| 6 | llama-rpc-server lifecycle | - | 60 |
| 7 | Cluster integration in child_main | - | 100 |
| 8 | Coordinator --rpc model reload | - | 40 |
| 9 | Web UI cluster card | - | 50 |
| 10 | Build, deploy, verify | - | 0 |
| **Total** | | **4 new files** | **~845 LOC** |

Test count: 8 new cluster tests + 2 new mDNS tests = **10 new tests** (total: ~148)
