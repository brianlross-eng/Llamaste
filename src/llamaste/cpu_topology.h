// cpu_topology.h — Detect CPU topology for optimal llama-server thread config
//
// Detects:
//   - Physical vs logical (HT) core count
//   - Hybrid P-core / E-core topology (Intel Alder Lake+)
//   - P-core list for --cpu-range pinning
//
// Backward compatible: non-hybrid CPUs (AMD, older Intel) get sensible
// defaults with no behavior change. Hybrid CPUs get P-core pinning.
#pragma once

#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <dirent.h>

#ifndef _WIN32
#include <unistd.h>
#endif

struct CpuTopology {
    int logical_cores = 0;       // Total threads (what /proc/cpuinfo "processor" count gives)
    int physical_cores = 0;      // Physical cores (deduplicated by core_id)
    bool is_hybrid = false;      // True if P-core/E-core detected
    int p_cores = 0;             // Performance cores (physical)
    int e_cores = 0;             // Efficiency cores (physical)
    int p_threads = 0;           // P-core threads (P-cores * 2 if HT)
    int e_threads = 0;           // E-core threads (E-cores, typically no HT)
    std::string p_core_range;    // CPU list for --cpu-range, e.g. "0-15"
    std::string cpu_features;    // Detected SIMD features for logging

    // Recommended thread counts for llama-server
    int recommended_threads = 1;       // For -t (token generation, memory-bound)
    int recommended_batch_threads = 1; // For -tb (prompt processing, compute-bound)
};

// Parse a CPU list string like "0-7,16-23" into a set of CPU IDs.
inline std::set<int> parse_cpulist(const std::string& list) {
    std::set<int> cpus;
    std::istringstream ss(list);
    std::string token;
    while (std::getline(ss, token, ',')) {
        auto dash = token.find('-');
        if (dash != std::string::npos) {
            int lo = std::stoi(token.substr(0, dash));
            int hi = std::stoi(token.substr(dash + 1));
            for (int i = lo; i <= hi; i++) cpus.insert(i);
        } else {
            cpus.insert(std::stoi(token));
        }
    }
    return cpus;
}

// Format a set of CPU IDs into a compact range string like "0-15".
inline std::string format_cpulist(const std::set<int>& cpus) {
    if (cpus.empty()) return "";
    std::string result;
    auto it = cpus.begin();
    int range_start = *it, range_end = *it;
    ++it;
    while (it != cpus.end()) {
        if (*it == range_end + 1) {
            range_end = *it;
        } else {
            if (!result.empty()) result += ",";
            if (range_start == range_end)
                result += std::to_string(range_start);
            else
                result += std::to_string(range_start) + "-" + std::to_string(range_end);
            range_start = range_end = *it;
        }
        ++it;
    }
    if (!result.empty()) result += ",";
    if (range_start == range_end)
        result += std::to_string(range_start);
    else
        result += std::to_string(range_start) + "-" + std::to_string(range_end);
    return result;
}

// Read a single-line sysfs file, trimming trailing whitespace.
inline std::string read_sysfs_trim(const std::string& path) {
    std::ifstream f(path);
    std::string line;
    if (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
            line.pop_back();
    }
    return line;
}

