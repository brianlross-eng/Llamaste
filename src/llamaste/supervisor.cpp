#include "supervisor.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <deque>
#include <atomic>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <termios.h>
#include <poll.h>
#include <sys/stat.h>
#include <linux/watchdog.h>
#endif

// ---------- Format helpers (testable on host) ----------

std::string format_uptime(long seconds) {
    if (seconds < 0) seconds = 0;
    long days = seconds / 86400;
    long hours = (seconds % 86400) / 3600;
    long mins = (seconds % 3600) / 60;

    char buf[64];
    if (days > 0) {
        snprintf(buf, sizeof(buf), "%ldd %ldh %ldm", days, hours, mins);
    } else if (hours > 0) {
        snprintf(buf, sizeof(buf), "%ldh %ldm", hours, mins);
    } else {
        snprintf(buf, sizeof(buf), "%ldm %lds", mins, seconds % 60);
    }
    return buf;
}

std::string format_bar(int percent, int width) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (width < 4) width = 4;

    int filled = (percent * width + 50) / 100;
    std::string bar;
    for (int i = 0; i < width; i++) {
        bar += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";  // █ and ░
    }
    return bar;
}

std::string format_bytes(long kb) {
    char buf[32];
    if (kb >= 1048576) {
        snprintf(buf, sizeof(buf), "%.1f GB", kb / 1048576.0);
    } else if (kb >= 1024) {
        snprintf(buf, sizeof(buf), "%.0f MB", kb / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%ld KB", kb);
    }
    return buf;
}

// ---------- Platform-specific supervisor code ----------

#ifndef _WIN32

static volatile sig_atomic_t g_child_exited = 0;
static volatile sig_atomic_t g_shutdown_requested = 0;
static volatile pid_t g_child_pid = 0;
static volatile sig_atomic_t g_reboot_requested = 0;  // 1 = reboot instead of poweroff

// Last child exit info — shown in console display to help diagnose crashes
static volatile int g_last_exit_code = -1;   // -1 = not yet exited
static volatile int g_last_exit_signal = 0;  // non-zero = killed by signal

// Console interaction state visible to display thread
enum ConsolePrompt { PROMPT_NONE, PROMPT_SHUTDOWN, PROMPT_REBOOT, PROMPT_DEBUG, PROMPT_PASSWD };
static volatile ConsolePrompt g_console_prompt = PROMPT_NONE;

// Password entry state (PROMPT_PASSWD mode)
// Display thread skips redraws while this is set so input isn't clobbered
static std::atomic<bool> g_passwd_entry_active{false};

// Log buffer — stderr tee'd here, displayed on 'D' keypress
static std::mutex g_log_mutex;
static std::deque<std::string> g_log_lines;
static constexpr int LOG_KEEP = 60;

// Append a line to the in-memory log buffer (safe to call from any thread)
static void log_append(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    g_log_lines.push_back(line);
    if ((int)g_log_lines.size() > LOG_KEEP)
        g_log_lines.pop_front();
}

// Start a thread that reads from a pipe fd and tees to both stderr and g_log_lines.
// Used to capture supervisor's own fprintf(stderr,...) output for the 'D' debug view.
// Path of the persistent debug log file.  Readable via HTTP once WiFi is up.
static constexpr const char* DEBUG_LOG_PATH = "/tmp/llamaste-debug.log";

static void start_stderr_tee() {
    int pipefd[2];
    if (pipe(pipefd) != 0) return;

    // Redirect stderr to write-end
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // Open the original stderr for pass-through (fd 2 was /dev/console)
    int original_console = open("/dev/console", O_WRONLY | O_NOCTTY);

    // Open persistent log file — full boot log, readable via HTTP once WiFi up
    int log_file = open(DEBUG_LOG_PATH,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    int read_fd = pipefd[0];
    std::thread([read_fd, original_console, log_file]() {
        char buf[512];
        std::string partial;
        while (true) {
            ssize_t n = read(read_fd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            // Pass through to original console
            if (original_console >= 0)
                write(original_console, buf, n);
            // Write to persistent log file
            if (log_file >= 0)
                write(log_file, buf, n);
            // Split into lines and add to in-memory ring buffer
            partial += buf;
            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos) {
                log_append(partial.substr(0, pos));
                partial = partial.substr(pos + 1);
            }
        }
        if (original_console >= 0) close(original_console);
        if (log_file >= 0) close(log_file);
    }).detach();
}

static void supervisor_sigchld(int) {
    g_child_exited = 1;
}

static void supervisor_sigterm(int) {
    g_shutdown_requested = 1;
}

static void supervisor_sigusr1(int) {
    g_reboot_requested = 1;
    g_shutdown_requested = 1;
}

extern int child_main(const SupervisorConfig& config);

static pid_t spawn_child(const SupervisorConfig& config) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[supervisor] fork failed");
        return -1;
    }
    if (pid == 0) {
        int rc = child_main(config);
        _exit(rc);
    }
    return pid;
}

