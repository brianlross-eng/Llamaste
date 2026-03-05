// tools_network.cpp — Network tools for Llamaste LLM-OS
//
// Provides network information by reading /sys/class/net/, /proc/net/tcp,
// and using POSIX socket APIs for DNS lookup and connectivity checks.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <chrono>

using json = nlohmann::json;

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

static std::string read_sysfs(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::string line;
    std::getline(f, line);
    return line;
}

// ---------------------------------------------------------------------------
// network.interfaces — list network interfaces
// ---------------------------------------------------------------------------
static std::string handle_network_interfaces(const std::string& args_json) {
    (void)args_json;

    DIR* dir = opendir("/sys/class/net");
    if (!dir) {
        return json_error("cannot read /sys/class/net");
    }

    json interfaces = json::array();
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        std::string base = "/sys/class/net/" + name;
        json iface;
        iface["name"] = name;

        // Read operstate (up/down/unknown)
        std::string state = read_sysfs(base + "/operstate");
        iface["state"] = state.empty() ? "unknown" : state;

        // Read MAC address
        std::string mac = read_sysfs(base + "/address");
        if (!mac.empty()) iface["mac"] = mac;

        // Read MTU
        std::string mtu_str = read_sysfs(base + "/mtu");
        if (!mtu_str.empty()) {
            try { iface["mtu"] = std::stoi(mtu_str); } catch (...) {}
        }

        // Read speed (only meaningful for physical interfaces)
        std::string speed = read_sysfs(base + "/speed");
        if (!speed.empty() && speed != "-1") {
            try { iface["speed_mbps"] = std::stoi(speed); } catch (...) {}
        }

        // Read TX/RX bytes from statistics
        std::string tx = read_sysfs(base + "/statistics/tx_bytes");
        std::string rx = read_sysfs(base + "/statistics/rx_bytes");
        if (!tx.empty()) {
            try { iface["tx_bytes"] = std::stol(tx); } catch (...) {}
        }
        if (!rx.empty()) {
            try { iface["rx_bytes"] = std::stol(rx); } catch (...) {}
        }

        // Read type (1 = ethernet, 772 = loopback, etc.)
        std::string type_str = read_sysfs(base + "/type");
        if (!type_str.empty()) {
            try {
                int type_num = std::stoi(type_str);
                switch (type_num) {
                    case 1: iface["link_type"] = "ethernet"; break;
                    case 772: iface["link_type"] = "loopback"; break;
                    case 65534: iface["link_type"] = "tun"; break;
                    default: iface["link_type"] = "other(" + type_str + ")"; break;
                }
            } catch (...) {}
        }

        interfaces.push_back(iface);
    }
    closedir(dir);

    json result;
    result["interfaces"] = interfaces;
    result["count"] = interfaces.size();
    return result.dump();
}

// ---------------------------------------------------------------------------
// network.connections — list TCP connections
// ---------------------------------------------------------------------------

// Parse a hex IP:port pair from /proc/net/tcp format
static std::string hex_to_ip_port(const std::string& hex_addr) {
    auto colon = hex_addr.find(':');
    if (colon == std::string::npos) return hex_addr;

    std::string ip_hex = hex_addr.substr(0, colon);
    std::string port_hex = hex_addr.substr(colon + 1);

    // IP address is in little-endian hex
    unsigned long ip_val = 0;
    try { ip_val = std::stoul(ip_hex, nullptr, 16); } catch (...) { return hex_addr; }

    struct in_addr addr;
    addr.s_addr = static_cast<in_addr_t>(ip_val);
    std::string ip = inet_ntoa(addr);

    unsigned int port = 0;
    try { port = std::stoul(port_hex, nullptr, 16); } catch (...) {}

    return ip + ":" + std::to_string(port);
}

static const char* tcp_state_name(int state) {
    switch (state) {
        case 1:  return "ESTABLISHED";
        case 2:  return "SYN_SENT";
        case 3:  return "SYN_RECV";
        case 4:  return "FIN_WAIT1";
        case 5:  return "FIN_WAIT2";
        case 6:  return "TIME_WAIT";
        case 7:  return "CLOSE";
        case 8:  return "CLOSE_WAIT";
        case 9:  return "LAST_ACK";
        case 10: return "LISTEN";
        case 11: return "CLOSING";
        default: return "UNKNOWN";
    }
}

static std::string handle_network_connections(const std::string& args_json) {
    (void)args_json;

    std::ifstream f("/proc/net/tcp");
    if (!f.is_open()) {
        return json_error("cannot read /proc/net/tcp");
    }

    json connections = json::array();
    std::string line;
    std::getline(f, line); // Skip header

    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string sl, local_addr, remote_addr, state_hex;
        ss >> sl >> local_addr >> remote_addr >> state_hex;

        if (local_addr.empty()) continue;

        int state_num = 0;
        try { state_num = std::stoi(state_hex, nullptr, 16); } catch (...) {}

        json conn;
        conn["local"] = hex_to_ip_port(local_addr);
        conn["remote"] = hex_to_ip_port(remote_addr);
        conn["state"] = tcp_state_name(state_num);

        connections.push_back(conn);
    }

    json result;
    result["connections"] = connections;
    result["count"] = connections.size();
    return result.dump();
}

