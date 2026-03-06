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
