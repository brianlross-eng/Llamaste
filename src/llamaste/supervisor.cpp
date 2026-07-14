#include "supervisor.h"
#include "init.h"
#include "version.h"
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
#include <vector>
#include <algorithm>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/statvfs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <ifaddrs.h>
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

    // Save original stderr before the dup2 so error fprintf in the tee
    // thread doesn't feed back into the pipe (stderr feedback loop fix).
    int orig_stderr = dup(STDERR_FILENO);

    // Redirect stderr to write-end
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    // Open the original stderr for pass-through (fd 2 was /dev/console)
    int original_console = open("/dev/console", O_WRONLY | O_NOCTTY);

    // Open persistent log file — full boot log, readable via HTTP once WiFi up
    int log_file = open(DEBUG_LOG_PATH,
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);

    int read_fd = pipefd[0];
    g_tee_thread = std::thread([read_fd, original_console, log_file, orig_stderr]() {
        char buf[512];
        std::string partial;
        while (true) {
            ssize_t n = read(read_fd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            // Pass through to original console
            if (original_console >= 0) {
                ssize_t written = write(original_console, buf, n);
                if (written < 0) {
                    dprintf(orig_stderr, "[tee] console write error: %s\n", strerror(errno));
                }
            }
            // Write to persistent log file
            if (log_file >= 0) {
                ssize_t written = write(log_file, buf, n);
                if (written < 0) {
                    dprintf(orig_stderr, "[tee] log write error: %s (path=%s)\n",
                            strerror(errno), DEBUG_LOG_PATH);
                }
            }
            // Split into lines and add to in-memory ring buffer
            partial += buf;
            // Cap unbounded growth: if a misbehaving process produces
            // newline-free output (crash dump, binary garbage), flush with
            // a truncation marker before partial balloons to OOM territory.
            static constexpr size_t MAX_PARTIAL = 65536;  // 64 KB
            if (__builtin_expect(partial.size() > MAX_PARTIAL, 0)) {
                log_append(partial + "\xe2\x80\xa6[line truncated]");
                partial.clear();
            }
            size_t pos;
            while ((pos = partial.find('\n')) != std::string::npos) {
                log_append(partial.substr(0, pos));
                partial = partial.substr(pos + 1);
            }
        }
        if (original_console >= 0) close(original_console);
        if (log_file >= 0) close(log_file);
        if (orig_stderr >= 0) close(orig_stderr);
    });
}

static void supervisor_sigchld(int) {
    int status;
    // Reap the child immediately in the signal handler to capture the
    // exit status before another waitpid() call could harvest the zombie.
    // This closes the race between async SIGCHLD delivery and the main
    // loop's waitpid(-1).  WNOHANG prevents blocking the signal handler.
    if (g_child_pid > 0) {
        pid_t result = waitpid(g_child_pid, &status, WNOHANG);
        if (result == g_child_pid) {
            g_child_exited = 1;
            g_last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            g_last_exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        }
    }
}

static void supervisor_sigterm(int) {
    g_shutdown_requested = 1;
    // Kill the child so waitpid() returns.  Without this, SA_RESTART
    // causes waitpid() to silently restart and the supervisor never
    // breaks out of the wait loop.
    if (g_child_pid > 0) kill(g_child_pid, SIGTERM);
}

