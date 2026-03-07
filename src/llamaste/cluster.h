#pragma once
// cluster.h -- Mesh clustering for distributed inference
//
// Manages peer discovery, coordinator election, llama-rpc-server
// lifecycle, and automatic model selection + layer distribution
// for multi-node inference via llama.cpp's RPC backend.

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

// Model tier for automatic selection based on cluster capacity
struct ModelTier {
    std::string name;         // e.g. "qwen2.5-0.5b-instruct"
    std::string gguf_file;    // e.g. "qwen2.5-0.5b-instruct-q4_k_m.gguf"
    uint32_t size_mb;         // GGUF file size in MB
    uint32_t min_ram_mb;      // Minimum usable cluster RAM needed
    uint32_t layers;          // Number of transformer layers
};

// Cluster capacity analysis result
struct ClusterCapacity {
    uint32_t total_ram_mb = 0;      // Raw total across all nodes (including self)
    uint32_t usable_ram_mb = 0;     // After 70% safety factor
    uint32_t total_cores = 0;       // Total CPU cores across cluster
    size_t node_count = 0;          // Number of nodes (self + peers)
    std::string recommended_model;  // Best model name for this capacity
    std::string recommended_gguf;   // Best GGUF filename
    std::string tensor_split;       // "1,2,4" format for --tensor-split
    bool upgrade_available = false; // True if bigger model could fit but isn't present
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

    // Fire the topology callback unconditionally (regardless of state change).
    // Used after a background model download completes to trigger llama-server
    // startup without waiting for a topology state transition.
    void fire_topology_callback();

    // --- Phase 5: Auto-offload ---

    // Analyze cluster capacity: sum RAM, compute tensor split, recommend model
    ClusterCapacity analyze_capacity(const std::string& model_dir = "/data/models") const;

    // Compute --tensor-split ratios based on per-node RAM
    // Returns "1,2,4" format string (or empty if single node)
    std::string compute_tensor_split() const;

    // Select best model from available files that fits cluster RAM
    // Returns empty ModelTier if nothing fits
    ModelTier select_model(const std::string& model_dir = "/data/models") const;

    // Get built-in model tier table (Qwen2.5-Instruct Q4_K_M)
    static const std::vector<ModelTier>& model_tiers();

    // Scan model directory for available GGUF files, match against tier table
    static std::vector<ModelTier> available_models(const std::string& model_dir);

private:
    mutable std::mutex mu_;
    PeerInfo self_;
    std::vector<PeerInfo> peers_;
    ClusterState state_ = ClusterState::STANDALONE;
    PeerInfo coordinator_;
    std::function<void()> topology_cb_;
};
