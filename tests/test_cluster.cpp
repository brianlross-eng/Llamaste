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

// --- Phase 5: Auto-offload tests ---

static void test_model_tiers() {
    auto& tiers = ClusterManager::model_tiers();
    assert(!tiers.empty());
    assert(tiers.size() == 7);  // 0.5B through 72B

    // Verify sorted ascending by size
    for (size_t i = 1; i < tiers.size(); i++) {
        assert(tiers[i].size_mb > tiers[i-1].size_mb);
    }

    // Verify smallest tier
    assert(tiers[0].name == "qwen2.5-0.5b-instruct");
    assert(tiers[0].size_mb == 400);
    assert(tiers[0].min_ram_mb == 1024);

    // Verify largest tier
    assert(tiers[6].name == "qwen2.5-72b-instruct");
    assert(tiers[6].size_mb == 42000);
    printf("  PASS: model_tiers\n");
}

static void test_tensor_split_single_node() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 8192; self.cpu_cores = 4;
    mgr.set_self_info(self);

    // Single node: no tensor split needed
    std::string split = mgr.compute_tensor_split();
    assert(split.empty());
    printf("  PASS: tensor_split_single_node\n");
}

static void test_tensor_split_two_equal_nodes() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 4096; self.cpu_cores = 2;
    mgr.set_self_info(self);

    PeerInfo peer;
    peer.hostname = "peer"; peer.ip = "192.168.1.20";
    peer.ram_mb = 4096; peer.cpu_cores = 2;
    mgr.add_peer(peer);

    std::string split = mgr.compute_tensor_split();
    // Equal RAM -> equal ratios: "1,1"
    assert(split == "1,1");
    printf("  PASS: tensor_split_two_equal_nodes\n");
}

static void test_tensor_split_unequal_nodes() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 4096; self.cpu_cores = 2;
    mgr.set_self_info(self);

    PeerInfo peer;
    peer.hostname = "peer"; peer.ip = "192.168.1.20";
    peer.ram_mb = 16384; peer.cpu_cores = 4;
    mgr.add_peer(peer);

    std::string split = mgr.compute_tensor_split();
    // self usable: 4096*0.70 - 300 = 2567 (approx)
    // peer usable: 16384*0.70 - 300 = 11168 (approx)
    // min = 2567, ratios: 2567/2567=1, 11168/2567=4
    assert(split == "1,4");
    printf("  PASS: tensor_split_unequal_nodes\n");
}

static void test_tensor_split_three_nodes() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 4096; self.cpu_cores = 2;
    mgr.set_self_info(self);

    PeerInfo p1;
    p1.hostname = "p1"; p1.ip = "192.168.1.20";
    p1.ram_mb = 8192; p1.cpu_cores = 4;
    mgr.add_peer(p1);

    PeerInfo p2;
    p2.hostname = "p2"; p2.ip = "192.168.1.30";
    p2.ram_mb = 16384; p2.cpu_cores = 8;
    mgr.add_peer(p2);

    std::string split = mgr.compute_tensor_split();
    // self: 4096*0.70 - 300 = 2567
    // p1:   8192*0.70 - 300 = 5434
    // p2:  16384*0.70 - 300 = 11168
    // min = 2567, ratios: 1, 2, 4
    assert(split == "1,2,4");
    printf("  PASS: tensor_split_three_nodes\n");
}

static void test_analyze_capacity_single_node() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 8192; self.cpu_cores = 4;
    mgr.set_self_info(self);

    // Use a non-existent dir so no models are found
    auto cap = mgr.analyze_capacity("/nonexistent");
    assert(cap.total_ram_mb == 8192);
    assert(cap.total_cores == 4);
    assert(cap.node_count == 1);
    assert(cap.usable_ram_mb == (uint32_t)(8192 * 0.70));
    assert(cap.tensor_split.empty());  // single node
    printf("  PASS: analyze_capacity_single_node\n");
}

static void test_analyze_capacity_multi_node() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 4096; self.cpu_cores = 2;
    mgr.set_self_info(self);

    PeerInfo peer;
    peer.hostname = "peer"; peer.ip = "192.168.1.20";
    peer.ram_mb = 4096; peer.cpu_cores = 2;
    mgr.add_peer(peer);

    auto cap = mgr.analyze_capacity("/nonexistent");
    assert(cap.total_ram_mb == 8192);
    assert(cap.total_cores == 4);
    assert(cap.node_count == 2);
    assert(cap.usable_ram_mb == (uint32_t)(8192 * 0.70));
    assert(!cap.tensor_split.empty());
    printf("  PASS: analyze_capacity_multi_node\n");
}

static void test_select_model_no_models() {
    ClusterManager mgr;
    PeerInfo self;
    self.hostname = "self"; self.ip = "192.168.1.10";
    self.ram_mb = 8192; self.cpu_cores = 4;
    mgr.set_self_info(self);

    // No model dir -> empty result
    ModelTier best = mgr.select_model("/nonexistent");
    assert(best.name.empty());
    printf("  PASS: select_model_no_models\n");
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

    printf("--- Auto-offload tests ---\n");
    test_model_tiers();
    test_tensor_split_single_node();
    test_tensor_split_two_equal_nodes();
    test_tensor_split_unequal_nodes();
    test_tensor_split_three_nodes();
    test_analyze_capacity_single_node();
    test_analyze_capacity_multi_node();
    test_select_model_no_models();
}

int main() {
    run_cluster_tests();
    printf("\nAll cluster tests passed!\n");
    return 0;
}