static int open_watchdog() {
    // O_CLOEXEC: prevent child processes from inheriting the watchdog fd.
    // Without this, forked children (child_main, llama-server) inherit the fd.
    // If a child closes its inherited copy, and softdog_expect_close was set by
    // a 'V' write, the softdog driver could interpret the close as "clean stop".
    int fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[supervisor] No hardware watchdog available\n");
        return fd;
    }

    // Extend watchdog timeout to 300s.
    // Default softdog timeout is 60s. During slow CPU-only inference (no SIMD),
    // a single token can take several seconds. The kicker thread kicks every 100ms,
    // so it should never approach 300s. This is defense-in-depth against any
    // scheduling edge case that prevents the kicker from running for a short period.
    int timeout = 300;
#ifdef WDIOC_SETTIMEOUT
    if (ioctl(fd, WDIOC_SETTIMEOUT, &timeout) < 0) {
        fprintf(stderr, "[supervisor] WDIOC_SETTIMEOUT failed (using default timeout)\n");
    } else {
        fprintf(stderr, "[supervisor] Watchdog timeout set to %ds\n", timeout);
    }
#endif

    fprintf(stderr, "[supervisor] Hardware watchdog opened (fd=%d)\n", fd);
    return fd;
}

static void kick_watchdog(int fd) {
    if (fd >= 0) {
        write(fd, "V", 1);
    }
}

// ---------- System metric readers (for console display) ----------

struct SystemMetrics {
    int cpu_percent = 0;
    long ram_used_kb = 0;
    long ram_total_kb = 0;
    long disk_used_kb = 0;
    long disk_total_kb = 0;
    int temperature_mc = -1;  // millicelsius, -1 = unavailable
    std::string ip_address;
    std::string hostname;
    long uptime_seconds = 0;
};

static long read_proc_value(const char* path, const char* prefix) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            val = atol(line + strlen(prefix));
            break;
        }
    }
    fclose(f);
    return val;
}

static int read_cpu_percent() {
    // Read /proc/stat twice with a small delay
    auto read_cpu = []() -> long long {
        FILE* f = fopen("/proc/stat", "r");
        if (!f) return -1;
        long long user, nice, system, idle, iowait, irq, softirq;
        if (fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld",
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 7) {
            fclose(f);
            return -1;
        }
        fclose(f);
        long long total = user + nice + system + idle + iowait + irq + softirq;
        long long busy = total - idle - iowait;
        return (busy << 32) | (total & 0xFFFFFFFF);
    };

    long long s1 = read_cpu();
    usleep(100000);  // 100ms
    long long s2 = read_cpu();

    if (s1 < 0 || s2 < 0) return 0;
    long long busy1 = s1 >> 32, total1 = s1 & 0xFFFFFFFF;
    long long busy2 = s2 >> 32, total2 = s2 & 0xFFFFFFFF;
    long long dt = total2 - total1;
    if (dt == 0) return 0;
    return (int)(100 * (busy2 - busy1) / dt);
}

static void read_memory(long& used_kb, long& total_kb) {
    total_kb = read_proc_value("/proc/meminfo", "MemTotal:");
    long available = read_proc_value("/proc/meminfo", "MemAvailable:");
    if (total_kb > 0 && available > 0) {
        used_kb = total_kb - available;
    } else {
        used_kb = 0;
    }
}

static void read_disk(long& used_kb, long& total_kb) {
    struct statvfs st;
    const char* path = "/data";
    if (statvfs(path, &st) != 0) {
        path = "/";
        if (statvfs(path, &st) != 0) {
            used_kb = total_kb = 0;
            return;
        }
    }
    total_kb = (long)(st.f_blocks * st.f_frsize / 1024);
    long free_kb = (long)(st.f_bfree * st.f_frsize / 1024);
    used_kb = total_kb - free_kb;
}

static int read_temperature() {
    FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return -1;
    int mc = 0;
    if (fscanf(f, "%d", &mc) != 1) mc = -1;
    fclose(f);
    return mc;
}

static std::string read_ip_address() {
    // Try common interface names
    const char* ifaces[] = {"eth0", "enp0s3", "ens33", "wlan0", nullptr};
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "unknown";

    std::string result = "no network";
    for (int i = 0; ifaces[i]; i++) {
        struct ifreq ifr = {};
        strncpy(ifr.ifr_name, ifaces[i], IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
            auto* addr = (struct sockaddr_in*)&ifr.ifr_addr;
            result = inet_ntoa(addr->sin_addr);
            break;
        }
    }
    close(sock);
    return result;
}

static SystemMetrics gather_metrics() {
    SystemMetrics m;
    m.cpu_percent = read_cpu_percent();
    read_memory(m.ram_used_kb, m.ram_total_kb);
    read_disk(m.disk_used_kb, m.disk_total_kb);
    m.temperature_mc = read_temperature();
    m.ip_address = read_ip_address();

    char hn[128] = {};
    gethostname(hn, sizeof(hn) - 1);
    m.hostname = hn;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    m.uptime_seconds = ts.tv_sec;

    return m;
}

// ---------- Console input thread (keyboard controls) ----------