static void supervisor_sigusr1(int) {
    g_reboot_requested = 1;
    g_shutdown_requested = 1;
    if (g_child_pid > 0) kill(g_child_pid, SIGTERM);
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
    // Scan all network interfaces via getifaddrs() — no hardcoded names.
    // Priority: wired ethernet > WiFi > anything else (skip loopback).
    struct ifaddrs* ifa_list = nullptr;
    if (getifaddrs(&ifa_list) != 0) return "unknown";

    std::string wired_ip, wifi_ip, other_ip;
    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        auto* sa = (struct sockaddr_in*)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));

        // Skip link-local (169.254.x.x)
        if (strncmp(ip, "169.254.", 8) == 0) continue;

        // Classify by interface name
        const char* name = ifa->ifa_name;
        bool is_wifi = (strncmp(name, "wl", 2) == 0);
        bool is_lo   = (strcmp(name, "lo") == 0);

        if (is_lo) continue;
        if (is_wifi) {
            if (wifi_ip.empty()) wifi_ip = ip;
        } else {
            if (wired_ip.empty()) wired_ip = ip;
        }
    }
    freeifaddrs(ifa_list);

    // Show all available IPs for discoverability
    if (!wired_ip.empty() && !wifi_ip.empty())
        return wired_ip + " / " + wifi_ip;
    if (!wired_ip.empty()) return wired_ip;
    if (!wifi_ip.empty()) return wifi_ip;
    return "no network";
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
        "\r\n",
        path, body.size());

    // Check for truncation — silent truncation would corrupt the request
    if (reqlen < 0 || (size_t)reqlen >= sizeof(req)) {
        fprintf(stderr, "[supervisor] http_post_localhost: header truncated "
                "(path=%s, body=%zu bytes)\n", path, body.size());
        close(sock);
        return "request too large";
    }

    // Write headers, then body separately — avoids formatting user-supplied
    // body into the snprintf buffer where it could be truncated.
    if (write(sock, req, reqlen) < 0) { close(sock); return "write error"; }
    if (!body.empty()) {
        if (write(sock, body.c_str(), body.size()) < 0) {
            close(sock);
            return "write error";
        }
    }

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
            snprintf(title, sizeof(title), "LLAMASTE  %s  (v%s)", mode_label, LLAMASTE_VERSION);
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

// Stderr tee thread — joinable so we can clean up on shutdown.
static std::thread g_tee_thread;

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

// Fork a command and capture its stdout into a string.
// argv[0] is the executable path; array must be null-terminated.
// Returns empty string on error or timeout.
static std::string capture_cmd(const char* exe, const char* const* argv, int timeout_ms) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return "";
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return ""; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        // Merge stderr into stdout so we capture error messages too
        dup2(pipefd[1], STDERR_FILENO);
        execv(exe, (char* const*)argv);
        _exit(127);
    }
    close(pipefd[1]);
    std::string out;
    char buf[512];
    struct pollfd pfd = { pipefd[0], POLLIN, 0 };
    for (;;) {
        int r = poll(&pfd, 1, timeout_ms);
        if (r <= 0) break;
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t)n);
        timeout_ms = 300; // shorter subsequent reads
    }
    close(pipefd[0]);
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
    return out;
}

struct WifiEntry {
    std::string ssid;
    std::string flags;   // raw wpa_cli flags e.g. "[WPA2-PSK-CCMP][ESS]"
    int signal_db;       // negative dBm
};

