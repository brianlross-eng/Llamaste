// wifi.h — WiFi manager using wpa_supplicant control protocol
//
// Manages WiFi connectivity by communicating directly with wpa_supplicant
// via its Unix domain socket control interface. No external library required —
// the protocol is simple ASCII commands over SOCK_DGRAM.
//
// Interface detection: /sys/class/net/<iface>/wireless/
// Control socket:      /run/wpa_supplicant/<iface>
// Config storage:      /data/llamaste/wifi/wpa.conf

#pragma once
#include <string>
#include <vector>

// Information about a visible or saved WiFi network.
struct WiFiNetwork {
    std::string ssid;
    std::string bssid;
    int signal_dbm = 0;       // e.g. -65 (0 = unknown)
    int freq_mhz   = 0;       // e.g. 2437 (0 = unknown), from SCAN_RESULTS col 2
    std::string security;     // "WPA2-PSK", "WPA-PSK", "OPEN"
    int network_id = -1;      // wpa_supplicant network id (-1 = not saved)
    bool connected = false;
    bool saved = false;
};

// Current WiFi state.
struct WiFiStatus {
    bool available = false;      // WiFi hardware detected
    bool daemon_running = false; // wpa_supplicant control socket reachable
    std::string iface;           // Interface name (e.g. "wlan0")
    std::string ssid;            // Current SSID (empty if not connected)
    std::string bssid;
    std::string ip_addr;
    std::string state;           // DISCONNECTED, SCANNING, ASSOCIATED, COMPLETED, etc.
    int signal_dbm = 0;
};

class WiFiManager {
public:
    WiFiManager();
    ~WiFiManager();

    // Detect WiFi interface (/sys/class/net/*/wireless).
    // Opens control socket if wpa_supplicant is already running.
    // Returns true if a WiFi interface was found.
    bool init();

    // True if a WiFi interface was found during init().
    bool has_wifi() const;

    // True if the wpa_supplicant control socket is reachable.
    bool daemon_running() const;

    // Current connection status.
    WiFiStatus status();

    // Trigger a passive scan and return visible networks.
    // Blocks up to ~4 seconds for scan to complete.
    std::vector<WiFiNetwork> scan();

    // List saved networks from wpa_supplicant.
    std::vector<WiFiNetwork> list_networks();

    // Connect to a network. psk="" for open networks.
    // Returns "" on success, or an error string.
    std::string connect(const std::string& ssid, const std::string& psk);

    // Disconnect from the current network.
    std::string disconnect();

    // Remove a saved network by ssid. Returns "" on success.
    std::string forget(const std::string& ssid);

    // Bring the WiFi interface up.
    std::string enable();

    // Bring the WiFi interface down.
    std::string disable();

private:
    std::string iface_;     // detected interface name
    int ctrl_fd_ = -1;      // our side of the socket pair
    std::string ctrl_path_; // /tmp/wpa_ctrl_<pid>_<N>

    // Send a wpa_supplicant command and receive the reply.
    // Returns the reply string, or "" on timeout/error.
    std::string wpa(const std::string& cmd, int timeout_ms = 3000);

    // Open control socket to /run/wpa_supplicant/<iface_>.
    bool open_ctrl();
    void close_ctrl();

    // Parse SCAN_RESULTS output into a list of networks.
    static std::vector<WiFiNetwork> parse_scan_results(const std::string& raw);

    // Parse LIST_NETWORKS output into a list of saved networks.
    static std::vector<WiFiNetwork> parse_list_networks(const std::string& raw);

    // Run ip addr show <iface> to get the current IP address.
    std::string get_ip_addr() const;
};