// Send a raw HTTP POST to 127.0.0.1:80 and return the response body
static std::string http_post_localhost(const char* path, const std::string& body) {
#ifndef _WIN32
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "socket error";

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(0x7f000001); // 127.0.0.1

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return "connect error";
    }

    char req[2048];
    int reqlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.0\r\n"
        "Host: localhost\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "\r\n"
        "%s",
        path, body.size(), body.c_str());

    if (write(sock, req, reqlen) < 0) { close(sock); return "write error"; }

    std::string resp;
    char buf[256];
    ssize_t n;
    while ((n = read(sock, buf, sizeof(buf))) > 0)
        resp.append(buf, n);
    close(sock);

    // Extract body after \r\n\r\n
    auto pos = resp.find("\r\n\r\n");
    if (pos != std::string::npos) return resp.substr(pos + 4);
    return resp;
#else
    return "unsupported";
#endif
}

// Read a password string from the console fd (raw mode, echo as *)
// Writes prompt to write_fd. Returns the entered string.
static std::string console_read_password(int read_fd, int write_fd, const char* prompt) {
    std::string pw;
    // Write prompt
    write(write_fd, prompt, strlen(prompt));

    char ch;
    while (true) {
        struct pollfd pfd = { read_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 100) <= 0) continue; // 100ms timeout — stays responsive
        if (read(read_fd, &ch, 1) != 1) continue;

        if (ch == '\n' || ch == '\r') {
            write(write_fd, "\r\n", 2);
            break;
        } else if (ch == 127 || ch == '\b') { // Backspace / DEL
            if (!pw.empty()) {
                pw.pop_back();
                write(write_fd, "\b \b", 3); // erase last *
            }
        } else if (ch == 27) { // Escape — cancel
            write(write_fd, " [cancelled]\r\n", 14);
            return "";
        } else if (ch >= 32 && ch < 127) { // Printable
            pw += ch;
            write(write_fd, "*", 1);
        }
    }
    return pw;
}

// Handle 'P' key: console-based password setup, bypasses Wayland input
// read_fd: O_RDONLY fd for keyboard input (from console_input_thread's fd)
// write_fd: O_WRONLY fd for text output to screen
static void do_console_passwd_setup(int read_fd, int write_fd) {
#ifndef _WIN32
    g_passwd_entry_active = true;

    // Clear line and show header
    const char* hdr = "\r\n\033[1m=== Llamaste Password Setup ===\033[0m\r\n"
                      "(Keyboard not working in browser? Set password here)\r\n\r\n";
    write(write_fd, hdr, strlen(hdr));

    std::string pw  = console_read_password(read_fd, write_fd, "New password (min 4 chars): ");
    if (pw.empty()) { g_passwd_entry_active = false; return; }
    if (pw.size() < 4) {
        const char* err = "Password too short (min 4).\r\n";
        write(write_fd, err, strlen(err));
        g_passwd_entry_active = false;
        return;
    }

    std::string pw2 = console_read_password(read_fd, write_fd, "Confirm password:           ");
    if (pw != pw2) {
        const char* err = "Passwords don't match. Try again (press P).\r\n";
        write(write_fd, err, strlen(err));
        g_passwd_entry_active = false;
        return;
    }

    // Build JSON and POST to the HTTP server
    std::string json_body = "{\"password\":\"" + pw + "\"}";
    const char* sending = "Setting password... ";
    write(write_fd, sending, strlen(sending));

    std::string resp = http_post_localhost("/llamaste/auth/setup", json_body);

    if (resp.find("\"success\"") != std::string::npos ||
        resp.find("true") != std::string::npos) {
        const char* ok = "OK!\r\nPassword set. Refresh the browser or press Enter in the web UI.\r\n";
        write(write_fd, ok, strlen(ok));
    } else {
        // Show truncated response for diagnosis
        std::string msg = "Response: " + resp.substr(0, 120) + "\r\n";
        write(write_fd, msg.c_str(), msg.size());
    }

    g_passwd_entry_active = false;
    g_console_prompt = PROMPT_NONE;
#endif
}

