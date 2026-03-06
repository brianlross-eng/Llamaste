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
                if (age >= timeout_seconds) {
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
