// wifi.cpp — WiFi manager using wpa_supplicant control protocol
//
// Communicates with wpa_supplicant via its Unix domain socket control interface.
// No vendored wpa_ctrl library — the protocol is simple ASCII text:
//   client binds /tmp/wpa_ctrl_<pid>_<N>
//   client connects to /run/wpa_supplicant/<iface>
//   send ASCII command → receive ASCII reply
//   event messages begin with '<' and are skipped
//
// SCAN_RESULTS output format (tab-separated, header first):
//   bssid / frequency / signal level / flags / ssid
//   00:11:22:33:44:55  2437  -65  [WPA2-PSK-CCMP][ESS]  MyNetwork
//
// LIST_NETWORKS output format (tab-separated, header first):
//   network id / ssid / bssid / flags
//   0  MyNetwork  any  [CURRENT]

#include "wifi.h"

#ifndef _WIN32

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <chrono>
#include <thread>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <algorithm>

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static bool dir_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string str_trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Split into up to max_parts tokens; if max_parts > 0 the last token gets
// everything remaining (handles SSIDs with tab characters, if any).
static std::vector<std::string> str_split(const std::string& s, char delim,
                                          int max_parts = -1) {
    std::vector<std::string> parts;
    std::string token;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == delim && (max_parts < 0 || (int)parts.size() < max_parts - 1)) {
            parts.push_back(token);
            token.clear();
        } else {
            token += s[i];
        }
    }
    parts.push_back(token);
    return parts;
}

// Classify WiFi security from wpa_supplicant flags string.
// e.g. "[WPA2-PSK-CCMP][WPS][ESS]" → "WPA2-PSK"
static std::string classify_security(const std::string& flags) {
    if (flags.find("WPA2") != std::string::npos) return "WPA2-PSK";
    if (flags.find("WPA")  != std::string::npos) return "WPA-PSK";
    if (flags.find("WEP")  != std::string::npos) return "WEP";
    return "OPEN";
}

static int g_ctrl_counter = 0;

// ---------------------------------------------------------------------------
// capture_cmd_wifi — fork/exec a command and capture stdout+stderr
// ---------------------------------------------------------------------------
static std::string capture_cmd_wifi(const char* exe, const char* const* argv,
                                    int timeout_ms) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) < 0) return "";
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return ""; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
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

// ---------------------------------------------------------------------------
// WiFiManager — construction / destruction
// ---------------------------------------------------------------------------

WiFiManager::WiFiManager() : ctrl_fd_(-1) {}

WiFiManager::~WiFiManager() {
    close_ctrl();
}

bool WiFiManager::has_wifi() const {
    return !iface_.empty();
}

bool WiFiManager::daemon_running() const {
    return ctrl_fd_ >= 0;
}

// ---------------------------------------------------------------------------
// Initialisation: detect interface + try to open control socket
// ---------------------------------------------------------------------------

bool WiFiManager::init() {
    // Scan /sys/class/net for interfaces with a "wireless" or "phy80211" subdir.
    DIR* d = opendir("/sys/class/net");
    if (!d) return false;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        if (dir_exists("/sys/class/net/" + name + "/wireless") ||
            dir_exists("/sys/class/net/" + name + "/phy80211")) {
            iface_ = name;
            break;
        }
    }
    closedir(d);

    if (iface_.empty()) {
        fprintf(stderr, "[wifi] No WiFi interface found\n");
        return false;
    }

    fprintf(stderr, "[wifi] Found WiFi interface: %s\n", iface_.c_str());

    // wpa_supplicant may not be running yet — that's OK.
    if (open_ctrl()) {
        fprintf(stderr, "[wifi] wpa_supplicant ready on %s\n", iface_.c_str());
    }

    return true;
}

// ---------------------------------------------------------------------------
// Control socket management
// ---------------------------------------------------------------------------

