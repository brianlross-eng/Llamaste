// Llamaste — LLM IS the OS
// main.cpp: PID 1 init, hardware detection, model loading, server startup
//
// This binary runs as PID 1 (the kernel's direct child). It:
// 1. Mounts essential filesystems (/proc, /sys, /dev, /data)
// 2. Configures networking (DHCP)
// 3. Detects hardware (CPU features, RAM, GPU)
// 4. Auto-selects the best model for available RAM
// 5. Loads the model and starts serving
// 6. Serves: inference API + agent chat + system tools + web UI

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <dirent.h>
#include <fstream>
#include <string>
#include <vector>
#include <functional>

#include "agent.h"
#include "tools.h"
#include "prompt_builder.h"

// Forward declarations for llama.cpp integration
// These will be resolved when we link against the actual llama.cpp libraries
struct llama_model;
struct llama_context;

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------

static volatile bool g_running = true;
static std::string g_mode = "server"; // "server" or "desktop"

struct SystemState {
    std::string hostname     = "llamaste";
    std::string ip_address   = "0.0.0.0";
    std::string cpu_model    = "unknown";
    int         cpu_cores    = 1;
    int         ram_total_mb = 0;
    int         ram_free_mb  = 0;
    std::string model_name   = "none";
    std::string model_path   = "";
    float       tokens_per_sec = 0.0f;
    bool        gpu_detected = false;
    std::string gpu_name     = "";
};

static SystemState g_state;

// ---------------------------------------------------------------------------
// Signal handling — PID 1 must not exit
// ---------------------------------------------------------------------------

static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        fprintf(stderr, "[llamaste] Received signal %d, shutting down...\n", sig);
        g_running = false;
    }
    if (sig == SIGUSR1) {
        // Graceful restart: reload model
        fprintf(stderr, "[llamaste] Received SIGUSR1, reloading model...\n");
    }
}

static void setup_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGUSR1, &sa, nullptr);

    // PID 1 must reap orphaned children
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = SA_NOCLDWAIT; // Auto-reap children
    sigaction(SIGCHLD, &sa, nullptr);
}

// ---------------------------------------------------------------------------
// PID 1 init: mount filesystems, configure basics
// ---------------------------------------------------------------------------

static bool is_pid1() {
    return getpid() == 1;
}

static void mount_fs(const char* source, const char* target,
                     const char* fstype, unsigned long flags,
                     const char* data) {
    mkdir(target, 0755);
    if (mount(source, target, fstype, flags, data) != 0) {
        perror(target);
    }
}

static void init_filesystems() {
    if (!is_pid1()) return;

    fprintf(stderr, "[llamaste] Mounting filesystems...\n");
    mount_fs("proc",     "/proc",    "proc",     0, nullptr);
    mount_fs("sysfs",    "/sys",     "sysfs",    0, nullptr);
    mount_fs("devtmpfs", "/dev",     "devtmpfs", 0, nullptr);
    mount_fs("tmpfs",    "/tmp",     "tmpfs",    0, "size=64M");
    mount_fs("tmpfs",    "/run",     "tmpfs",    0, "size=16M");

    mkdir("/dev/pts", 0755);
    mount_fs("devpts", "/dev/pts", "devpts", 0, nullptr);

    // Mount data partition (ext4, last partition on disk)
    // Try common device paths
    const char* data_devs[] = {
        "/dev/sda4", "/dev/sda3", "/dev/sda2",
        "/dev/vda4", "/dev/vda3", "/dev/vda2",
        "/dev/nvme0n1p4", "/dev/nvme0n1p3",
        nullptr
    };

    mkdir("/data", 0755);
    bool data_mounted = false;
    for (int i = 0; data_devs[i] != nullptr; i++) {
        struct stat st;
        if (stat(data_devs[i], &st) == 0) {
            if (mount(data_devs[i], "/data", "ext4", 0, nullptr) == 0) {
                fprintf(stderr, "[llamaste] Mounted %s on /data\n", data_devs[i]);
                data_mounted = true;
                break;
            }
        }
    }

    if (!data_mounted) {
        fprintf(stderr, "[llamaste] WARNING: No data partition found, using tmpfs\n");
        mount_fs("tmpfs", "/data", "tmpfs", 0, "size=512M");
    }

    // Create data subdirectories
    mkdir("/data/models",       0755);
    mkdir("/data/llamaste",     0755);
    mkdir("/data/llamaste/conversations", 0755);
    mkdir("/data/llamaste/config",        0755);
    mkdir("/data/llamaste/logs",          0755);
    mkdir("/data/llamaste/skills",        0755);
}