static void console_input_thread() {
    fprintf(stderr, "[supervisor] Console input thread started\n");

    // Open console for reading — try /dev/tty0 (VGA) first
    int fd = open("/dev/tty0", O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        fd = open("/dev/console", O_RDONLY | O_NOCTTY);
    }
    if (fd < 0) {
        fprintf(stderr, "[supervisor] Cannot open any console for input: %m\n");
        return;
    }
    fprintf(stderr, "[supervisor] Console input fd=%d\n", fd);

    // Set raw terminal mode (no echo, no canonical/line buffering)
    struct termios raw;
    if (tcgetattr(fd, &raw) == 0) {
        raw.c_lflag &= ~(ECHO | ICANON | ISIG);  // No echo, no line buffering, no signals
        raw.c_cc[VMIN] = 0;   // Non-blocking
        raw.c_cc[VTIME] = 0;
        tcsetattr(fd, TCSANOW, &raw);
    }

    struct pollfd pfd = { fd, POLLIN, 0 };

    while (!g_shutdown_requested) {
        // Poll with 200ms timeout so we check g_shutdown_requested regularly
        int ret = poll(&pfd, 1, 200);
        if (ret <= 0) continue;

        char ch = 0;
        if (read(fd, &ch, 1) != 1) continue;

        if (g_console_prompt == PROMPT_NONE) {
            // Normal mode: S=shutdown, R=reboot, D=debug log, P=set password
            if (ch == 'S' || ch == 's') {
                g_console_prompt = PROMPT_SHUTDOWN;
            } else if (ch == 'R' || ch == 'r') {
                g_console_prompt = PROMPT_REBOOT;
            } else if (ch == 'D' || ch == 'd') {
                g_console_prompt = PROMPT_DEBUG;
            } else if (ch == 'P' || ch == 'p') {
                // fd is O_RDONLY (keyboard input); open a separate O_WRONLY fd for output.
                // Passing fd as read_fd and wfd as write_fd keeps them distinct.
                int wfd = open("/dev/tty0", O_WRONLY | O_NOCTTY);
                if (wfd < 0) wfd = open("/dev/console", O_WRONLY | O_NOCTTY);
                g_console_prompt = PROMPT_PASSWD;
                do_console_passwd_setup(fd, wfd >= 0 ? wfd : fd);
                if (wfd >= 0) close(wfd);
                // do_console_passwd_setup resets g_console_prompt itself
            }
        } else if (g_console_prompt == PROMPT_DEBUG) {
            // Any key exits debug view
            g_console_prompt = PROMPT_NONE;
        } else {
            // Confirmation mode: Y=confirm, N/anything else=cancel
            if (ch == 'Y' || ch == 'y') {
                if (g_console_prompt == PROMPT_REBOOT) {
                    g_reboot_requested = 1;
                }
                g_shutdown_requested = 1;
                // Wake up the supervisor's waitpid() so it can reach reboot()
                if (g_child_pid > 0) kill(g_child_pid, SIGTERM);
            } else {
                // Cancel — any key other than Y cancels
                g_console_prompt = PROMPT_NONE;
            }
        }
    }

    close(fd);
    fprintf(stderr, "[supervisor] Console input thread stopped\n");
}

// ---------- Console display thread ----------