bool WiFiManager::open_ctrl() {
    if (ctrl_fd_ >= 0) return true;

    std::string server_path = "/run/wpa_supplicant/" + iface_;
    struct stat st;
    if (stat(server_path.c_str(), &st) != 0) {
        return false;  // wpa_supplicant socket not present
    }

    ctrl_fd_ = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ctrl_fd_ < 0) {
        fprintf(stderr, "[wifi] socket() failed: %s\n", strerror(errno));
        return false;
    }

    ctrl_path_ = "/tmp/wpa_ctrl_" + std::to_string(getpid()) + "_" +
                 std::to_string(++g_ctrl_counter);

    struct sockaddr_un local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sun_family = AF_UNIX;
    strncpy(local_addr.sun_path, ctrl_path_.c_str(),
            sizeof(local_addr.sun_path) - 1);

    if (bind(ctrl_fd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        fprintf(stderr, "[wifi] bind(%s) failed: %s\n",
                ctrl_path_.c_str(), strerror(errno));
        close(ctrl_fd_);
        ctrl_fd_ = -1;
        ctrl_path_.clear();
        return false;
    }

    struct sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, server_path.c_str(),
            sizeof(server_addr.sun_path) - 1);

    if (::connect(ctrl_fd_, (struct sockaddr*)&server_addr,
                sizeof(server_addr)) < 0) {
        fprintf(stderr, "[wifi] connect(%s) failed: %s\n",
                server_path.c_str(), strerror(errno));
        unlink(ctrl_path_.c_str());
        close(ctrl_fd_);
        ctrl_fd_ = -1;
        ctrl_path_.clear();
        return false;
    }

    return true;
}

void WiFiManager::close_ctrl() {
    if (ctrl_fd_ >= 0) {
        close(ctrl_fd_);
        ctrl_fd_ = -1;
    }
    if (!ctrl_path_.empty()) {
        unlink(ctrl_path_.c_str());
        ctrl_path_.clear();
    }
}

// ---------------------------------------------------------------------------
// mask_psk() — redact PSK from wpa_supplicant command strings for logging.
// Commands like "SET_NETWORK 0 psk \"secret\"" become "...psk \"****\"".
// ---------------------------------------------------------------------------
static std::string mask_psk(const std::string& cmd) {
    std::string result;
    result.reserve(cmd.size());

    // Look for "psk " or "psk\"" patterns and mask the value that follows
    for (size_t i = 0; i < cmd.size(); ) {
        // Check if we're at a PSK keyword
        if ((i + 4 <= cmd.size()) &&
            (cmd[i] == 'p' && cmd[i+1] == 's' && cmd[i+2] == 'k') &&
            (i == 0 || cmd[i-1] == ' ' || cmd[i-1] == '\"')) {
            size_t j = i + 3;  // past "psk"
            // Skip whitespace
            while (j < cmd.size() && cmd[j] == ' ') j++;
            bool quoted = (j < cmd.size() && cmd[j] == '\"');
            if (quoted) j++;  // skip opening quote
            // Mask the value
            result += "psk ";
            if (quoted) result += '\"';
            result += "****";
            if (quoted) result += '\"';
            // Skip original chars up to the closing quote or next space
            if (quoted) {
                while (j < cmd.size() && cmd[j] != '\"') j++;
                if (j < cmd.size()) j++;  // skip closing quote
            } else {
                while (j < cmd.size() && cmd[j] != ' ') j++;
            }
            i = j;
        } else {
            result += cmd[i];
            i++;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// wpa() — send one command, receive one reply, skip event messages
// ---------------------------------------------------------------------------

std::string WiFiManager::wpa(const std::string& cmd, int timeout_ms) {
    if (ctrl_fd_ < 0 && !open_ctrl()) return "";

    if (send(ctrl_fd_, cmd.c_str(), cmd.size(), 0) < 0) {
        fprintf(stderr, "[wifi] send(%s) failed: %s\n",
                mask_psk(cmd).c_str(), strerror(errno));
        close_ctrl();
        return "";
    }

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    for (;;) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return "";

        int ms_left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - now).count();
        struct pollfd pfd;
        pfd.fd     = ctrl_fd_;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, ms_left);
        if (ret <= 0) return "";

        char buf[65536];
        ssize_t n = recv(ctrl_fd_, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close_ctrl(); return ""; }
        buf[n] = '\0';

        // Skip unsolicited event messages (start with '<priority>')
        if (buf[0] == '<') continue;

        return std::string(buf, (size_t)n);
    }
}