// ---------------------------------------------------------------------------
// Parse kernel command line for boot mode
// ---------------------------------------------------------------------------

static void parse_cmdline() {
    std::ifstream cmdline("/proc/cmdline");
    std::string line;
    if (std::getline(cmdline, line)) {
        if (line.find("llamaste.mode=desktop") != std::string::npos) {
            g_mode = "desktop";
        }
        // Could also parse: llamaste.model=xxx, llamaste.port=xxx, etc.
    }
    fprintf(stderr, "[llamaste] Boot mode: %s\n", g_mode.c_str());
}

// ---------------------------------------------------------------------------
// Hardware detection
// ---------------------------------------------------------------------------

static std::string read_file_line(const char* path) {
    std::ifstream f(path);
    std::string line;
    if (std::getline(f, line)) return line;
    return "";
}

static int read_meminfo_kb(const char* key) {
    std::ifstream f("/proc/meminfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find(key) == 0) {
            // "MemTotal:       16384000 kB"
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                return std::atoi(line.c_str() + colon + 1);
            }
        }
    }
    return 0;
}

static void detect_hardware() {
    fprintf(stderr, "[llamaste] Detecting hardware...\n");

    // CPU info
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    int core_count = 0;
    while (std::getline(cpuinfo, line)) {
        if (line.find("model name") == 0 && g_state.cpu_model == "unknown") {
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                g_state.cpu_model = line.substr(colon + 2);
            }
        }
        if (line.find("processor") == 0) {
            core_count++;
        }
    }
    g_state.cpu_cores = core_count > 0 ? core_count : 1;

    // RAM
    g_state.ram_total_mb = read_meminfo_kb("MemTotal") / 1024;
    g_state.ram_free_mb  = read_meminfo_kb("MemAvailable") / 1024;

    // GPU detection (basic: check for NVIDIA/AMD/Intel in /sys)
    // Full GPU detection would check /sys/class/drm/card*/device/vendor
    DIR* drm = opendir("/sys/class/drm");
    if (drm) {
        struct dirent* entry;
        while ((entry = readdir(drm)) != nullptr) {
            std::string vendor_path = std::string("/sys/class/drm/") +
                                      entry->d_name + "/device/vendor";
            std::string vendor = read_file_line(vendor_path.c_str());
            if (vendor == "0x10de") { // NVIDIA
                g_state.gpu_detected = true;
                g_state.gpu_name = "NVIDIA GPU";
            } else if (vendor == "0x1002") { // AMD
                g_state.gpu_detected = true;
                g_state.gpu_name = "AMD GPU";
            } else if (vendor == "0x8086") { // Intel
                g_state.gpu_detected = true;
                g_state.gpu_name = "Intel GPU";
            }
        }
        closedir(drm);
    }

    fprintf(stderr, "[llamaste] CPU: %s (%d cores)\n",
            g_state.cpu_model.c_str(), g_state.cpu_cores);
    fprintf(stderr, "[llamaste] RAM: %d MB total, %d MB available\n",
            g_state.ram_total_mb, g_state.ram_free_mb);
    if (g_state.gpu_detected) {
        fprintf(stderr, "[llamaste] GPU: %s\n", g_state.gpu_name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Model auto-selection based on available RAM
// ---------------------------------------------------------------------------

struct ModelCandidate {
    const char* name;
    const char* filename;
    int         required_mb; // Model size + ~2 GB for KV cache + OS
};

// These are the Qwen2.5-Instruct models at Q4_K_M quantization
static const ModelCandidate MODEL_CANDIDATES[] = {
    {"Qwen2.5-32B-Instruct",  "qwen2.5-32b-instruct-q4_k_m.gguf",  22000},
    {"Qwen2.5-14B-Instruct",  "qwen2.5-14b-instruct-q4_k_m.gguf",  11000},
    {"Qwen2.5-7B-Instruct",   "qwen2.5-7b-instruct-q4_k_m.gguf",    6500},
    {"Qwen2.5-3B-Instruct",   "qwen2.5-3b-instruct-q4_k_m.gguf",    4000},
    {"Qwen2.5-1.5B-Instruct", "qwen2.5-1.5b-instruct-q4_k_m.gguf",  2500},
    {nullptr, nullptr, 0}
};

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void select_model() {
    fprintf(stderr, "[llamaste] Selecting model for %d MB available RAM...\n",
            g_state.ram_free_mb);

    // Account for desktop mode overhead
    int available = g_state.ram_free_mb;
    if (g_mode == "desktop") {
        available -= 500; // Reserve for compositor + browser
    }

    // Find the largest model that fits AND exists on disk
    for (int i = 0; MODEL_CANDIDATES[i].name != nullptr; i++) {
        if (available >= MODEL_CANDIDATES[i].required_mb) {
            std::string path = std::string("/data/models/") +
                               MODEL_CANDIDATES[i].filename;
            if (file_exists(path.c_str())) {
                g_state.model_name = MODEL_CANDIDATES[i].name;
                g_state.model_path = path;
                fprintf(stderr, "[llamaste] Selected model: %s (%s)\n",
                        g_state.model_name.c_str(), path.c_str());
                return;
            }
        }
    }

    // If no model found, check for ANY .gguf file in /data/models
    DIR* dir = opendir("/data/models");
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > 5 && name.substr(name.size() - 5) == ".gguf") {
                g_state.model_name = name;
                g_state.model_path = "/data/models/" + name;
                fprintf(stderr, "[llamaste] Found model: %s\n",
                        g_state.model_path.c_str());
                closedir(dir);
                return;
            }
        }
        closedir(dir);
    }

    fprintf(stderr, "[llamaste] WARNING: No model found in /data/models/\n");
    fprintf(stderr, "[llamaste] Place a .gguf file in /data/models/ and reboot,\n");
    fprintf(stderr, "[llamaste] or use the web UI to download one.\n");
}

