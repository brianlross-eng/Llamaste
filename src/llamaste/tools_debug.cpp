// tools_debug.cpp — Debug tools for Llamaste recovery testing
//
// Gated behind LLAMASTE_TEST_API. Provides tools to simulate crashes
// and inspect state for automated QEMU test suites.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>

using json = nlohmann::json;

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

// Scan /proc for a process whose cmdline contains "llama-server".
// Skip PID 1 (init/supervisor) and our own PID.
static pid_t find_llama_server_pid() {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;

    pid_t self = getpid();
    pid_t found = -1;
    struct dirent* ent;

    while ((ent = readdir(dir)) != nullptr) {
        // Only numeric directories are PIDs
        bool is_pid = true;
        for (const char* p = ent->d_name; *p; p++) {
            if (*p < '0' || *p > '9') { is_pid = false; break; }
        }
        if (!is_pid) continue;

        int pid = std::stoi(ent->d_name);
        if (pid == 1 || pid == self) continue;

        std::string cmdline_path = "/proc/" + std::to_string(pid) + "/cmdline";
        std::ifstream f(cmdline_path);
        if (!f.is_open()) continue;

        std::string cmdline;
        std::getline(f, cmdline, '\0');
        // cmdline may contain multiple null-separated args; read the rest too
        std::string arg;
        while (std::getline(f, arg, '\0')) {
            cmdline += " " + arg;
        }

        if (cmdline.find("llama-server") != std::string::npos) {
            found = pid;
            break;
        }
    }
    closedir(dir);
    return found;
}

// ---------------------------------------------------------------------------
// debug.kill_child — SIGKILL the llama-server child process
// ---------------------------------------------------------------------------
static std::string handle_kill_child(const std::string& args_json) {
    (void)args_json;

    pid_t pid = find_llama_server_pid();
    if (pid <= 0) {
        return json_error("llama-server process not found");
    }

    int rc = kill(pid, SIGKILL);
    if (rc != 0) {
        return json_error("kill() failed: " + std::string(strerror(errno)));
    }

    json result;
    result["killed"] = true;
    result["pid"] = pid;
    result["signal"] = "SIGKILL";
    return result.dump();
}

// ---------------------------------------------------------------------------
// debug.crash_info — read last 20 lines of llama-server log
// ---------------------------------------------------------------------------
static std::string handle_crash_info(const std::string& args_json) {
    (void)args_json;

    std::vector<std::string> lines;
    std::ifstream f("/tmp/llama-server.log");
    if (f.is_open()) {
        std::string line;
        while (std::getline(f, line)) {
            lines.push_back(line);
            if (lines.size() > 20) {
                lines.erase(lines.begin());
            }
        }
    }

    json log_tail = json::array();
    for (const auto& l : lines) {
        log_tail.push_back(l);
    }

    json result;
    result["log_tail"] = log_tail;
    result["self_pid"] = getpid();
    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_debug_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "debug.kill_child",
        .description = "Kill the llama-server child process with SIGKILL. "
                       "Used by automated tests to verify supervisor restart. "
                       "Only available in test builds.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_kill_child
    });

    reg.register_tool({
        .name = "debug.crash_info",
        .description = "Read the last 20 lines of /tmp/llama-server.log and "
                       "return the current PID. Used for post-crash diagnostics.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_crash_info
    });
}
