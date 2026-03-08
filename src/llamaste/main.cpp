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

// Read a config value from /data/llamaste/config/<key>.
// Returns empty string if not set.
static std::string read_config(const char* key) {
    std::string path = std::string("/data/llamaste/config/") + key;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return "";
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    // Trim trailing whitespace/newlines
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return std::string(buf, n);
}

static std::string select_model(int available_mb) {
    // Check for user-configured model override
    std::string override_path = read_config("model.path");
    if (!override_path.empty()) {
        if (access(override_path.c_str(), R_OK) == 0) {
            fprintf(stderr, "[main] Model override: %s\n", override_path.c_str());
            return override_path;
        }
        fprintf(stderr, "[main] Model override not found: %s (falling back to auto)\n",
                override_path.c_str());
    }

    // Auto-select: pick largest model that fits available RAM
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
        // Live ISO: pivot to the full squashfs system and re-exec.
        // No-op on installed system (guard checks /boot/bzImage which only
        // exists on the ISO root, not inside rootfs.squashfs).
        // If pivot succeeds, execv() is called and we never reach the next line.
        do_live_pivot(argv);
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
        init_bring_up_loopback();
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