// Bring iface up, scan using `iw` tool (direct nl80211), return visible networks.
// Uses `iw dev <iface> scan` instead of wpa_supplicant — no ctrl socket needed.
// wpa_supplicant is only started later by child_main for the actual connection.
static std::vector<WifiEntry> supervisor_scan_wifi(const std::string& iface) {
    std::vector<WifiEntry> nets;

    // Unblock rfkill — some laptops have WiFi soft-blocked by default.
    // Uses /dev/rfkill device directly (no rfkill command needed).
    {
        int rf = open("/dev/rfkill", O_RDWR | O_CLOEXEC);
        if (rf >= 0) {
            struct {
                uint32_t idx;
                uint8_t  type;
                uint8_t  op;
                uint8_t  soft;
                uint8_t  hard;
            } ev = {};
            ev.type = 1; // RFKILL_TYPE_WLAN
            ev.op   = 3; // RFKILL_OP_CHANGE_ALL
            ev.soft = 0; // unblock
            if (write(rf, &ev, sizeof(ev)) > 0)
                fprintf(stderr, "[scan] rfkill: unblocked WLAN\n");
            close(rf);
        }
    }

    // Bring interface up (required before scanning)
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, iface.c_str(), IFNAMSIZ - 1);
            if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
                ifr.ifr_flags |= IFF_UP;
                if (ioctl(s, SIOCSIFFLAGS, &ifr) == 0)
                    fprintf(stderr, "[scan] interface %s UP\n", iface.c_str());
                else
                    fprintf(stderr, "[scan] interface %s UP failed: %m\n", iface.c_str());
            }
            close(s);
        }
    }

    // Reload regulatory database — cfg80211 (built-in) tried to load
    // regulatory.db during early kernel init (before squashfs pivot) and
    // cached the failure.  Now that the pivot is done and /lib/firmware/
    // is available, tell cfg80211 to retry, then set US regulatory domain
    // to unlock 5 GHz channels.
    {
        const char* reload_argv[] = { "iw", "reg", "reload", nullptr };
        std::string rr = capture_cmd("/usr/sbin/iw", reload_argv, 3000);
        while (!rr.empty() && (rr.back()=='\n'||rr.back()=='\r')) rr.pop_back();
        fprintf(stderr, "[scan] iw reg reload -> '%s'\n",
                rr.empty() ? "(ok)" : rr.c_str());

        // Brief pause for regulatory.db to be parsed
        usleep(500000);

        const char* argv[] = { "iw", "reg", "set", "US", nullptr };
        std::string r = capture_cmd("/usr/sbin/iw", argv, 3000);
        while (!r.empty() && (r.back()=='\n'||r.back()=='\r')) r.pop_back();
        fprintf(stderr, "[scan] iw reg set US -> '%s'\n",
                r.empty() ? "(ok)" : r.c_str());

        // Verify regulatory domain is active
        const char* get_argv[] = { "iw", "reg", "get", nullptr };
        std::string reg = capture_cmd("/usr/sbin/iw", get_argv, 3000);
        if (reg.find("country US") != std::string::npos)
            fprintf(stderr, "[scan] regulatory domain: US (confirmed)\n");
        else
            fprintf(stderr, "[scan] WARNING: regulatory domain NOT US:\n%s\n",
                    reg.substr(0, 200).c_str());
    }

    // Wait for driver to fully initialize after IFF_UP.
    // RTL8821CE needs ~2-4s for firmware + radio init; other drivers vary.
    fprintf(stderr, "[scan] waiting 4s for driver init...\n");
    usleep(4000000);

    // Log interface state for diagnostics
    {
        const char* argv[] = { "iw", "dev", iface.c_str(), "info", nullptr };
        std::string info = capture_cmd("/usr/sbin/iw", argv, 3000);
        fprintf(stderr, "[scan] iw dev %s info:\n%s\n", iface.c_str(), info.c_str());
    }

    // Scan with retry — up to 5 attempts using `iw dev <iface> scan`.
    // This talks directly to nl80211 via netlink — no wpa_supplicant, no ctrl
    // socket, no timing issues.  The command triggers a scan, waits for the
    // firmware to complete it, and dumps results in one shot.
    std::string raw;
    const int max_attempts = 5;
    for (int attempt = 1; attempt <= max_attempts; attempt++) {
        fprintf(stderr, "[scan] attempt %d/%d: iw dev %s scan...\n",
                attempt, max_attempts, iface.c_str());

        const char* argv[] = { "iw", "dev", iface.c_str(), "scan", nullptr };
        // 15s timeout — scan + dump can take 5-10s on some drivers
        raw = capture_cmd("/usr/sbin/iw", argv, 15000);

        fprintf(stderr, "[scan] iw scan returned %zu bytes\n", raw.size());

        // Check for BSS entries (indicates success)
        if (raw.find("BSS ") != std::string::npos) {
            fprintf(stderr, "[scan] got BSS entries on attempt %d\n", attempt);
            break;
        }

        // Log output for diagnostics (first 300 chars — may contain error message)
        if (!raw.empty()) {
            std::string preview = raw.substr(0, 300);
            // Trim for clean log
            while (!preview.empty() && (preview.back()=='\n'||preview.back()=='\r'))
                preview.pop_back();
            fprintf(stderr, "[scan] output: %s\n", preview.c_str());
        } else {
            fprintf(stderr, "[scan] (empty output — iw not found or exec failed)\n");
        }

        if (attempt < max_attempts) {
            // Common errors:
            //   "command failed: Device or resource busy (-16)" — scan in progress
            //   "command failed: Network is down (-100)" — IFF_UP not settled
            fprintf(stderr, "[scan] no BSS entries, retrying in 3s...\n");
            usleep(3000000);
        }
    }

    // Post-scan diagnostics: if 0 BSS entries, dump channel/phy info to help
    // diagnose regulatory or driver issues (visible via /debug/dmesg or serial)
    if (raw.find("BSS ") == std::string::npos) {
        fprintf(stderr, "[scan] --- post-scan diagnostics ---\n");
        // Show which channels the driver can use
        {
            const char* argv[] = { "iw", "phy", "phy0", "channels", nullptr };
            std::string ch = capture_cmd("/usr/sbin/iw", argv, 3000);
            // Just log first 500 chars to avoid flooding
            fprintf(stderr, "[scan] phy0 channels:\n%s\n", ch.substr(0, 500).c_str());
        }
        // Show interface link state
        {
            const char* argv[] = { "iw", "dev", iface.c_str(), "link", nullptr };
            std::string lnk = capture_cmd("/usr/sbin/iw", argv, 3000);
            fprintf(stderr, "[scan] %s link: %s\n", iface.c_str(),
                    lnk.empty() ? "(empty)" : lnk.substr(0, 200).c_str());
        }
        // Show regulatory domain
        {
            const char* argv[] = { "iw", "reg", "get", nullptr };
            std::string reg = capture_cmd("/usr/sbin/iw", argv, 3000);
            fprintf(stderr, "[scan] --- regulatory domain ---\n%s--- end regulatory ---\n",
                    reg.substr(0, 500).c_str());
        }
    }

    // Parse iw scan output.  Format:
    //   BSS aa:bb:cc:dd:ee:ff(on wlan0)
    //       freq: 2412
    //       signal: -65.00 dBm
    //       capability: ESS Privacy ShortSlotTime (0x0411)
    //       SSID: MyNetwork
    //       RSN:     * Version: 1
    //           ...
    // Each BSS block starts with "BSS " at column 0.  Fields are tab-indented.
    std::string cur_ssid;
    int cur_signal = -100;
    bool cur_privacy = false;
    bool cur_is_eap = false;
    bool in_bss = false;

    auto flush_bss = [&]() {
        if (!in_bss || cur_ssid.empty()) return;
        // Classify security type
        std::string flags;
        if (!cur_privacy) {
            flags = "[Open]";
        } else if (cur_is_eap) {
            flags = "[WPA-EAP]";
        } else {
            flags = "[WPA-PSK]";
        }
        // De-duplicate by SSID — keep strongest signal
        bool dup = false;
        for (auto& e : nets) {
            if (e.ssid == cur_ssid) {
                if (cur_signal > e.signal_db) {
                    e.signal_db = cur_signal;
                    e.flags = flags;
                }
                dup = true;
                break;
            }
        }
        if (!dup) {
            nets.push_back({cur_ssid, flags, cur_signal});
        }
    };

    const char* lp = raw.c_str();
    while (*lp) {
        // Read one line
        const char* ls = lp;
        while (*lp && *lp != '\n') lp++;
        size_t ll = (size_t)(lp - ls);
        if (*lp == '\n') lp++;

        // New BSS block?
        if (ll >= 4 && ls[0]=='B' && ls[1]=='S' && ls[2]=='S' && ls[3]==' ') {
            flush_bss();
            in_bss = true;
            cur_ssid.clear();
            cur_signal = -100;
            cur_privacy = false;
            cur_is_eap = false;
            continue;
        }

        if (!in_bss) continue;

        // Skip leading whitespace
        const char* tp = ls;
        while (tp < ls + ll && (*tp == ' ' || *tp == '\t')) tp++;
        size_t tl = ll - (size_t)(tp - ls);
        if (tl == 0) continue;

        // Parse key fields
        if (tl > 6 && memcmp(tp, "SSID: ", 6) == 0) {
            cur_ssid.assign(tp + 6, tl - 6);
            // Trim trailing whitespace from SSID
            while (!cur_ssid.empty() &&
                   (cur_ssid.back()==' '||cur_ssid.back()=='\r'))
                cur_ssid.pop_back();
        } else if (tl > 8 && memcmp(tp, "signal: ", 8) == 0) {
            // "signal: -65.00 dBm" → extract integer part
            cur_signal = atoi(tp + 8);
        } else if (tl > 12 && memcmp(tp, "capability: ", 12) == 0) {
            // "Privacy" flag in capability = encrypted network
            std::string cap(tp + 12, tl - 12);
            if (cap.find("Privacy") != std::string::npos)
                cur_privacy = true;
        } else if ((tl >= 4 && memcmp(tp, "RSN:", 4) == 0) ||
                   (tl >= 4 && memcmp(tp, "WPA:", 4) == 0)) {
            cur_privacy = true;
        } else {
            // Detect Enterprise (802.1X/EAP) authentication
            std::string line(tp, tl);
            if (line.find("Authentication suites:") != std::string::npos) {
                if (line.find("802.1X") != std::string::npos ||
                    line.find("EAP") != std::string::npos) {
                    cur_is_eap = true;
                }
            }
        }
    }
    flush_bss(); // last BSS entry

    std::sort(nets.begin(), nets.end(),
              [](const WifiEntry& a, const WifiEntry& b) {
                  return a.signal_db > b.signal_db;
              });

    fprintf(stderr, "[scan] found %zu networks via iw\n", nets.size());

    // Dump regulatory domain for diagnostics
    {
        const char* argv[] = { "iw", "reg", "get", nullptr };
        std::string reg = capture_cmd("/usr/sbin/iw", argv, 3000);
        fprintf(stderr, "[scan] --- regulatory domain ---\n%s", reg.c_str());
        fprintf(stderr, "[scan] --- end regulatory ---\n");
    }

    return nets;
}

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

    // --- Phase 1: Scan ---
    wstr("\033[2J\033[H\r\n");
    wstr("  +--------------------------------------------------+\r\n");
    wstr("  |           Llamaste  -  WiFi Setup                |\r\n");
    wstr("  +--------------------------------------------------+\r\n");
    wstr("\r\n");
    wstr("  Adapter : "); wstr(iface.c_str()); wstr("\r\n");
    wstr("  Scanning for networks...\r\n");

    auto nets = supervisor_scan_wifi(iface);

    // --- Phase 2: Show results / pick network ---
    wstr("\033[2J\033[H\r\n");
    wstr("  +--------------------------------------------------+\r\n");
    wstr("  |           Llamaste  -  WiFi Setup                |\r\n");
    wstr("  +--------------------------------------------------+\r\n");
    wstr("\r\n");

    std::string ssid;
    bool is_open = false;

    if (!nets.empty()) {
        int n_show = (int)nets.size() > 9 ? 9 : (int)nets.size();
        wstr("  Visible networks:\r\n\r\n");
        for (int i = 0; i < n_show; i++) {
            const auto& e = nets[i];
            // Use flags as-is — already classified as [Open], [WPA-PSK], or [WPA-EAP]
            char line[96];
            snprintf(line, sizeof(line), "   %d  %-32s  %-9s  %4d dBm\r\n",
                     i + 1, e.ssid.substr(0, 32).c_str(),
                     e.flags.c_str(),
                     e.signal_db);
            wstr(line);
        }
        wstr("\r\n");
        char prompt[48];
        snprintf(prompt, sizeof(prompt),
                 "  Enter number (1-%d), or Enter to type manually: ", n_show);
        wstr(prompt);

        // Single keypress — no Enter needed
        char c = 0;
        read(tty, &c, 1);
        write(tty, &c, 1);
        wstr("\r\n");

        if (c >= '1' && c <= ('0' + n_show)) {
            int idx = c - '1';
            const auto& chosen = nets[idx];
            ssid    = chosen.ssid;
            is_open = (chosen.flags == "[Open]");
            wstr("  Selected: "); wstr(ssid.c_str()); wstr("\r\n");

            // Warn about Enterprise/802.1X networks — PSK won't work
            if (chosen.flags.find("EAP") != std::string::npos) {
                wstr("\r\n");
                wstr("  ** WARNING: This network uses Enterprise (802.1X) auth **\r\n");
                wstr("  ** PSK/passphrase may not work — try a different network **\r\n");
                wstr("\r\n");
            }
        }
        // else: fall through to manual entry below
    } else {
        wstr("  No networks found.\r\n\r\n");
    }

    // --- Phase 3: Manual SSID entry if not picked from list ---
    if (ssid.empty()) {
        wstr("  SSID     : ");
        ssid = read_line(true, 63);
    }

    if (ssid.empty()) {
        wstr("  Skipping WiFi setup.\r\n\r\n");
        tcsetattr(tty, TCSAFLUSH, &old_tio);
        close(tty);
        return;
    }

    // --- Phase 4: Password ---
    std::string psk;
    if (!is_open) {
        wstr("  Password : ");
        psk = read_line(false, 63);
        wstr("\r\n");
        if (psk.empty()) {
            // Treat as open if user skips password for a manually entered SSID
            is_open = true;
        }
    } else {
        wstr("  (Open network — no password needed)\r\n");
    }

    tcsetattr(tty, TCSAFLUSH, &old_tio);
    close(tty);

    // --- Phase 5: Write wpa.conf ---
    // Ensure data directories exist (we run before child_main creates them).
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
    // country=US: set regulatory domain so 5GHz channels are enabled.
    // cfg80211 fails to load regulatory.db from the filesystem at early boot
    // (before the squashfs pivot mounts the full OS), so wpa_supplicant setting
    // the country code via nl80211 is the reliable way to get a proper
    // regulatory domain at runtime.
    fprintf(f, "country=US\n");
    fprintf(f, "\n");
    fprintf(f, "network={\n");
    fprintf(f, "    ssid=\"%s\"\n", ssid.c_str());
    if (is_open || psk.empty()) {
        fprintf(f, "    key_mgmt=NONE\n");
    } else {
        fprintf(f, "    psk=\"%s\"\n", psk.c_str());
        fprintf(f, "    key_mgmt=WPA-PSK\n");
    }
    fprintf(f, "}\n");
    fclose(f);
    fprintf(stderr, "[wifi] console_wifi_setup: saved SSID '%s' (%s)\n",
            ssid.c_str(), is_open ? "open" : "WPA-PSK");
    // wpa_supplicant not yet running — child_main will start it with the saved config.
}