// Detect CPU topology. Safe on all x86-64 Linux systems.
inline CpuTopology detect_cpu_topology() {
    CpuTopology topo;

    // ---------------------------------------------------------------
    // Step 1: Count logical cores and detect physical cores + features
    // ---------------------------------------------------------------
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    std::set<std::string> physical_ids; // "physical_id:core_id" pairs
    std::string cur_phys_id, cur_core_id;
    bool saw_avx2 = false, saw_avx512 = false, saw_amx = false, saw_vnni = false;

    while (std::getline(cpuinfo, line)) {
        if (line.find("processor") == 0) {
            topo.logical_cores++;
            // Flush previous core's IDs
            if (!cur_phys_id.empty() && !cur_core_id.empty()) {
                physical_ids.insert(cur_phys_id + ":" + cur_core_id);
            }
            cur_phys_id.clear();
            cur_core_id.clear();
        }
        if (line.find("physical id") == 0) {
            auto colon = line.find(':');
            if (colon != std::string::npos)
                cur_phys_id = line.substr(colon + 2);
        }
        if (line.find("core id") == 0) {
            auto colon = line.find(':');
            if (colon != std::string::npos)
                cur_core_id = line.substr(colon + 2);
        }
        if (line.find("flags") == 0 || line.find("Features") == 0) {
            if (line.find("avx2") != std::string::npos) saw_avx2 = true;
            if (line.find("avx512") != std::string::npos) saw_avx512 = true;
            if (line.find("amx_tile") != std::string::npos) saw_amx = true;
            if (line.find("avx_vnni") != std::string::npos || line.find("avx512_vnni") != std::string::npos) saw_vnni = true;
        }
    }
    // Flush last core
    if (!cur_phys_id.empty() && !cur_core_id.empty()) {
        physical_ids.insert(cur_phys_id + ":" + cur_core_id);
    }

    if (topo.logical_cores == 0) topo.logical_cores = 1;
    topo.physical_cores = physical_ids.empty() ? topo.logical_cores : (int)physical_ids.size();

    // Build features string for logging
    std::string feats;
    if (saw_avx2) feats += "AVX2 ";
    if (saw_avx512) feats += "AVX-512 ";
    if (saw_vnni) feats += "VNNI ";
    if (saw_amx) feats += "AMX ";
    if (feats.empty()) feats = "SSE4.2";
    topo.cpu_features = feats;

    // ---------------------------------------------------------------
    // Step 2: Detect hybrid P-core / E-core topology
    // ---------------------------------------------------------------
    // Method A: sysfs cpu types (kernel 6.12+)
    std::set<int> p_core_cpus, e_core_cpus;

    std::string p_list = read_sysfs_trim("/sys/devices/system/cpu/types/intel_core/cpulist");
    std::string e_list = read_sysfs_trim("/sys/devices/system/cpu/types/intel_atom/cpulist");

    if (!p_list.empty() && !e_list.empty()) {
        p_core_cpus = parse_cpulist(p_list);
        e_core_cpus = parse_cpulist(e_list);
        topo.is_hybrid = true;
        fprintf(stderr, "[topology] Hybrid CPU detected via sysfs: P-cores=%s E-cores=%s\n",
                p_list.c_str(), e_list.c_str());
    }

    // Method B: cpufreq max frequency clustering (fallback)
    if (!topo.is_hybrid && topo.logical_cores > 1) {
        std::vector<std::pair<int, int>> cpu_freqs; // {cpu_id, max_freq_khz}
        for (int i = 0; i < topo.logical_cores; i++) {
            std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(i) +
                               "/cpufreq/cpuinfo_max_freq";
            std::string val = read_sysfs_trim(path);
            if (!val.empty()) {
                cpu_freqs.push_back({i, std::stoi(val)});
            }
        }

        if (cpu_freqs.size() >= 2) {
            // Find max and min frequencies
            int max_freq = 0, min_freq = INT32_MAX;
            for (auto& [id, freq] : cpu_freqs) {
                if (freq > max_freq) max_freq = freq;
                if (freq < min_freq) min_freq = freq;
            }
            // If there's a significant gap (>20%), treat as hybrid
            if (min_freq > 0 && max_freq > min_freq * 1.2) {
                int threshold = (max_freq + min_freq) / 2;
                for (auto& [id, freq] : cpu_freqs) {
                    if (freq >= threshold)
                        p_core_cpus.insert(id);
                    else
                        e_core_cpus.insert(id);
                }
                topo.is_hybrid = true;
                fprintf(stderr, "[topology] Hybrid CPU detected via cpufreq: "
                        "P-freq=%dMHz E-freq=%dMHz threshold=%dMHz\n",
                        max_freq / 1000, min_freq / 1000, threshold / 1000);
            }
        }
    }

    // ---------------------------------------------------------------
    // Step 3: Compute thread recommendations
    // ---------------------------------------------------------------
    if (topo.is_hybrid && !p_core_cpus.empty()) {
        topo.p_threads = (int)p_core_cpus.size();
        topo.e_threads = (int)e_core_cpus.size();

        // P-core physical count: each P-core has 2 threads (HT), E-cores have 1
        // Heuristic: if p_threads > physical_cores - e_core count, P-cores have HT
        // Simpler: count unique core_ids among P-core CPUs
        // For now, estimate: if p_threads is even and e_threads matches remaining,
        // assume P-cores have HT (2 threads each)
        int total_e_physical = topo.e_threads; // E-cores typically don't have HT
        int p_physical_estimate = topo.physical_cores - total_e_physical;
        if (p_physical_estimate <= 0) p_physical_estimate = topo.p_threads / 2;
        if (p_physical_estimate <= 0) p_physical_estimate = 1;
        topo.p_cores = p_physical_estimate;
        topo.e_cores = total_e_physical;

        topo.p_core_range = format_cpulist(p_core_cpus);

        // Token generation: physical P-cores only (HT hurts memory-bound work)
        topo.recommended_threads = topo.p_cores;
        // Prompt processing: all P-core threads (HT helps compute-bound work)
        topo.recommended_batch_threads = topo.p_threads;

        fprintf(stderr, "[topology] P-cores: %d physical, %d threads (%s) | E-cores: %d\n",
                topo.p_cores, topo.p_threads, topo.p_core_range.c_str(), topo.e_cores);
        fprintf(stderr, "[topology] Recommended: -t %d -tb %d --cpu-range %s\n",
                topo.recommended_threads, topo.recommended_batch_threads,
                topo.p_core_range.c_str());
    } else {
        // Homogeneous CPU (AMD, older Intel, single-core)
        // Token generation: physical cores (no HT)
        topo.recommended_threads = topo.physical_cores;
        // Prompt processing: all logical cores (HT helps compute-bound)
        topo.recommended_batch_threads = topo.logical_cores;

        fprintf(stderr, "[topology] Homogeneous CPU: %d physical, %d logical | Features: %s\n",
                topo.physical_cores, topo.logical_cores, topo.cpu_features.c_str());
        fprintf(stderr, "[topology] Recommended: -t %d -tb %d\n",
                topo.recommended_threads, topo.recommended_batch_threads);
    }

    // Sanity clamp
    if (topo.recommended_threads < 1) topo.recommended_threads = 1;
    if (topo.recommended_batch_threads < 1) topo.recommended_batch_threads = 1;

    return topo;
}