// ---------------------------------------------------------------------------
// status()
// ---------------------------------------------------------------------------

WiFiStatus WiFiManager::status() {
    WiFiStatus s;
    s.available = !iface_.empty();
    s.iface     = iface_;
    if (!s.available) return s;

    if (ctrl_fd_ < 0) open_ctrl();
    s.daemon_running = (ctrl_fd_ >= 0);

    if (!s.daemon_running) { s.state = "DISCONNECTED"; return s; }

    std::string reply = wpa("STATUS");
    if (reply.empty()) { s.state = "DISCONNECTED"; return s; }

    std::istringstream ss(reply);
    std::string line;
    while (std::getline(ss, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = str_trim(line.substr(0, eq));
        std::string val = str_trim(line.substr(eq + 1));
        if      (key == "wpa_state")    s.state   = val;
        else if (key == "ssid")         s.ssid    = val;
        else if (key == "bssid")        s.bssid   = val;
        else if (key == "ip_address")   s.ip_addr = val;
        else if (key == "signal_level") { try { s.signal_dbm = std::stoi(val); } catch (...) {} }
    }
    if (s.state.empty()) s.state = "UNKNOWN";
    if (s.ip_addr.empty() && s.state == "COMPLETED") s.ip_addr = get_ip_addr();
    return s;
}

// ---------------------------------------------------------------------------
// scan()
// ---------------------------------------------------------------------------

std::vector<WiFiNetwork> WiFiManager::scan() {
    if (!has_wifi()) return {};

    // Try wpa_supplicant scan first (works when ctrl socket is healthy)
    if (ctrl_fd_ >= 0 || open_ctrl()) {
        wpa("SCAN");  // reply may be "OK" or "FAIL-BUSY-SCAN-STARTED"

        // Wait for scan to finish (up to 4 seconds)
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::string st = wpa("STATUS");
            if (st.find("wpa_state=SCANNING") == std::string::npos) break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::string results = wpa("SCAN_RESULTS", 5000);
        auto nets = parse_scan_results(results);
        if (!nets.empty()) {
            fprintf(stderr, "[wifi] scan: %zu networks via wpa_supplicant\n", nets.size());
            return nets;
        }
        fprintf(stderr, "[wifi] scan: wpa_supplicant returned 0 networks, trying iw fallback\n");
    } else {
        fprintf(stderr, "[wifi] scan: no wpa_supplicant ctrl socket, using iw scan\n");
    }

    // Fallback: direct iw scan (confirmed working on RTL8821CE hardware)
    return iw_scan();
}

// static
std::vector<WiFiNetwork> WiFiManager::parse_scan_results(const std::string& raw) {
    std::vector<WiFiNetwork> nets;
    if (raw.empty()) return nets;

    std::istringstream ss(raw);
    std::string line;
    bool header = true;
    while (std::getline(ss, line)) {
        line = str_trim(line);
        if (line.empty()) continue;
        if (header) { header = false; continue; }

        // bssid \t freq \t signal \t flags \t ssid
        auto p = str_split(line, '\t', 5);
        if ((int)p.size() < 5) continue;

        WiFiNetwork net;
        net.bssid      = str_trim(p[0]);
        try { net.freq_mhz   = std::stoi(str_trim(p[1])); } catch (...) {}
        try { net.signal_dbm = std::stoi(str_trim(p[2])); } catch (...) {}
        net.security   = classify_security(str_trim(p[3]));
        net.ssid       = str_trim(p[4]);
        if (!net.ssid.empty()) nets.push_back(net);
    }
    return nets;
}

// ---------------------------------------------------------------------------
// list_networks()
// ---------------------------------------------------------------------------

std::vector<WiFiNetwork> WiFiManager::list_networks() {
    if (!has_wifi()) return {};
    if (ctrl_fd_ < 0 && !open_ctrl()) return {};
    return parse_list_networks(wpa("LIST_NETWORKS"));
}

// static
std::vector<WiFiNetwork> WiFiManager::parse_list_networks(const std::string& raw) {
    std::vector<WiFiNetwork> nets;
    if (raw.empty()) return nets;

    std::istringstream ss(raw);
    std::string line;
    bool header = true;
    while (std::getline(ss, line)) {
        line = str_trim(line);
        if (line.empty()) continue;
        if (header) { header = false; continue; }

        // network_id \t ssid \t bssid \t flags
        // Note: empty flags field causes trailing \t to be trimmed, so accept 3+ parts.
        auto p = str_split(line, '\t', 4);
        if ((int)p.size() < 3) continue;
        std::string flags = ((int)p.size() >= 4) ? str_trim(p[3]) : "";

        WiFiNetwork net;
        try { net.network_id = std::stoi(str_trim(p[0])); } catch (...) { continue; }
        net.ssid    = str_trim(p[1]);
        net.bssid   = str_trim(p[2]);
        net.saved   = true;
        net.connected = (flags.find("CURRENT") != std::string::npos);
        nets.push_back(net);
    }
    return nets;
}

// ---------------------------------------------------------------------------
// connect()
// ---------------------------------------------------------------------------

std::string WiFiManager::connect(const std::string& ssid,
                                 const std::string& psk) {
    if (!has_wifi()) return "No WiFi interface";
    if (ctrl_fd_ < 0 && !open_ctrl()) {
        // Try to spawn wpa_supplicant on-demand (desktop mode: user clicks
        // Connect before any daemon was started, or daemon silently crashed).
        if (spawn_cb_) {
            fprintf(stderr, "[wifi] connect: no daemon, trying spawn callback\n");
            spawn_cb_(iface_);
            // Wait for ctrl socket to appear
            for (int i = 0; i < 8; i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (open_ctrl()) break;
            }
        }
        if (ctrl_fd_ < 0) return "wpa_supplicant not running";
    }

    // Reuse saved network if it exists
    auto saved = list_networks();
    int net_id = -1;
    for (const auto& n : saved) {
        if (n.ssid == ssid) { net_id = n.network_id; break; }
    }

    bool created_new = (net_id < 0);

    if (created_new) {
        std::string r = str_trim(wpa("ADD_NETWORK"));
        if (r == "FAIL") return "ADD_NETWORK failed";
        try { net_id = std::stoi(r); } catch (...) {
            return "ADD_NETWORK unexpected reply: " + r;
        }

        std::string id = std::to_string(net_id);
        if (str_trim(wpa("SET_NETWORK " + id + " ssid \"" + ssid + "\"")) != "OK")
            return "SET_NETWORK ssid failed";

        if (psk.empty()) {
            if (str_trim(wpa("SET_NETWORK " + id + " key_mgmt NONE")) != "OK")
                return "SET_NETWORK key_mgmt failed";
        } else {
            if (str_trim(wpa("SET_NETWORK " + id + " psk \"" + psk + "\"")) != "OK")
                return "SET_NETWORK psk failed";
        }
    }

    // Clear any stale BSSID hint — forces wpa_supplicant to scan for the
    // AP fresh rather than targeting a cached BSSID that may have moved
    // channels or be unreachable.  This prevents 4-way handshake timeouts
    // on installed systems where wpa.conf persists across reboots.
    std::string id_str = std::to_string(net_id);
    wpa("SET_NETWORK " + id_str + " bssid any");

    if (str_trim(wpa("SELECT_NETWORK " + id_str)) != "OK")
        return "SELECT_NETWORK failed";

    // Poll for COMPLETED (up to 20s — 4-way handshake can take 10-15s)
    std::string last_state;
    std::string prev_state;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::string st = wpa("STATUS");
        std::istringstream ss(st);
        std::string line;
        while (std::getline(ss, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            if (str_trim(line.substr(0, eq)) == "wpa_state") {
                last_state = str_trim(line.substr(eq + 1));
                break;
            }
        }
        if (last_state != prev_state) {
            fprintf(stderr, "[wifi] connect(%s): state %s -> %s\n",
                    ssid.c_str(), prev_state.c_str(), last_state.c_str());
            prev_state = last_state;
        }
        if (last_state == "COMPLETED") break;
    }

    if (last_state != "COMPLETED") {
        if (created_new) wpa("REMOVE_NETWORK " + std::to_string(net_id));
        return "Connection failed (state: " + last_state + ")";
    }

    wpa("SAVE_CONFIG");
    return "";
}

// ---------------------------------------------------------------------------
// disconnect() / forget()
// ---------------------------------------------------------------------------

std::string WiFiManager::disconnect() {
    if (!has_wifi()) return "No WiFi interface";
    if (ctrl_fd_ < 0 && !open_ctrl()) return "wpa_supplicant not running";
    std::string r = str_trim(wpa("DISCONNECT"));
    return (r == "OK") ? "" : "DISCONNECT failed: " + r;
}

std::string WiFiManager::forget(const std::string& ssid) {
    if (!has_wifi()) return "No WiFi interface";
    if (ctrl_fd_ < 0 && !open_ctrl()) return "wpa_supplicant not running";

    auto saved = list_networks();
    int found_id = -1;
    for (const auto& n : saved) {
        if (n.ssid == ssid) { found_id = n.network_id; break; }
    }
    if (found_id < 0) return "Network not found: " + ssid;

    std::string r = str_trim(wpa("REMOVE_NETWORK " + std::to_string(found_id)));
    if (r != "OK") return "REMOVE_NETWORK failed: " + r;
    wpa("SAVE_CONFIG");
    return "";
}

// ---------------------------------------------------------------------------
// clear_all_bssids() — remove stale BSSID hints from all saved networks
// ---------------------------------------------------------------------------

void WiFiManager::clear_all_bssids() {
    if (ctrl_fd_ < 0 && !open_ctrl()) return;

    auto saved = list_networks();
    for (const auto& n : saved) {
        if (n.network_id >= 0) {
            std::string id = std::to_string(n.network_id);
            wpa("SET_NETWORK " + id + " bssid any");
        }
    }
    if (!saved.empty()) {
        wpa("SAVE_CONFIG");
        fprintf(stderr, "[wifi] Cleared BSSID hints from %zu saved networks\n",
                saved.size());
    }
}

// ---------------------------------------------------------------------------
// enable() / disable() — bring interface up/down via SIOCSIFFLAGS
// ---------------------------------------------------------------------------

std::string WiFiManager::enable() {
    if (!has_wifi()) return "No WiFi interface";

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return std::string("socket() failed: ") + strerror(errno);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface_.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        std::string e = strerror(errno);
        close(sock); return "SIOCGIFFLAGS failed: " + e;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        std::string e = strerror(errno);
        close(sock); return "SIOCSIFFLAGS failed: " + e;
    }
    close(sock);

    if (ctrl_fd_ < 0) open_ctrl();
    return "";
}