// Quick ethernet pre-flight: scan for wired interfaces and spawn dhcpcd.
// Headless servers should not wait 6s for nonexistent WiFi — start DHCP
// on any wired interface immediately so they get network access right away.
static void supervisor_preflight_ethernet() {
#ifndef _WIN32
    DIR* nd = opendir("/sys/class/net");
    if (!nd) return;
    struct dirent* ne;
    bool found = false;
    while ((ne = readdir(nd))) {
        if (ne->d_name[0] == '.') continue;
        const std::string ifname = ne->d_name;
        if (ifname == "lo") continue;
        // Skip known tunnel/virtual interfaces
        if (ifname == "sit0" || ifname == "tunl0" || ifname == "ip6tnl0" ||
            ifname == "gre0" || ifname == "ip_vti0" || ifname == "ip6_vti0")
            continue;

        // Check type is ARPHRD_ETHER (1) — skip tunnels, loopback, etc.
        char tpath[256];
        snprintf(tpath, sizeof(tpath), "/sys/class/net/%s/type", ifname.c_str());
        FILE* tf = fopen(tpath, "r");
        if (tf) {
            int iftype = 0;
            fscanf(tf, "%d", &iftype);
            fclose(tf);
            if (iftype != 1) continue;
        }

        // Skip WiFi interfaces
        char wpath[256];
        snprintf(wpath, sizeof(wpath), "/sys/class/net/%s/phy80211", ifname.c_str());
        if (access(wpath, F_OK) == 0) continue;

        // Bring interface UP via ioctl
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock >= 0) {
            struct ifreq ifr = {};
            strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
            if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
                ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
                ioctl(sock, SIOCSIFFLAGS, &ifr);
            }
            close(sock);
        }

        // Spawn dhcpcd in background on this interface
        pid_t pid = fork();
        if (pid == 0) {
            execl("/sbin/dhcpcd", "dhcpcd", "-b", ifname.c_str(), (char*)nullptr);
            _exit(127);
        }
        if (pid > 0) {
            fprintf(stderr, "[supervisor] ethernet pre-flight: dhcpcd spawned on %s\n",
                    ifname.c_str());
            found = true;
        }
    }
    closedir(nd);
    if (found)
        fprintf(stderr, "[supervisor] ethernet pre-flight: DHCP started on wired interfaces\n");