static void console_display_thread(const SupervisorConfig& config) {
    fprintf(stderr, "[supervisor] Console display thread started\n");

    // Wait a moment for system to settle
    sleep(3);

    // Open console fd once — try /dev/tty0 (VGA) first, then /dev/console
    int console_fd = open("/dev/tty0", O_WRONLY | O_NOCTTY);
    if (console_fd < 0) {
        console_fd = open("/dev/console", O_WRONLY | O_NOCTTY);
        fprintf(stderr, "[supervisor] /dev/tty0 failed, /dev/console fd=%d\n", console_fd);
    } else {
        fprintf(stderr, "[supervisor] Opened /dev/tty0 fd=%d for display\n", console_fd);
    }
    if (console_fd < 0) {
        fprintf(stderr, "[supervisor] Cannot open any console device: %m\n");
        return;
    }

    // Extract model name from path (e.g. "/data/models/qwen.gguf" -> "qwen.gguf")
    std::string model_name = "none";
    if (!config.model_path.empty()) {
        size_t pos = config.model_path.rfind('/');
        model_name = (pos != std::string::npos)
            ? config.model_path.substr(pos + 1)
            : config.model_path;
        // Trim .gguf extension for display
        if (model_name.size() > 5 && model_name.substr(model_name.size() - 5) == ".gguf") {
            model_name = model_name.substr(0, model_name.size() - 5);
        }
    }

    while (!g_shutdown_requested) {
        // Pause display while password is being entered on the console
        if (g_passwd_entry_active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        SystemMetrics m = gather_metrics();

        int ram_pct = (m.ram_total_kb > 0) ? (int)(100 * m.ram_used_kb / m.ram_total_kb) : 0;
        int disk_pct = (m.disk_total_kb > 0) ? (int)(100 * m.disk_used_kb / m.disk_total_kb) : 0;

        std::string ram_bar = format_bar(ram_pct);
        std::string cpu_bar = format_bar(m.cpu_percent);
        std::string disk_bar = format_bar(disk_pct);
        std::string uptime = format_uptime(m.uptime_seconds);
        std::string ram_str = format_bytes(m.ram_used_kb) + " / " + format_bytes(m.ram_total_kb);
        std::string disk_str = format_bytes(m.disk_used_kb) + " / " + format_bytes(m.disk_total_kb);

        std::string temp_str;
        if (m.temperature_mc >= 0) {
            char tmp[16];
            snprintf(tmp, sizeof(tmp), "%d\xc2\xb0""C", m.temperature_mc / 1000);
            temp_str = tmp;
        } else {
            temp_str = "N/A";
        }

        // Determine child status
        const char* status_str = (g_child_pid > 0 && !g_child_exited)
            ? "\033[32mRUNNING\033[0m" : "\033[31mDOWN\033[0m";

        // Build the display using string builder
        std::string out;
        out.reserve(2048);

        // VT100: clear screen + cursor home
        out += "\033[2J\033[H";

        // Box drawing chars
        const char* TL = "\xe2\x95\x94";  // ╔
        const char* TR = "\xe2\x95\x97";  // ╗
        const char* BL = "\xe2\x95\x9a";  // ╚
        const char* BR = "\xe2\x95\x9d";  // ╝
        const char* H  = "\xe2\x95\x90";  // ═
        const char* V  = "\xe2\x95\x91";  // ║
        const char* ML = "\xe2\x95\xa0";  // ╠
        const char* MR = "\xe2\x95\xa3";  // ╣

        auto hline = [&](const char* left, const char* right, int w) {
            out += left;
            for (int i = 0; i < w; i++) out += H;
            out += right;
            out += "\n";
        };

        auto padded = [&](const char* text, int w) {
            // Write text left-aligned in a field of width w
            int len = (int)strlen(text);
            out += V;
            out += "  ";
            out += text;
            for (int i = len + 2; i < w; i++) out += " ";
            out += V;
            out += "\n";
        };

        int W = 50;  // inner width

        // Title — show actual boot mode
        hline(TL, TR, W);
        {
            const char* mode_label = "SERVER";
            if (config.boot_mode == "desktop") mode_label = "DESKTOP";
            else if (config.boot_mode == "live") mode_label = "LIVE";

            char title[64];
            snprintf(title, sizeof(title), "LLAMASTE  %s", mode_label);
            int tlen = (int)strlen(title);
            int pad_left = (W - tlen) / 2;
            int pad_right = W - tlen - pad_left;
            out += V;
            out += "\033[1m";  // Bold
            for (int i = 0; i < pad_left; i++) out += " ";
            out += title;
            for (int i = 0; i < pad_right; i++) out += " ";
            out += "\033[0m";
            out += V;
            out += "\n";
        }
        hline(ML, MR, W);

        // Pivot status (is squashfs overlay mounted? labwc only exists in squashfs)
        bool pivot_done = (access("/usr/bin/labwc", F_OK) == 0 ||
                           access("/usr/sbin/wpa_supplicant", F_OK) == 0);

        // Status
        {
            char buf[128];
            snprintf(buf, sizeof(buf), "Status:   %s", status_str);
            // Status has ANSI codes so we can't use padded() directly
            int visible_len = 10 + (g_child_pid > 0 && !g_child_exited ? 7 : 4);
            out += V;
            out += "  ";
            out += buf;
            for (int i = visible_len + 2; i < W; i++) out += " ";
            out += V;
            out += "\n";
        }

        // Pivot + last exit info
        {
            char buf[80];
            if (g_last_exit_signal > 0) {
                snprintf(buf, sizeof(buf), "Exit:     signal %d  Pivot: %s",
                         g_last_exit_signal, pivot_done ? "YES" : "NO");
            } else if (g_last_exit_code >= 0) {
                snprintf(buf, sizeof(buf), "Exit:     code %d  Pivot: %s",
                         g_last_exit_code, pivot_done ? "YES" : "NO");
            } else {
                snprintf(buf, sizeof(buf), "Pivot:    %s", pivot_done ? "YES" : "NO");
            }
            padded(buf, W);
        }

        // Metrics
        char line_buf[128];

        snprintf(line_buf, sizeof(line_buf), "Uptime:   %s", uptime.c_str());
        padded(line_buf, W);

        snprintf(line_buf, sizeof(line_buf), "CPU:      %s  %d%%",
                 cpu_bar.c_str(), m.cpu_percent);
        padded(line_buf, W);

        snprintf(line_buf, sizeof(line_buf), "RAM:      %s  %s",
                 ram_bar.c_str(), ram_str.c_str());
        padded(line_buf, W);

        snprintf(line_buf, sizeof(line_buf), "Disk:     %s  %s",
                 disk_bar.c_str(), disk_str.c_str());
        padded(line_buf, W);

        snprintf(line_buf, sizeof(line_buf), "Temp:     %s", temp_str.c_str());
        padded(line_buf, W);

        snprintf(line_buf, sizeof(line_buf), "IP:       %s", m.ip_address.c_str());
        padded(line_buf, W);

        snprintf(line_buf, sizeof(line_buf), "Hostname: %s.local", m.hostname.c_str());
        padded(line_buf, W);

        // Trim model name if too long
        std::string model_display = model_name;
        if (model_display.size() > 32) {
            model_display = model_display.substr(0, 29) + "...";
        }
        snprintf(line_buf, sizeof(line_buf), "Model:    %s", model_display.c_str());
        padded(line_buf, W);

        snprintf(line_buf, sizeof(line_buf), "Web UI:   http://%s", m.ip_address.c_str());
        padded(line_buf, W);

        hline(ML, MR, W);

        // Bottom info — key hints, confirmation prompt, or debug log
        ConsolePrompt prompt = g_console_prompt;
        if (prompt == PROMPT_DEBUG) {
            // Debug view: show last log lines, any key to exit
            hline(BL, BR, W);
            // Switch to full-screen debug output
            out += "\033[2J\033[H";
            out += "\033[1m=== LLAMASTE DEBUG LOG (any key to exit) ===\033[0m\n";
            // Last 15 lines of supervisor log
            {
                std::lock_guard<std::mutex> lk(g_log_mutex);
                int start = (int)g_log_lines.size() > 15
                            ? (int)g_log_lines.size() - 15 : 0;
                for (int i = start; i < (int)g_log_lines.size(); i++) {
                    const std::string& l = g_log_lines[i];
                    out += (l.size() > 78 ? l.substr(0, 78) : l) + "\n";
                }
            }
            // Last 15 lines of child log
            out += "\033[1m--- child ---\033[0m\n";
            {
                FILE* cf = fopen("/tmp/child.log", "r");
                if (cf) {
                    // Seek to last ~1200 bytes
                    fseek(cf, 0, SEEK_END);
                    long sz = ftell(cf);
                    if (sz > 1200) fseek(cf, sz - 1200, SEEK_SET);
                    else rewind(cf);
                    char cbuf[1300];
                    size_t nr = fread(cbuf, 1, sizeof(cbuf) - 1, cf);
                    fclose(cf);
                    cbuf[nr] = '\0';
                    // Find first newline (may be mid-line after seek)
                    char* start_ptr = (char*)memchr(cbuf, '\n', nr);
                    const char* p = start_ptr ? start_ptr + 1 : cbuf;
                    // Output, truncating long lines
                    const char* line_start = p;
                    for (const char* c = p; *c; c++) {
                        if (*c == '\n' || *(c + 1) == '\0') {
                            int len = (int)(c - line_start + (*c == '\n' ? 0 : 1));
                            if (len > 78) len = 78;
                            out.append(line_start, len);
                            out += "\n";
                            line_start = c + 1;
                        }
                    }
                } else {
                    out += "(no child log yet)\n";
                }
            }
        } else if (prompt == PROMPT_SHUTDOWN) {
            // Shutdown confirmation
            const char* confirm_text = "\033[33m  Shutdown?  Press [Y] to confirm, any key to cancel\033[0m";
            int visible_len = 53;  // visible chars without ANSI
            out += V;
            out += confirm_text;
            for (int i = visible_len; i < W; i++) out += " ";
            out += V;
            out += "\n";
            hline(BL, BR, W);
        } else if (prompt == PROMPT_REBOOT) {
            // Reboot confirmation
            const char* confirm_text = "\033[33m  Reboot?    Press [Y] to confirm, any key to cancel\033[0m";
            int visible_len = 53;
            out += V;
            out += confirm_text;
            for (int i = visible_len; i < W; i++) out += " ";
            out += V;
            out += "\n";
            hline(BL, BR, W);
        } else {
            // Normal: show available controls
            snprintf(line_buf, sizeof(line_buf), "[S] Shutdown  [R] Reboot  [D] Debug  [P] Set Password");
            int len = (int)strlen(line_buf);
            int pad_left = (W - len) / 2;
            int pad_right = W - len - pad_left;
            out += V;
            for (int i = 0; i < pad_left; i++) out += " ";
            out += "\033[36m";  // Cyan for key hints
            out += line_buf;
            out += "\033[0m";
            for (int i = 0; i < pad_right; i++) out += " ";
            out += V;
            out += "\n";
            hline(BL, BR, W);
        }

        // Write to console
        ssize_t wr = write(console_fd, out.c_str(), out.size());
        if (wr < 0) {
            fprintf(stderr, "[supervisor] Console write failed: %m\n");
        }

        // Sleep ~5s normally, but wake immediately if prompt state changes
        for (int i = 0; i < 10 && !g_shutdown_requested; i++) {
            usleep(500000);
            if (g_console_prompt != prompt) break;  // Key pressed — redraw now
        }
    }

    close(console_fd);
    fprintf(stderr, "[supervisor] Console display thread stopped\n");
}

// ---------- Dedicated watchdog kicker thread ----------
//
// Runs independently of the main supervisor loop.
// Kicks the watchdog every 1 second regardless of what the main thread is
// doing (e.g. blocked in waitpid(), fprintf(), or sleep()).
//
// All signals are blocked in this thread so that:
// (a) SA_RESTART on SIGCHLD cannot cause sleep(1) to restart indefinitely
// (b) Signal handlers run in the main thread where g_child_exited is checked
//
static int g_watchdog_fd = -1;  // set before kicker thread starts

static void watchdog_kicker_thread() {
    // Block ALL signals — signals must only run in the main thread.
    sigset_t all_sigs;
    sigfillset(&all_sigs);
    pthread_sigmask(SIG_BLOCK, &all_sigs, nullptr);

    // Open a persistent diagnostic log. Written on every kick so we can
    // check after a reboot exactly how many kicks happened and when they stopped.
    int log_fd = open("/data/watchdog-kicker.log",
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);

    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "[kicker] STARTED fd=%d log_fd=%d\n",
                     g_watchdog_fd, log_fd);
    write(STDERR_FILENO, buf, n);
    if (log_fd >= 0) write(log_fd, buf, n);

    int kick_count = 0;
    while (!g_shutdown_requested) {
        // Kick: write any byte to reset the softdog timer.
        // Using '1' (not 'V') so we don't accidentally prime "clean close" flag
        // in the softdog driver if some child fd gets closed unexpectedly.
        int wr = -2;
        if (g_watchdog_fd >= 0) {
            wr = (int)write(g_watchdog_fd, "1", 1);
        }
        kick_count++;

        // Log every 30 kicks (~3 seconds) to the persistent file.
        // Log errors immediately to serial.
        if (log_fd >= 0 && kick_count % 30 == 0) {
            n = snprintf(buf, sizeof(buf),
                         "[kicker] kick=%d wr=%d fd=%d\n",
                         kick_count, wr, g_watchdog_fd);
            write(log_fd, buf, n);
        }
        if (wr < 0 && g_watchdog_fd >= 0) {
            n = snprintf(buf, sizeof(buf),
                         "[kicker] WRITE ERROR kick=%d errno=%d\n",
                         kick_count, errno);
            write(STDERR_FILENO, buf, n);
            if (log_fd >= 0) write(log_fd, buf, n);
        }

        // 100ms sleep — 10 kicks/second, well within any watchdog timeout.
        usleep(100000);
    }

    n = snprintf(buf, sizeof(buf),
                 "[kicker] EXITING kicks=%d shutdown=%d\n",
                 kick_count, g_shutdown_requested);
    write(STDERR_FILENO, buf, n);
    if (log_fd >= 0) { write(log_fd, buf, n); close(log_fd); }

    // One final kick for shutdown margin
    kick_watchdog(g_watchdog_fd);
}

