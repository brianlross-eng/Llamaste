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