#endif
}

static void supervisor_preflight_wifi(const SupervisorConfig& config) {
    if (config.boot_mode == "desktop") return;

    // Retry interface detection — driver may still be probing after module load.
    // init_load_modules() only waits 500ms; some drivers need up to 3s to
    // create the wlan0 interface after finit_module().
    std::string iface;
    for (int try_n = 0; try_n < 6; try_n++) {
        iface = supervisor_detect_wifi_iface();
        if (!iface.empty()) break;
        if (try_n == 0)
            fprintf(stderr, "[wifi] no WiFi iface yet, waiting for driver probe...\n");
        usleep(1000000); // 1s per retry, up to 6s total
    }
    if (iface.empty()) {
        fprintf(stderr, "[wifi] no WiFi interface found after 6s — skipping setup\n");
        return;
    }
    fprintf(stderr, "[wifi] detected interface: %s\n", iface.c_str());

    if (supervisor_wifi_has_saved_networks()) {
        fprintf(stderr, "[wifi] saved networks found in wpa.conf — skipping setup\n");
        return;
    }
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

    // First-boot ethernet + WiFi setup: run before display thread so the
    // terminal is clean. Start wired DHCP immediately so headless servers
    // don't wait 6s for nonexistent WiFi.
#ifndef _WIN32
    supervisor_preflight_ethernet();
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

        // Wait for any child to exit.
        // Uses waitpid(-1) to catch exits from child_main, dbus, or bluetoothd.
        // If dbus/bluetoothd exit, respawn them (max 3 per 5 minutes).
        // When child_main exits, proceed with restart logic.
        int status = 0;
        static int dbus_restarts = 0, bt_restarts = 0;
        static time_t dbus_restart_window = 0, bt_restart_window = 0;

        while (true) {
            // If the SIGCHLD handler already reaped the child (race-free
            // path), skip the blocking wait — g_last_exit_code / signal
            // are already populated by the handler.
            if (g_child_exited) break;

            pid_t exited = waitpid(-1, &status, 0);
            if (exited < 0) break; // error or no children

            if (exited == g_child_pid) {
                // Main inference child exited — handle below
                g_child_exited = 1;
                g_last_exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                g_last_exit_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
                break;
            }

            // Check if it's a daemon we should respawn
            time_t now = time(nullptr);

            if (exited == g_dbus_pid) {
                fprintf(stderr, "[supervisor] dbus-daemon exited, respawning\n");
                if (now - dbus_restart_window > 300) { dbus_restarts = 0; dbus_restart_window = now; }
                if (dbus_restarts < 3) {
                    init_start_dbus();
                    dbus_restarts++;
                    // Also restart bluetoothd since it depends on dbus
                    if (g_bluetoothd_pid > 0) {
                        kill(g_bluetoothd_pid, SIGTERM);
                        waitpid(g_bluetoothd_pid, nullptr, WNOHANG);
                    }
                    init_start_bluetoothd();
                } else {
                    fprintf(stderr, "[supervisor] dbus-daemon restart limit (3/5min) reached\n");
                }
                continue;
            }

            if (exited == g_bluetoothd_pid) {
                fprintf(stderr, "[supervisor] bluetoothd exited, respawning\n");
                if (now - bt_restart_window > 300) { bt_restarts = 0; bt_restart_window = now; }
                if (bt_restarts < 3) {
                    init_start_bluetoothd();
                    bt_restarts++;
                } else {
                    fprintf(stderr, "[supervisor] bluetoothd restart limit (3/5min) reached\n");
                }
                continue;
            }

            // Unknown child — ignore (could be a fire-and-forget process)
        }

        fprintf(stderr, "[supervisor] Child exited (code=%d signal=%d)\n",
                g_last_exit_code, g_last_exit_signal);

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

    // Join tee thread so it flushes and closes cleanly.
    // Closing the pipe write-end (via dup2 above) causes read() to
    // return 0, which exits the thread. We join with a 2s timeout
    // to avoid hanging on a stuck read().
    close(STDERR_FILENO);  // close write-end to unblock tee thread
    if (g_tee_thread.joinable()) {
        // Simple deadline: poll joinable, then detach if stuck
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_tee_thread.joinable() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (g_tee_thread.joinable()) {
            // Thread didn't exit in time — detach and proceed
            g_tee_thread.detach();
        } else {
            g_tee_thread.join();
        }
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