// ---------------------------------------------------------------------------
// Network configuration (simple DHCP)
// ---------------------------------------------------------------------------

static void configure_network() {
    if (!is_pid1()) return;

    fprintf(stderr, "[llamaste] Configuring network...\n");

    // Bring up loopback
    system("ip link set lo up 2>/dev/null || ifconfig lo up 2>/dev/null");

    // Find first ethernet interface
    DIR* net = opendir("/sys/class/net");
    if (!net) return;

    struct dirent* entry;
    std::string iface;
    while ((entry = readdir(net)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == ".." || name == "lo") continue;

        // Check if it's a physical device (has /device symlink)
        std::string dev_path = "/sys/class/net/" + name + "/device";
        struct stat st;
        if (stat(dev_path.c_str(), &st) == 0) {
            iface = name;
            break;
        }
    }
    closedir(net);

    if (iface.empty()) {
        fprintf(stderr, "[llamaste] No network interface found\n");
        return;
    }

    fprintf(stderr, "[llamaste] Using interface: %s\n", iface.c_str());

    // Bring up interface and run DHCP
    std::string cmd;
    cmd = "ip link set " + iface + " up 2>/dev/null";
    system(cmd.c_str());

    // Try udhcpc (BusyBox DHCP client) or dhclient
    cmd = "udhcpc -i " + iface + " -s /usr/share/udhcpc/default.script "
          "-t 5 -T 3 -n -q 2>/dev/null &";
    system(cmd.c_str());

    // Give DHCP a moment
    sleep(3);

    // Read assigned IP
    cmd = "ip -4 addr show " + iface + " | grep 'inet ' | awk '{print $2}' | cut -d/ -f1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (pipe) {
        char buf[64];
        if (fgets(buf, sizeof(buf), pipe)) {
            g_state.ip_address = buf;
            // Trim newline
            while (!g_state.ip_address.empty() &&
                   g_state.ip_address.back() == '\n') {
                g_state.ip_address.pop_back();
            }
        }
        pclose(pipe);
    }

    fprintf(stderr, "[llamaste] IP address: %s\n", g_state.ip_address.c_str());
}

// ---------------------------------------------------------------------------
// CPU performance tuning
// ---------------------------------------------------------------------------

static void tune_performance() {
    if (!is_pid1()) return;

    fprintf(stderr, "[llamaste] Tuning CPU performance...\n");

    // Set CPU governor to performance
    DIR* cpu = opendir("/sys/devices/system/cpu");
    if (cpu) {
        struct dirent* entry;
        while ((entry = readdir(cpu)) != nullptr) {
            std::string name = entry->d_name;
            if (name.find("cpu") == 0 && name.size() > 3 &&
                name[3] >= '0' && name[3] <= '9') {
                std::string gov_path = "/sys/devices/system/cpu/" + name +
                                       "/cpufreq/scaling_governor";
                std::ofstream gov(gov_path);
                if (gov.is_open()) {
                    gov << "performance";
                }
            }
        }
        closedir(cpu);
    }

    // Enable THP in madvise mode
    std::ofstream thp("/sys/kernel/mm/transparent_hugepage/enabled");
    if (thp.is_open()) {
        thp << "madvise";
    }

    // Low swappiness
    std::ofstream swap("/proc/sys/vm/swappiness");
    if (swap.is_open()) {
        swap << "1";
    }
}

// ---------------------------------------------------------------------------
// Desktop mode: start Wayland compositor
// ---------------------------------------------------------------------------

