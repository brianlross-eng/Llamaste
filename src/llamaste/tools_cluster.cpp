// tools_cluster.cpp — Cluster mesh tools for Llamaste
//
// Tools: cluster.status, cluster.peers, cluster.reload, cluster.capacity,
//        cluster.models, cluster.offload
// Provides the LLM and HTTP API with cluster visibility, auto-offload, and control.

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

static std::string handle_cluster_capacity(const std::string& args_json) {
    (void)args_json;
    if (!g_cluster) return json_error("cluster not initialized");

    auto cap = g_cluster->analyze_capacity();
    json result;
    result["total_ram_mb"] = cap.total_ram_mb;
    result["usable_ram_mb"] = cap.usable_ram_mb;
    result["total_cores"] = cap.total_cores;
    result["node_count"] = cap.node_count;
    result["recommended_model"] = cap.recommended_model;
    result["recommended_gguf"] = cap.recommended_gguf;
    result["tensor_split"] = cap.tensor_split;
    result["upgrade_available"] = cap.upgrade_available;
    return result.dump();
}

static std::string handle_cluster_models(const std::string& args_json) {
    (void)args_json;
    if (!g_cluster) return json_error("cluster not initialized");

    auto cap = g_cluster->analyze_capacity();
    auto available = ClusterManager::available_models("/data/models");
    auto tiers = ClusterManager::model_tiers();

    json result;
    result["usable_ram_mb"] = cap.usable_ram_mb;

    // Available models on disk
    json avail_list = json::array();
    for (const auto& m : available) {
        avail_list.push_back({
            {"name", m.name},
            {"gguf_file", m.gguf_file},
            {"size_mb", m.size_mb},
            {"min_ram_mb", m.min_ram_mb},
            {"layers", m.layers},
            {"fits_cluster", m.min_ram_mb <= cap.usable_ram_mb}
        });
    }
    result["available"] = avail_list;

    // All known model tiers (for upgrade guidance)
    json tier_list = json::array();
    for (const auto& t : tiers) {
        bool on_disk = false;
        for (const auto& a : available) {
            if (a.gguf_file == t.gguf_file) { on_disk = true; break; }
        }
        tier_list.push_back({
            {"name", t.name},
            {"gguf_file", t.gguf_file},
            {"size_mb", t.size_mb},
            {"min_ram_mb", t.min_ram_mb},
            {"on_disk", on_disk},
            {"fits_cluster", t.min_ram_mb <= cap.usable_ram_mb}
        });
    }
    result["tiers"] = tier_list;

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

    reg.register_tool(ToolDef{
        .name = "cluster.capacity",
        .description = "Analyze cluster capacity: pooled RAM, usable RAM (70% safety), "
                       "recommended model, tensor-split ratios, upgrade availability.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_cluster_capacity
    });

    reg.register_tool(ToolDef{
        .name = "cluster.models",
        .description = "List available models on disk and all known model tiers. "
                       "Shows which models fit current cluster capacity and which "
                       "could be downloaded for an upgrade.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = handle_cluster_models
    });
}
