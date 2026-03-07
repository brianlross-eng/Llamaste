#include "cluster.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#endif

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
    bool should_notify = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        bool changed = false;
        peers_.erase(
            std::remove_if(peers_.begin(), peers_.end(),
                [&](const PeerInfo& p) {
                    auto age = std::chrono::duration_cast<std::chrono::seconds>(
                        now - p.last_seen).count();
                    if (age >= timeout_seconds) {
                        changed = true;
                        return true;
                    }
                    return false;
                }),
            peers_.end());
        should_notify = changed && topology_cb_;
    }
    // Call callback OUTSIDE lock to avoid blocking all cluster operations
    if (should_notify) {
        topology_cb_();
    }
}

void ClusterManager::run_election() {
    bool should_notify = false;
    {
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
        should_notify = (state_ != old_state) && topology_cb_;
    }
    // Call callback OUTSIDE lock to avoid blocking all cluster operations
    // (topology callback may restart llama-server, taking 120s+)
    if (should_notify) {
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

void ClusterManager::fire_topology_callback() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cb = topology_cb_;
    }
    if (cb) cb();
}

// ---------------------------------------------------------------------------
// Phase 5: Auto-offload — model tiers, capacity analysis, tensor split
// ---------------------------------------------------------------------------

// Built-in model tier table: Qwen2.5-Instruct Q4_K_M quantization
// Sorted ascending by size so select_model() picks the largest that fits.
static const std::vector<ModelTier> g_model_tiers = {
    {"qwen2.5-0.5b-instruct",  "qwen2.5-0.5b-instruct-q4_k_m.gguf",   400,  1024, 24},
    {"qwen2.5-1.5b-instruct",  "qwen2.5-1.5b-instruct-q4_k_m.gguf",  1100,  2048, 28},
    {"qwen2.5-3b-instruct",    "qwen2.5-3b-instruct-q4_k_m.gguf",    2000,  3072, 36},
    {"qwen2.5-7b-instruct",    "qwen2.5-7b-instruct-q4_k_m.gguf",    4400,  6144, 32},
    {"qwen2.5-14b-instruct",   "qwen2.5-14b-instruct-q4_k_m.gguf",   8500, 12288, 40},
    {"qwen2.5-32b-instruct",   "qwen2.5-32b-instruct-q4_k_m.gguf",  19000, 24576, 64},
    {"qwen2.5-72b-instruct",   "qwen2.5-72b-instruct-q4_k_m.gguf",  42000, 52224, 80},
};

const std::vector<ModelTier>& ClusterManager::model_tiers() {
    return g_model_tiers;
}

std::vector<ModelTier> ClusterManager::available_models(const std::string& model_dir) {
    std::vector<ModelTier> result;
#ifndef _WIN32
    DIR* dir = opendir(model_dir.c_str());
    if (!dir) return result;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type != DT_REG && entry->d_type != DT_UNKNOWN) continue;
        std::string fname = entry->d_name;
        // Match against tier table by GGUF filename
        for (const auto& tier : g_model_tiers) {
            if (fname == tier.gguf_file) {
                // Verify file actually exists and get real size
                std::string path = model_dir + "/" + fname;
                struct stat st;
                if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
                    ModelTier found = tier;
                    found.size_mb = (uint32_t)(st.st_size / (1024 * 1024));
                    result.push_back(found);
                }
                break;
            }
        }
        // Also match any .gguf file not in the tier table (user-provided models)
        if (fname.size() > 5 && fname.substr(fname.size() - 5) == ".gguf") {
            bool found_in_tiers = false;
            for (const auto& tier : g_model_tiers) {
                if (fname == tier.gguf_file) { found_in_tiers = true; break; }
            }
            if (!found_in_tiers) {
                std::string path = model_dir + "/" + fname;
                struct stat st;
                if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
                    ModelTier custom;
                    custom.name = fname.substr(0, fname.size() - 5);
                    custom.gguf_file = fname;
                    custom.size_mb = (uint32_t)(st.st_size / (1024 * 1024));
                    custom.min_ram_mb = custom.size_mb + 512; // rough estimate
                    custom.layers = 32; // default guess
                    result.push_back(custom);
                }
            }
        }
    }
    closedir(dir);

    // Sort by size ascending
    std::sort(result.begin(), result.end(),
              [](const ModelTier& a, const ModelTier& b) { return a.size_mb < b.size_mb; });
#endif
    return result;
}

std::string ClusterManager::compute_tensor_split() const {
    std::lock_guard<std::mutex> lock(mu_);

    if (peers_.empty()) return "";  // single node, no split needed

    // Collect usable RAM per node (70% safety factor, minus 300MB overhead)
    std::vector<uint32_t> usable;
    uint32_t self_usable = (uint32_t)(self_.ram_mb * 0.70) > 300 ?
                           (uint32_t)(self_.ram_mb * 0.70) - 300 : 0;
    usable.push_back(self_usable);

    for (const auto& p : peers_) {
        uint32_t pu = (uint32_t)(p.ram_mb * 0.70) > 300 ?
                      (uint32_t)(p.ram_mb * 0.70) - 300 : 0;
        usable.push_back(pu);
    }

    // Find minimum for normalization (avoid division by zero)
    uint32_t min_val = *std::min_element(usable.begin(), usable.end());
    if (min_val == 0) min_val = 1;

    // Build ratio string: normalize to smallest node
    std::string result;
    for (size_t i = 0; i < usable.size(); i++) {
        if (i > 0) result += ",";
        uint32_t ratio = usable[i] / min_val;
        if (ratio == 0) ratio = 1;
        result += std::to_string(ratio);
    }
    return result;
}

ModelTier ClusterManager::select_model(const std::string& model_dir) const {
    // Get cluster usable RAM
    uint32_t usable = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        usable = (uint32_t)(self_.ram_mb * 0.70);
        for (const auto& p : peers_)
            usable += (uint32_t)(p.ram_mb * 0.70);
    }

    // Scan available models
    auto models = available_models(model_dir);

    // Pick largest model that fits
    ModelTier best;
    for (const auto& m : models) {
        if (m.min_ram_mb <= usable) {
            best = m;  // sorted ascending, so last match wins
        }
    }
    return best;
}

ClusterCapacity ClusterManager::analyze_capacity(const std::string& model_dir) const {
    ClusterCapacity cap;
    {
        std::lock_guard<std::mutex> lock(mu_);
        cap.total_ram_mb = self_.ram_mb;
        cap.total_cores = self_.cpu_cores;
        cap.node_count = 1 + peers_.size();
        for (const auto& p : peers_) {
            cap.total_ram_mb += p.ram_mb;
            cap.total_cores += p.cpu_cores;
        }
    }
    cap.usable_ram_mb = (uint32_t)(cap.total_ram_mb * 0.70);

    // Select best model
    ModelTier best = select_model(model_dir);
    if (!best.name.empty()) {
        cap.recommended_model = best.name;
        cap.recommended_gguf = best.gguf_file;
    }

    // Compute tensor split
    cap.tensor_split = compute_tensor_split();

    // Check if a larger model from the tier table could fit but isn't downloaded
    for (auto it = g_model_tiers.rbegin(); it != g_model_tiers.rend(); ++it) {
        if (it->min_ram_mb <= cap.usable_ram_mb) {
            // This tier could fit — is it bigger than our best available?
            if (best.name.empty() || it->size_mb > best.size_mb) {
                cap.upgrade_available = true;
            }
            break;
        }
    }

    return cap;
}
