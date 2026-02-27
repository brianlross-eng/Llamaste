// tools_process.cpp — Process tools for Llamaste LLM-OS
//
// Reads process information from /proc. Since Llamaste runs as PID 1
// with a child inference process, there are typically very few processes.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <sys/types.h>

using json = nlohmann::json;

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

// Read the contents of a file into a string. Returns empty on failure.
static std::string read_file_contents(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Parse /proc/<pid>/stat into a JSON object with key fields.
// Format: pid (comm) state ppid pgrp session tty_nr tpgid flags
//         minflt cminflt majflt cmajflt utime stime cutime cstime
//         priority nice num_threads itrealvalue starttime vsize rss ...
static json parse_proc_stat(int pid) {
    std::string stat_path = "/proc/" + std::to_string(pid) + "/stat";
    std::string content = read_file_contents(stat_path);
    if (content.empty()) return json();

    // Extract comm (name in parentheses) — handles names with spaces
    auto open_paren = content.find('(');
    auto close_paren = content.rfind(')');
    if (open_paren == std::string::npos || close_paren == std::string::npos) {
        return json();
    }

    std::string comm = content.substr(open_paren + 1, close_paren - open_paren - 1);

    // Parse fields after the closing paren
    std::string rest = content.substr(close_paren + 2);
    std::istringstream ss(rest);
    std::vector<std::string> fields;
    std::string field;
    while (ss >> field) {
        fields.push_back(field);
    }

    json info;
    info["pid"] = pid;
    info["name"] = comm;

    if (fields.size() > 0) info["state"] = fields[0];
    if (fields.size() > 1) {
        try { info["ppid"] = std::stoi(fields[1]); } catch (...) {}
    }
    if (fields.size() > 11) {
        try {
            long utime = std::stol(fields[11]);
            long stime = std::stol(fields[12]);
            // Convert jiffies to seconds (assuming 100 Hz)
            info["cpu_time_sec"] = (utime + stime) / 100;
        } catch (...) {}
    }
    if (fields.size() > 17) {
        try { info["threads"] = std::stoi(fields[17]); } catch (...) {}
    }
    if (fields.size() > 20) {
        try { info["vsize_bytes"] = std::stol(fields[20]); } catch (...) {}
    }
    if (fields.size() > 21) {
        try {
            long rss_pages = std::stol(fields[21]);
            info["rss_bytes"] = rss_pages * sysconf(_SC_PAGESIZE);
        } catch (...) {}
    }

    // Read cmdline for the full command
    std::string cmdline_path = "/proc/" + std::to_string(pid) + "/cmdline";
    std::string cmdline = read_file_contents(cmdline_path);
    // Replace null bytes with spaces
    for (char& c : cmdline) {
        if (c == '\0') c = ' ';
    }
    if (!cmdline.empty() && cmdline.back() == ' ') {
        cmdline.pop_back();
    }
    if (!cmdline.empty()) {
        info["cmdline"] = cmdline;
    }

    return info;
}

// ---------------------------------------------------------------------------
// process.list — list running processes
// ---------------------------------------------------------------------------
static std::string handle_process_list(const std::string& args_json) {
    (void)args_json;

    DIR* dir = opendir("/proc");
    if (!dir) {
        return json_error("cannot open /proc");
    }

    json processes = json::array();
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        // Only numeric directories are PIDs
        bool is_pid = true;
        for (const char* p = ent->d_name; *p; p++) {
            if (*p < '0' || *p > '9') { is_pid = false; break; }
        }
        if (!is_pid) continue;

        int pid = std::stoi(ent->d_name);
        json info = parse_proc_stat(pid);
        if (!info.is_null()) {
            processes.push_back(info);
        }
    }
    closedir(dir);

    json result;
    result["processes"] = processes;
    result["count"] = processes.size();
    return result.dump();
}

// ---------------------------------------------------------------------------
// process.info — get detailed info about a specific PID
// ---------------------------------------------------------------------------
static std::string handle_process_info(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    int pid = args.value("pid", -1);
    if (pid < 0) return json_error("pid is required and must be non-negative");

    json info = parse_proc_stat(pid);
    if (info.is_null()) {
        return json_error("process not found: " + std::to_string(pid));
    }

    // Read additional info from /proc/<pid>/status for supplementary fields
    std::string status_path = "/proc/" + std::to_string(pid) + "/status";
    std::ifstream status_file(status_path);
    if (status_file.is_open()) {
        std::string line;
        while (std::getline(status_file, line)) {
            if (line.substr(0, 4) == "Uid:") {
                std::istringstream ss(line.substr(5));
                int uid;
                if (ss >> uid) info["uid"] = uid;
            } else if (line.substr(0, 4) == "Gid:") {
                std::istringstream ss(line.substr(5));
                int gid;
                if (ss >> gid) info["gid"] = gid;
            } else if (line.substr(0, 6) == "VmRSS:") {
                std::istringstream ss(line.substr(7));
                long kb;
                if (ss >> kb) info["rss_kb"] = kb;
            } else if (line.substr(0, 7) == "VmSize:") {
                std::istringstream ss(line.substr(8));
                long kb;
                if (ss >> kb) info["vsize_kb"] = kb;
            }
        }
    }

    // Read /proc/<pid>/fd count (open file descriptors)
    std::string fd_path = "/proc/" + std::to_string(pid) + "/fd";
    DIR* fd_dir = opendir(fd_path.c_str());
    if (fd_dir) {
        int fd_count = 0;
        struct dirent* ent;
        while ((ent = readdir(fd_dir)) != nullptr) {
            if (ent->d_name[0] != '.') fd_count++;
        }
        closedir(fd_dir);
        info["open_fds"] = fd_count;
    }

    return info.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_process_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "process.list",
        .description = "List all running processes with PID, name, state, CPU time, "
                       "memory usage, and command line. Reads from /proc.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_process_list
    });

    reg.register_tool({
        .name = "process.info",
        .description = "Get detailed information about a specific process by PID, "
                       "including memory usage, threads, UID, and open file descriptors.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "pid": {
                    "type": "integer",
                    "description": "Process ID to query"
                }
            },
            "required": ["pid"]
        })json",
        .handler = handle_process_info
    });
}