// ---------------------------------------------------------------------------
// Console WiFi pre-flight
// ---------------------------------------------------------------------------
// Called BEFORE the display thread starts so the terminal is uncontested.
// Detects WiFi hardware, checks for saved networks, and shows SSID/PSK
// prompts if this is the first boot in server mode.
// wpa_supplicant is NOT running yet — child_main starts it with the saved
// config, so we skip the wpa_cli reconfigure step here.
// ---------------------------------------------------------------------------
#ifndef _WIN32
static std::string supervisor_detect_wifi_iface() {
    DIR* d = opendir("/sys/class/net");
    if (!d) return "";
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        char phy[256];
        snprintf(phy, sizeof(phy), "/sys/class/net/%s/phy80211", de->d_name);
        struct stat st;
        if (stat(phy, &st) == 0) {
            std::string iface = de->d_name;
            closedir(d);
            return iface;
        }
    }
    closedir(d);
    return "";
}

static bool supervisor_wifi_has_saved_networks() {
    FILE* f = fopen("/data/llamaste/wifi/wpa.conf", "r");
    if (!f) return false;
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "network={")) { found = true; break; }
    }
    fclose(f);
    return found;
}

static void supervisor_console_wifi_setup(const std::string& iface) {
    int tty = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty < 0)
        tty = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (tty < 0) {
        fprintf(stderr, "[wifi] console_wifi_setup: cannot open tty: %m\n");
        return;
    }

    struct termios old_tio, raw_tio;
    tcgetattr(tty, &old_tio);
    raw_tio = old_tio;
    cfmakeraw(&raw_tio);
    raw_tio.c_oflag |= OPOST | ONLCR;
    tcsetattr(tty, TCSAFLUSH, &raw_tio);

    auto wstr = [&](const char* s) { write(tty, s, strlen(s)); };

    wstr("\033[2J\033[H\r\n");
    wstr("  +--------------------------------------------------+\r\n");
    wstr("  |           Llamaste  -  WiFi Setup                |\r\n");
    wstr("  +--------------------------------------------------+\r\n");
    wstr("\r\n");
    wstr("  WiFi hardware detected: ");
    wstr(iface.c_str());
    wstr("\r\n");
    wstr("  No saved networks found.  Enter credentials below,\r\n");
    wstr("  or press Enter to skip and use the web UI later.\r\n");
    wstr("\r\n");

    auto read_line = [&](bool echo_chars, size_t max_len) -> std::string {
        std::string s;
        char c;
        while (read(tty, &c, 1) == 1) {
            if (c == '\r' || c == '\n') { wstr("\r\n"); break; }
            if (c == 3 || c == 4)      { s.clear(); wstr("\r\n"); break; }
            if ((c == 127 || c == '\b') && !s.empty()) {
                s.pop_back(); wstr("\b \b"); continue;
            }
            if (c >= 0x20 && c < 0x7f && s.size() < max_len) {
                s += c;
                if (echo_chars) write(tty, &c, 1); else wstr("*");
            }
        }
        return s;
    };

    wstr("  SSID     : ");
    std::string ssid = read_line(true, 63);

    if (ssid.empty()) {
        wstr("  Skipping WiFi setup.\r\n\r\n");
        tcsetattr(tty, TCSAFLUSH, &old_tio);
        close(tty);
        return;
    }

    wstr("  Password : ");
    std::string psk = read_line(false, 63);
    wstr("\r\n");

    tcsetattr(tty, TCSAFLUSH, &old_tio);
    close(tty);

    // Ensure data directory exists (child_main normally creates it, but we
    // run before the child starts).
    mkdir("/data", 0755);
    mkdir("/data/llamaste", 0755);
    mkdir("/data/llamaste/wifi", 0755);

    const char* conf = "/data/llamaste/wifi/wpa.conf";
    // Write the full config — header + network block.  spawn_wpa_supplicant()
    // skips the skeleton if the file already exists, so we include
    // ctrl_interface here or wpa_supplicant won't create its control socket.
    FILE* f = fopen(conf, "w");
    if (!f) {
        fprintf(stderr, "[wifi] console_wifi_setup: cannot write wpa.conf: %m\n");
        return;
    }
    fprintf(f, "ctrl_interface=/run/wpa_supplicant\n");
    fprintf(f, "ctrl_interface_group=0\n");
    fprintf(f, "update_config=1\n");
    fprintf(f, "\n");
    fprintf(f, "network={\n");
    fprintf(f, "    ssid=\"%s\"\n", ssid.c_str());
    fprintf(f, "    psk=\"%s\"\n",  psk.c_str());
    fprintf(f, "    key_mgmt=WPA-PSK\n");
    fprintf(f, "}\n");
    fclose(f);
    fprintf(stderr, "[wifi] console_wifi_setup: saved SSID '%s'\n", ssid.c_str());
    // wpa_supplicant not yet running — child_main will start it with the saved config.
}

