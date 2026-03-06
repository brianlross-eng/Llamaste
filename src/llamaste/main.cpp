#include <cstdio>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include "init.h"
#include "hwdetect.h"
#include "supervisor.h"

static const char* VERSION = "0.1.0";

struct ModelCandidate {
    const char* name;
    const char* filename;
    int required_mb;
};

static const ModelCandidate MODELS[] = {
    {"Qwen2.5-32B-Instruct",  "qwen2.5-32b-instruct-q4_k_m.gguf",  22000},
    {"Qwen2.5-14B-Instruct",  "qwen2.5-14b-instruct-q4_k_m.gguf",  11000},
    {"Qwen2.5-7B-Instruct",   "qwen2.5-7b-instruct-q4_k_m.gguf",    6500},
    {"Qwen2.5-3B-Instruct",   "qwen2.5-3b-instruct-q4_k_m.gguf",    4000},
    {"Qwen2.5-1.5B-Instruct", "qwen2.5-1.5b-instruct-q4_k_m.gguf",  2500},
    {"Qwen2.5-0.5B-Instruct", "qwen2.5-0.5b-instruct-q4_k_m.gguf",  1500},
    {nullptr, nullptr, 0}
};

static std::string select_model(int available_mb) {
    for (int i = 0; MODELS[i].name; i++) {
        if (available_mb >= MODELS[i].required_mb) {
            std::string path = std::string("/data/models/") + MODELS[i].filename;
            if (access(path.c_str(), R_OK) == 0) {
                fprintf(stderr, "[main] Selected model: %s\n", MODELS[i].name);
                return path;
            }
        }
    }
    return "";
}

int main(int argc, char** argv) {
    fprintf(stderr, "\n");
    fprintf(stderr, "  Llamaste v%s — LLM IS the OS\n", VERSION);
    fprintf(stderr, "\n");

    bool pid1 = (getpid() == 1);

    if (pid1) {
        init_mount_filesystems();
    }

    std::string mode = init_parse_boot_mode();
    fprintf(stderr, "[main] Boot mode: %s\n", mode.c_str());

    if (pid1) {
        if (mode == "live") {
            // Live ISO mode: use tmpfs for /data, no disk probe
            fprintf(stderr, "[main] Live mode: using tmpfs for /data\n");
            mkdir("/data", 0755);
            mount("tmpfs", "/data", "tmpfs", 0, "size=1G");
        } else {
            init_mount_data();
        }
        init_create_data_dirs();
        init_mount_esp();   // Mount ESP for grubenv (A/B update slot management)
    }

    HardwareInfo hw = detect_hardware();
    fprintf(stderr, "[main] CPU: %s (%d cores)\n",
            hw.cpu_model.c_str(), hw.cpu_cores);
    fprintf(stderr, "[main] RAM: %d MB total, %d MB available\n",
            hw.ram_total_mb, hw.ram_free_mb);

    if (pid1) {
        init_apply_network_config();
        init_tune_performance();
        init_set_hostname("llamaste");
    }

    int available = hw.ram_free_mb;
    if (mode == "desktop") available -= 500;
    std::string model_path = select_model(available);

    if (model_path.empty()) {
        fprintf(stderr, "[main] No model found in /data/models/\n");
        fprintf(stderr, "[main] Will start without model (web UI only)\n");
    }

    SupervisorConfig sc;
    sc.model_path = model_path;
    sc.boot_mode = mode;
    sc.http_port = 80;
    sc.cpu_cores = hw.cpu_cores;
    sc.ram_total_mb = hw.ram_total_mb;

    if (pid1) {
        supervisor_run(sc);
    } else {
        fprintf(stderr, "[main] Not PID 1, running child_main directly\n");
        extern int child_main(const SupervisorConfig& config);
        return child_main(sc);
    }
}