// ---------------------------------------------------------------------------
// network.dns_lookup — resolve hostname using getaddrinfo
// ---------------------------------------------------------------------------
static std::string handle_network_dns_lookup(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string hostname = args.value("hostname", "");
    if (hostname.empty()) return json_error("hostname is required");

    // Basic validation: no slashes, reasonable length
    if (hostname.size() > 253 || hostname.find('/') != std::string::npos) {
        return json_error("invalid hostname");
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(hostname.c_str(), nullptr, &hints, &res);
    if (err != 0) {
        json result;
        result["error"] = "DNS lookup failed: " + std::string(gai_strerror(err));
        result["hostname"] = hostname;
        return result.dump();
    }

    json addresses = json::array();
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char buf[INET6_ADDRSTRLEN];
        void* addr;
        std::string family;

        if (p->ai_family == AF_INET) {
            addr = &((struct sockaddr_in*)p->ai_addr)->sin_addr;
            family = "IPv4";
        } else if (p->ai_family == AF_INET6) {
            addr = &((struct sockaddr_in6*)p->ai_addr)->sin6_addr;
            family = "IPv6";
        } else {
            continue;
        }

        inet_ntop(p->ai_family, addr, buf, sizeof(buf));
        json entry;
        entry["address"] = std::string(buf);
        entry["family"] = family;
        addresses.push_back(entry);
    }
    freeaddrinfo(res);

    json result;
    result["hostname"] = hostname;
    result["addresses"] = addresses;
    result["count"] = addresses.size();
    return result.dump();
}

// ---------------------------------------------------------------------------
// network.ping — basic reachability check via TCP connect
// ---------------------------------------------------------------------------
static std::string handle_network_ping(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string host = args.value("host", "");
    if (host.empty()) return json_error("host is required");
    int port = args.value("port", 80);
    int timeout_ms = args.value("timeout_ms", 3000);
    if (timeout_ms > 10000) timeout_ms = 10000;  // Cap at 10 seconds

    // Resolve hostname
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    std::string port_str = std::to_string(port);
    int err = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res);
    if (err != 0) {
        json result;
        result["host"] = host;
        result["port"] = port;
        result["reachable"] = false;
        result["error"] = "DNS resolution failed: " + std::string(gai_strerror(err));
        return result.dump();
    }

    // Create non-blocking socket
    int sock = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock < 0) {
        freeaddrinfo(res);
        json result;
        result["host"] = host;
        result["port"] = port;
        result["reachable"] = false;
        result["error"] = "socket creation failed";
        return result.dump();
    }

    // Attempt non-blocking connect
    auto start = std::chrono::steady_clock::now();
    int ret = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    bool reachable = false;
    if (ret == 0) {
        reachable = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLOUT;
        int poll_ret = poll(&pfd, 1, timeout_ms);
        if (poll_ret > 0 && (pfd.revents & POLLOUT)) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
            reachable = (so_error == 0);
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    close(sock);

    json result;
    result["host"] = host;
    result["port"] = port;
    result["reachable"] = reachable;
    result["latency_ms"] = elapsed_ms;
    return result.dump();
}

// ---------------------------------------------------------------------------
// network.get_ip — get current IP configuration
// ---------------------------------------------------------------------------
static std::string handle_network_get_ip(const std::string& args_json) {
    (void)args_json;
    json result;

    // Read saved config
    std::ifstream f("/data/config/network.json");
    if (f.is_open()) {
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        f.close();
        auto cfg = json::parse(content, nullptr, false);
        if (!cfg.is_discarded()) {
            result["config"] = cfg;
        }
    }

    if (!result.contains("config")) {
        result["config"] = {{"mode", "dhcp"}};
    }

    // Read current active IP from first non-loopback interface
    DIR* dir = opendir("/sys/class/net");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name = ent->d_name;
            if (name == "." || name == ".." || name == "lo") continue;
            std::string state = read_sysfs("/sys/class/net/" + name + "/operstate");
            if (state != "up") continue;

            // Get IP via ioctl
            int sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock >= 0) {
                struct ifreq ifr;
                memset(&ifr, 0, sizeof(ifr));
                strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
                if (ioctl(sock, SIOCGIFADDR, &ifr) == 0) {
                    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_addr;
                    result["active_ip"] = inet_ntoa(addr->sin_addr);
                    result["active_interface"] = name;
                }
                if (ioctl(sock, SIOCGIFNETMASK, &ifr) == 0) {
                    struct sockaddr_in* addr = (struct sockaddr_in*)&ifr.ifr_netmask;
                    result["active_netmask"] = inet_ntoa(addr->sin_addr);
                }
                close(sock);
            }
            break;  // Use first active interface
        }
        closedir(dir);
    }

    // Read DNS
    std::ifstream resolv("/etc/resolv.conf");
    if (resolv.is_open()) {
        std::string line;
        while (std::getline(resolv, line)) {
            if (line.substr(0, 11) == "nameserver ") {
                result["active_dns"] = line.substr(11);
                break;
            }
        }
    }

    return result.dump();
}