static void supervisor_preflight_wifi(const SupervisorConfig& config) {
    if (config.boot_mode == "desktop") return;
    std::string iface = supervisor_detect_wifi_iface();
    if (iface.empty()) return;
    if (supervisor_wifi_has_saved_networks()) return;
    fprintf(stderr, "[wifi] No saved networks — showing console setup (iface=%s)\n",
            iface.c_str());
    supervisor_console_wifi_setup(iface);
}
#endif

// ---------- Main supervisor loop ----------

[[noreturn]] void supervisor_run(const SupervisorConfig& config) {
    struct sigaction sa = {};
    sa.sa_handler = supervisor_sigchld;
    sigemptyset(&sa.sa_mask);
    // SA_RESTART: restart interrupted syscalls (e.g. sleep) after signal.
    // SA_NOCLDSTOP: do NOT deliver SIGCHLD when child is merely stopped
    // (SIGSTOP) or continued (SIGCONT). Without SA_NOCLDSTOP, a stop event
    // would set g_child_exited=1 and exit the kick loop even though the child
    // is still alive, leaving waitpid() to block indefinitely with no kicks.
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, nullptr);

    sa.sa_handler = supervisor_sigterm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    sa.sa_handler = supervisor_sigusr1;
    sigaction(SIGUSR1, &sa, nullptr);

    g_watchdog_fd = open_watchdog();
    int crash_count = 0;
    time_t last_crash = 0;

    // Dedicated watchdog kicker thread: runs for the entire supervisor lifetime.
    // Kicks g_watchdog_fd every 1 second with all signals blocked, so it can
    // never be interrupted by SA_RESTART, blocked by fprintf(), or stalled by
    // waitpid() in the main thread.
    std::thread kicker_thread(watchdog_kicker_thread);
    kicker_thread.detach();

    // Tee supervisor stderr to in-memory log buffer (for 'D' debug view on console)
    start_stderr_tee();

    // First-boot WiFi setup: run before display thread so the terminal is clean.
    // Shows SSID/PSK prompts if server mode + WiFi hardware + no saved networks.
