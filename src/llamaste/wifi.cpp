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
// wpa() — send one command, receive one reply, skip event messages
// ---------------------------------------------------------------------------

std::string WiFiManager::wpa(const std::string& cmd, int timeout_ms) {
    if (ctrl_fd_ < 0 && !open_ctrl()) return "";

    if (send(ctrl_fd_, cmd.c_str(), cmd.size(), 0) < 0) {
        fprintf(stderr, "[wifi] send(%s) failed: %s\n",
                cmd.c_str(), strerror(errno));
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
        if      (key == "wpa_state")   s.state   = val;
        else if (key == "ssid")        s.ssid    = val;
        else if (key == "bssid")       s.bssid   = val;
        else if (key == "ip_address")  s.ip_addr = val;
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
    if (ctrl_fd_ < 0 && !open_ctrl()) return {};

    wpa("SCAN");  // reply may be "OK" or "FAIL-BUSY-SCAN-STARTED"

    // Wait for scan to finish (up to 4 seconds)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string st = wpa("STATUS");
        if (st.find("wpa_state=SCANNING") == std::string::npos) break;
    }

    // Short additional wait to let results settle
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string results = wpa("SCAN_RESULTS", 5000);
    return parse_scan_results(results);
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
    if (ctrl_fd_ < 0 && !open_ctrl()) return "wpa_supplicant not running";

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

    if (str_trim(wpa("SELECT_NETWORK " + std::to_string(net_id))) != "OK")
        return "SELECT_NETWORK failed";

    // Poll for COMPLETED (up to 15s)
    std::string last_state;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
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
std::string WiFiManager::get_ip_addr() const { return ""; }

#endif // _WIN32