std::string WiFiManager::disable() {
    if (!has_wifi()) return "No WiFi interface";

    if (ctrl_fd_ >= 0) wpa("DISCONNECT");

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return std::string("socket() failed: ") + strerror(errno);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface_.c_str(), IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        std::string e = strerror(errno);
        close(sock); return "SIOCGIFFLAGS failed: " + e;
    }
    ifr.ifr_flags &= ~IFF_UP;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        std::string e = strerror(errno);
        close(sock); return "SIOCSIFFLAGS failed: " + e;
    }
    close(sock);
    return "";
}

// ---------------------------------------------------------------------------
// get_ip_addr() — SIOCGIFADDR ioctl
// ---------------------------------------------------------------------------

std::string WiFiManager::get_ip_addr() const {
    if (iface_.empty()) return "";

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "";

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, iface_.c_str(), IFNAMSIZ - 1);

    std::string ip;
    if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
        ip = inet_ntoa(((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr);
    }
    close(sock);
    return ip;
}

// ---------------------------------------------------------------------------
// iw_scan() — direct nl80211 scan via `iw dev <iface> scan`
//
// Fallback for when wpa_supplicant SCAN_RESULTS returns empty.
// This is the same approach used by supervisor_scan_wifi() which
// confirmed working on real hardware (9 networks found on RTL8821CE).
// ---------------------------------------------------------------------------