#ifndef _WIN32
    supervisor_preflight_wifi(config);
#endif

    // Start console display + input threads (all boot modes)
    std::thread display_thread(console_display_thread, config);
    display_thread.detach();
    std::thread input_thread(console_input_thread);
    input_thread.detach();

    while (!g_shutdown_requested) {
        fprintf(stderr, "[supervisor] Spawning inference child...\n");
        g_child_exited = 0;
        g_child_pid = spawn_child(config);

        if (g_child_pid < 0) {
            fprintf(stderr, "[supervisor] Failed to spawn child, retrying in 5s\n");
            sleep(5);
            continue;
        }

        fprintf(stderr, "[supervisor] Child PID %d running\n", g_child_pid);

        // Wait for child to exit.
        // The dedicated kicker thread handles watchdog kicks, so this waitpid()
        // can block indefinitely without risking the softdog timeout.
        int status = 0;
        waitpid(g_child_pid, &status, 0);
        g_child_exited = 1;

        fprintf(stderr, "[supervisor] Child exited (status=%d)\n", status);

        // Record exit reason for console display
        if (WIFEXITED(status)) {
            g_last_exit_code = WEXITSTATUS(status);
            g_last_exit_signal = 0;
            fprintf(stderr, "[supervisor] Child exited with code %d\n",
                    WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            g_last_exit_code = -1;
            g_last_exit_signal = WTERMSIG(status);
            fprintf(stderr, "[supervisor] Child killed by signal %d\n",
                    WTERMSIG(status));
        }

        if (g_shutdown_requested) break;

        time_t now = time(nullptr);
        if (now - last_crash < 60) {
            crash_count++;
        } else {
            crash_count = 1;
        }
        last_crash = now;

        if (crash_count > 3) {
            fprintf(stderr,
                "[supervisor] Too many crashes, waiting 30s before restart\n");
            // Kick watchdog during restart delay (30s > 60s timeout risk)
            for (int i = 0; i < 30; i++) {
                kick_watchdog(g_watchdog_fd);
                sleep(1);
            }
            crash_count = 0;
        } else {
            kick_watchdog(g_watchdog_fd);
            sleep(2);
        }
    }

    if (g_reboot_requested) {
        fprintf(stderr, "[supervisor] Rebooting...\n");
    } else {
        fprintf(stderr, "[supervisor] Shutting down...\n");
    }

    if (g_child_pid > 0) {
        kill(g_child_pid, SIGTERM);
        int status;
        alarm(10);
        waitpid(g_child_pid, &status, 0);
        alarm(0);
    }

    if (g_watchdog_fd >= 0) {
        write(g_watchdog_fd, "V", 1);
        close(g_watchdog_fd);
    }

    sync();
    umount2("/data", MNT_DETACH);

    if (g_reboot_requested) {
        reboot(RB_AUTOBOOT);
    } else {
        reboot(RB_POWER_OFF);
    }
    _exit(0);
}

#endif // _WIN32