// ---------------------------------------------------------------------------
// network.set_ip — configure static IP or switch back to DHCP
// ---------------------------------------------------------------------------
static std::string handle_network_set_ip(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string mode = args.value("mode", "");
    if (mode != "dhcp" && mode != "static")
        return json_error("mode must be 'dhcp' or 'static'");

    json config;
    config["mode"] = mode;

    if (mode == "static") {
        std::string ip = args.value("ip", "");
        if (ip.empty()) return json_error("ip is required for static mode");

        // Validate IP format
        struct in_addr test;
        if (inet_pton(AF_INET, ip.c_str(), &test) != 1)
            return json_error("invalid IP address format");

        config["ip"] = ip;
        config["netmask"] = args.value("netmask", "255.255.255.0");
        config["gateway"] = args.value("gateway", "");
        config["dns"] = args.value("dns", "8.8.8.8");
        config["interface"] = args.value("interface", "eth0");

        // Validate netmask
        if (inet_pton(AF_INET, config["netmask"].get<std::string>().c_str(), &test) != 1)
            return json_error("invalid netmask format");

        // Validate gateway if provided
        std::string gw = config["gateway"].get<std::string>();
        if (!gw.empty() && inet_pton(AF_INET, gw.c_str(), &test) != 1)
            return json_error("invalid gateway format");
    }

    // Save config
    std::string dir_path = "/data/config";
    mkdir(dir_path.c_str(), 0755);
    std::ofstream out(dir_path + "/network.json");
    if (!out.is_open())
        return json_error("cannot write network config");
    out << config.dump(2);
    out.close();

    json result;
    result["status"] = "saved";
    result["config"] = config;
    result["message"] = (mode == "dhcp")
        ? "Switched to DHCP. Reboot to apply."
        : "Static IP configured. Reboot to apply.";
    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_network_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "network.interfaces",
        .description = "List all network interfaces with state, MAC address, MTU, "
                       "speed, and traffic statistics. Reads from /sys/class/net/.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_network_interfaces
    });

    reg.register_tool({
        .name = "network.connections",
        .description = "List active TCP connections showing local address, remote "
                       "address, and connection state. Reads from /proc/net/tcp.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_network_connections
    });

    reg.register_tool({
        .name = "network.dns_lookup",
        .description = "Resolve a hostname to IP addresses using DNS. Returns both "
                       "IPv4 and IPv6 addresses if available.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "hostname": {
                    "type": "string",
                    "description": "Hostname to resolve (e.g., 'example.com')"
                }
            },
            "required": ["hostname"]
        })json",
        .handler = handle_network_dns_lookup
    });

    reg.register_tool({
        .name = "network.get_ip",
        .description = "Get current IP configuration — shows whether DHCP or static IP "
                       "is configured, the active IP address, netmask, gateway, and DNS.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_network_get_ip
    });

    reg.register_tool({
        .name = "network.set_ip",
        .description = "Configure network IP address. Set mode to 'dhcp' for automatic "
                       "address or 'static' with ip/netmask/gateway/dns. Changes take "
                       "effect on next reboot.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "mode": {
                    "type": "string",
                    "description": "IP mode: 'dhcp' or 'static'",
                    "enum": ["dhcp", "static"]
                },
                "ip": {
                    "type": "string",
                    "description": "Static IP address (e.g., '192.168.1.100')"
                },
                "netmask": {
                    "type": "string",
                    "description": "Subnet mask (default: '255.255.255.0')",
                    "default": "255.255.255.0"
                },
                "gateway": {
                    "type": "string",
                    "description": "Default gateway IP address"
                },
                "dns": {
                    "type": "string",
                    "description": "DNS server IP (default: '8.8.8.8')",
                    "default": "8.8.8.8"
                },
                "interface": {
                    "type": "string",
                    "description": "Network interface name (default: 'eth0')",
                    "default": "eth0"
                }
            },
            "required": ["mode"]
        })json",
        .handler = handle_network_set_ip
    });

    reg.register_tool({
        .name = "network.ping",
        .description = "Check if a host is reachable by attempting a TCP connection "
                       "to the specified port. Returns reachability and latency.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "host": {
                    "type": "string",
                    "description": "Hostname or IP address to check"
                },
                "port": {
                    "type": "integer",
                    "description": "TCP port to connect to (default: 80)",
                    "default": 80
                },
                "timeout_ms": {
                    "type": "integer",
                    "description": "Connection timeout in milliseconds (default: 3000, max: 10000)",
                    "default": 3000
                }
            },
            "required": ["host"]
        })json",
        .handler = handle_network_ping
    });
}