std::vector<WiFiNetwork> WiFiManager::iw_scan() {
    std::vector<WiFiNetwork> nets;
    if (iface_.empty()) return nets;

    fprintf(stderr, "[wifi] iw_scan fallback on %s\n", iface_.c_str());

    // Ensure interface is up
    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, iface_.c_str(), IFNAMSIZ - 1);
            if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
                if (!(ifr.ifr_flags & IFF_UP)) {
                    ifr.ifr_flags |= IFF_UP;
                    ioctl(s, SIOCSIFFLAGS, &ifr);
                }
            }
            close(s);
        }
    }

    // Try scan up to 3 times (wpa_supplicant may hold the interface,
    // causing "Device or resource busy" on the first attempt)
    std::string raw;
    for (int attempt = 1; attempt <= 3; attempt++) {
        const char* argv[] = { "iw", "dev", iface_.c_str(), "scan", nullptr };
        raw = capture_cmd_wifi("/usr/sbin/iw", argv, 15000);

        if (raw.find("BSS ") != std::string::npos) {
            fprintf(stderr, "[wifi] iw_scan: got BSS entries on attempt %d\n", attempt);
            break;
        }

        // "Device or resource busy" — wpa_supplicant is scanning; use its cached results
        if (raw.find("busy") != std::string::npos) {
            // Try `iw dev <iface> scan dump` to get cached results instead
            const char* dump_argv[] = { "iw", "dev", iface_.c_str(), "scan", "dump", nullptr };
            raw = capture_cmd_wifi("/usr/sbin/iw", dump_argv, 5000);
            if (raw.find("BSS ") != std::string::npos) {
                fprintf(stderr, "[wifi] iw_scan: got cached BSS from scan dump\n");
                break;
            }
        }

        if (attempt < 3) {
            fprintf(stderr, "[wifi] iw_scan: attempt %d failed, retrying in 2s\n", attempt);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }

    if (raw.find("BSS ") == std::string::npos) {
        fprintf(stderr, "[wifi] iw_scan: no BSS entries found\n");
        return nets;
    }

    // Parse BSS entries — same format as supervisor_scan_wifi()
    std::string cur_ssid;
    std::string cur_bssid;
    int cur_signal = -100;
    int cur_freq = 0;
    bool cur_privacy = false;
    bool cur_is_eap = false;
    bool in_bss = false;

    auto flush_bss = [&]() {
        if (!in_bss || cur_ssid.empty()) return;
        // Classify security
        std::string sec = "OPEN";
        if (cur_privacy) {
            sec = cur_is_eap ? "WPA2-EAP" : "WPA2-PSK";
        }
        // De-duplicate by SSID — keep strongest signal
        bool dup = false;
        for (auto& n : nets) {
            if (n.ssid == cur_ssid) {
                if (cur_signal > n.signal_dbm) {
                    n.signal_dbm = cur_signal;
                    n.security = sec;
                    n.bssid = cur_bssid;
                    n.freq_mhz = cur_freq;
                }
                dup = true;
                break;
            }
        }
        if (!dup) {
            WiFiNetwork net;
            net.ssid = cur_ssid;
            net.bssid = cur_bssid;
            net.signal_dbm = cur_signal;
            net.freq_mhz = cur_freq;
            net.security = sec;
            nets.push_back(net);
        }
    };

    const char* lp = raw.c_str();
    while (*lp) {
        const char* ls = lp;
        while (*lp && *lp != '\n') lp++;
        size_t ll = (size_t)(lp - ls);
        if (*lp == '\n') lp++;

        // New BSS block? "BSS aa:bb:cc:dd:ee:ff(on wlan0)"
        if (ll >= 4 && ls[0]=='B' && ls[1]=='S' && ls[2]=='S' && ls[3]==' ') {
            flush_bss();
            in_bss = true;
            cur_ssid.clear();
            cur_bssid.clear();
            cur_signal = -100;
            cur_freq = 0;
            cur_privacy = false;
            cur_is_eap = false;
            // Extract BSSID: "BSS aa:bb:cc:dd:ee:ff(on wlan0)"
            if (ll > 4) {
                const char* bp = ls + 4;
                const char* be = bp;
                while (be < ls + ll && *be != '(' && *be != ' ') be++;
                cur_bssid.assign(bp, (size_t)(be - bp));
            }
            continue;
        }

        if (!in_bss) continue;

        // Skip leading whitespace
        const char* tp = ls;
        while (tp < ls + ll && (*tp == ' ' || *tp == '\t')) tp++;
        size_t tl = ll - (size_t)(tp - ls);
        if (tl == 0) continue;

        if (tl > 6 && memcmp(tp, "SSID: ", 6) == 0) {
            cur_ssid.assign(tp + 6, tl - 6);
            while (!cur_ssid.empty() &&
                   (cur_ssid.back()==' '||cur_ssid.back()=='\r'))
                cur_ssid.pop_back();
        } else if (tl > 8 && memcmp(tp, "signal: ", 8) == 0) {
            cur_signal = atoi(tp + 8);
        } else if (tl > 6 && memcmp(tp, "freq: ", 6) == 0) {
            cur_freq = atoi(tp + 6);
        } else if (tl > 12 && memcmp(tp, "capability: ", 12) == 0) {
            std::string cap(tp + 12, tl - 12);
            if (cap.find("Privacy") != std::string::npos)
                cur_privacy = true;
        } else if ((tl >= 4 && memcmp(tp, "RSN:", 4) == 0) ||
                   (tl >= 4 && memcmp(tp, "WPA:", 4) == 0)) {
            cur_privacy = true;
        } else {
            // Detect Enterprise (802.1X/EAP) authentication within RSN/WPA IEs
            // After whitespace strip: "* Authentication suites: PSK" or "...IEEE 802.1X"
            std::string line(tp, tl);
            if (line.find("Authentication suites:") != std::string::npos) {
                if (line.find("802.1X") != std::string::npos ||
                    line.find("EAP") != std::string::npos) {
                    cur_is_eap = true;
                }
            }
        }
    }
    flush_bss();

    std::sort(nets.begin(), nets.end(),
              [](const WiFiNetwork& a, const WiFiNetwork& b) {
                  return a.signal_dbm > b.signal_dbm;
              });

    fprintf(stderr, "[wifi] iw_scan: found %zu networks\n", nets.size());
    return nets;
}