static pid_t g_compositor_pid = 0;

static void start_desktop() {
    if (g_mode != "desktop") return;

    fprintf(stderr, "[llamaste] Starting desktop mode...\n");

    g_compositor_pid = fork();
    if (g_compositor_pid == 0) {
        // Child process: start cage with chromium pointing to our web UI
        // cage runs a single application fullscreen under Wayland
        // Fallback: try cage, then labwc
        setenv("XDG_RUNTIME_DIR", "/run", 1);
        setenv("WLR_LIBINPUT_NO_DEVICES", "1", 0); // Don't fail if no input

        // Try cage first (kiosk mode)
        execlp("cage", "cage", "--",
               "chromium", "--kiosk", "--no-sandbox",
               "--disable-gpu", // CPU rendering for compatibility
               "http://127.0.0.1:80",
               nullptr);

        // If cage not available, try labwc
        execlp("labwc", "labwc", nullptr);

        // If nothing works
        fprintf(stderr, "[llamaste] ERROR: No Wayland compositor available\n");
        _exit(1);
    }

    fprintf(stderr, "[llamaste] Compositor started (PID %d)\n", g_compositor_pid);
}

// ---------------------------------------------------------------------------
// Hostname setup
// ---------------------------------------------------------------------------

static void set_hostname() {
    // Read hostname from config or generate default
    std::string config_path = "/data/llamaste/config/hostname";
    std::ifstream hf(config_path);
    if (hf.is_open()) {
        std::getline(hf, g_state.hostname);
    }

    if (is_pid1()) {
        std::ofstream hn("/proc/sys/kernel/hostname");
        if (hn.is_open()) {
            hn << g_state.hostname;
        }
    }

    fprintf(stderr, "[llamaste] Hostname: %s\n", g_state.hostname.c_str());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char** argv) {
    fprintf(stderr, "\n");
    fprintf(stderr, "  ╦  ╦  ┌─┐  ┌┬┐  ┌─┐  ┌─┐  ┌┬┐  ┌─┐\n");
    fprintf(stderr, "  ║  ║  ├─┤  │││  ├─┤  └─┐   │   ├┤ \n");
    fprintf(stderr, "  ╩  ╩─╝└  ┘ ┴ ┴  ┴ ┴  └─┘   ┴   └─┘\n");
    fprintf(stderr, "  LLM IS the OS  •  v%s\n", "0.1.0");
    fprintf(stderr, "\n");

    setup_signals();

    // Phase 1: Init (only when running as PID 1)
    if (is_pid1()) {
        init_filesystems();
    }

    // Phase 2: Detect environment
    parse_cmdline();
    detect_hardware();
    set_hostname();

    // Phase 3: Performance tuning
    tune_performance();

    // Phase 4: Network
    configure_network();

    // Phase 5: Select and prepare model
    select_model();

    // Phase 6: Initialize the tool system
    ToolRegistry tools;
    tools.register_all(g_state);
    fprintf(stderr, "[llamaste] Registered %d tools\n", tools.count());

    // Phase 7: Initialize the agent
    Agent agent(g_state, tools);

    // Phase 8: Start desktop compositor (if desktop mode)
    start_desktop();

    // Phase 9: Start the HTTP server
    // This serves:
    //   /              -> Web UI (chat + dashboard)
    //   /v1/*          -> OpenAI-compatible inference API
    //   /api/agent     -> Agent chat endpoint (SSE)
    //   /api/tools/*   -> Direct tool access
    //   /health        -> Health check
    //   /metrics       -> Prometheus metrics
    fprintf(stderr, "[llamaste] Starting server on port 80...\n");
    fprintf(stderr, "[llamaste] Web UI: http://%s/\n", g_state.ip_address.c_str());
    fprintf(stderr, "[llamaste] API:    http://%s/v1/\n", g_state.ip_address.c_str());
    fprintf(stderr, "[llamaste] Ready.\n\n");

    // TODO: Integrate with llama.cpp server loop
    // For now, this is the main event loop placeholder.
    // The actual implementation will use llama-server's httplib-based
    // HTTP server with additional routes for the agent and tools.
    agent.run(g_running);

    // Shutdown
    fprintf(stderr, "[llamaste] Shutting down...\n");

    if (g_compositor_pid > 0) {
        kill(g_compositor_pid, SIGTERM);
        waitpid(g_compositor_pid, nullptr, 0);
    }

    if (is_pid1()) {
        // PID 1 shutdown: sync disks, unmount
        sync();
        umount2("/data", MNT_DETACH);
        reboot(RB_POWER_OFF);
    }

    return 0;
}