#else // _WIN32

// Stub implementation — WiFi runs only on the Linux target.
WiFiManager::WiFiManager() : ctrl_fd_(-1) {}
WiFiManager::~WiFiManager() {}
bool WiFiManager::init() { return false; }
bool WiFiManager::has_wifi() const { return false; }
bool WiFiManager::daemon_running() const { return false; }
WiFiStatus WiFiManager::status() { return {}; }
std::vector<WiFiNetwork> WiFiManager::scan() { return {}; }
std::vector<WiFiNetwork> WiFiManager::list_networks() { return {}; }
std::string WiFiManager::connect(const std::string&, const std::string&) {
    return "WiFi not available";
}
std::string WiFiManager::disconnect() { return "WiFi not available"; }
std::string WiFiManager::forget(const std::string&) { return "WiFi not available"; }
std::string WiFiManager::enable()  { return "WiFi not available"; }
std::string WiFiManager::disable() { return "WiFi not available"; }
bool WiFiManager::open_ctrl() { return false; }
void WiFiManager::close_ctrl() {}
std::string WiFiManager::wpa(const std::string&, int) { return ""; }
std::vector<WiFiNetwork> WiFiManager::parse_scan_results(const std::string&) { return {}; }
std::vector<WiFiNetwork> WiFiManager::parse_list_networks(const std::string&) { return {}; }
std::vector<WiFiNetwork> WiFiManager::iw_scan() { return {}; }
std::string WiFiManager::get_ip_addr() const { return ""; }

#endif // _WIN32
