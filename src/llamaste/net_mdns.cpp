// net_mdns.cpp -- Minimal mDNS responder implementation
//
// Listens on 224.0.0.251:5353 for DNS A-record queries matching our hostname
// and replies with our IPv4 address.  See net_mdns.h for design notes.

#include "net_mdns.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

#ifdef _WIN32
// Windows stubs -- mDNS only runs on the real Linux target
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

static void close_socket(int s) { closesocket(s); }
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <fcntl.h>

static void close_socket(int s) { close(s); }
#endif

// mDNS constants
static constexpr const char* MDNS_MCAST_ADDR = "224.0.0.251";
static constexpr uint16_t    MDNS_PORT       = 5353;
static constexpr uint32_t    MDNS_TTL        = 120;  // seconds

// DNS record types / classes
static constexpr uint16_t DNS_TYPE_A   = 1;
static constexpr uint16_t DNS_CLASS_IN = 1;

// DNS header flags for an authoritative response
static constexpr uint16_t DNS_FLAG_RESPONSE       = 0x8000;
static constexpr uint16_t DNS_FLAG_AUTHORITATIVE   = 0x0400;

// ---------------------------------------------------------------------------
// Helpers: big-endian (network-order) read/write
// ---------------------------------------------------------------------------

static inline void put_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static inline void put_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >>  8) & 0xFF));
    buf.push_back(static_cast<uint8_t>( v        & 0xFF));
}

static inline uint16_t get_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// ---------------------------------------------------------------------------
// DNS name encoding / decoding
// ---------------------------------------------------------------------------

std::vector<uint8_t> MdnsResponder::encode_dns_name(const std::string& fqdn) {
    std::vector<uint8_t> out;
    size_t start = 0;
    while (start < fqdn.size()) {
        size_t dot = fqdn.find('.', start);
        if (dot == std::string::npos) dot = fqdn.size();
        size_t label_len = dot - start;
        if (label_len > 63) label_len = 63;  // DNS label limit
        out.push_back(static_cast<uint8_t>(label_len));
        for (size_t i = start; i < start + label_len; ++i) {
            out.push_back(static_cast<uint8_t>(fqdn[i]));
        }
        start = dot + 1;
    }
    out.push_back(0);  // root label
    return out;
}

std::string MdnsResponder::decode_dns_name(const uint8_t* pkt, size_t pkt_len,
                                            size_t& offset) {
    std::string name;
    bool jumped = false;
    size_t saved_offset = 0;
    int max_jumps = 10;  // prevent infinite loops from malformed packets

    while (offset < pkt_len && max_jumps > 0) {
        uint8_t len = pkt[offset];

        if (len == 0) {
            // End of name
            if (!jumped) ++offset;
            break;
        }

        // Compression pointer?
        if ((len & 0xC0) == 0xC0) {
            if (offset + 1 >= pkt_len) break;
            uint16_t ptr = static_cast<uint16_t>(((len & 0x3F) << 8) | pkt[offset + 1]);
            if (!jumped) {
                saved_offset = offset + 2;
            }
            offset = ptr;
            jumped = true;
            --max_jumps;
            continue;
        }

        // Normal label
        ++offset;
        if (offset + len > pkt_len) break;
        if (!name.empty()) name += '.';
        name.append(reinterpret_cast<const char*>(pkt + offset), len);
        offset += len;
    }

    if (jumped) {
        offset = saved_offset;
    }

    return name;
}

// ---------------------------------------------------------------------------
// Build mDNS response packet
// ---------------------------------------------------------------------------

std::vector<uint8_t> MdnsResponder::build_response(const std::string& fqdn,
                                                     const uint8_t ip[4],
                                                     uint16_t query_id) {
    std::vector<uint8_t> pkt;
    pkt.reserve(64);

    // --- DNS Header (12 bytes) ---
    put_u16(pkt, query_id);                          // Transaction ID
    put_u16(pkt, DNS_FLAG_RESPONSE | DNS_FLAG_AUTHORITATIVE);  // Flags
    put_u16(pkt, 0);                                 // Questions: 0
    put_u16(pkt, 1);                                 // Answers: 1
    put_u16(pkt, 0);                                 // Authority RRs: 0
    put_u16(pkt, 0);                                 // Additional RRs: 0

    // --- Answer section ---
    // Name
    auto name_bytes = encode_dns_name(fqdn);
    pkt.insert(pkt.end(), name_bytes.begin(), name_bytes.end());

    // Type A
    put_u16(pkt, DNS_TYPE_A);

    // Class IN with cache-flush bit set (0x8001)
    put_u16(pkt, DNS_CLASS_IN | 0x8000);

    // TTL
    put_u32(pkt, MDNS_TTL);

    // RDLENGTH = 4 (IPv4)
    put_u16(pkt, 4);

    // RDATA = IPv4 address
    pkt.push_back(ip[0]);
    pkt.push_back(ip[1]);
    pkt.push_back(ip[2]);
    pkt.push_back(ip[3]);

    return pkt;
}

// ---------------------------------------------------------------------------
// Get local (non-loopback) IPv4 address
// ---------------------------------------------------------------------------

std::string MdnsResponder::get_local_ip() {
#ifdef _WIN32
    // On Windows (dev only), return localhost
    return "127.0.0.1";
#else
    struct ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0) return "0.0.0.0";

    std::string result = "0.0.0.0";
    for (struct ifaddrs* ifa = addrs; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;

        auto* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        result = buf;
        break;  // Take first non-loopback interface
    }

    freeifaddrs(addrs);
    return result;
#endif
}

// ---------------------------------------------------------------------------
// Parse an IPv4 address string into 4 bytes
// ---------------------------------------------------------------------------

static bool parse_ipv4(const std::string& ip_str, uint8_t out[4]) {
    unsigned a, b, c, d;
    if (sscanf(ip_str.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = static_cast<uint8_t>(a);
    out[1] = static_cast<uint8_t>(b);
    out[2] = static_cast<uint8_t>(c);
    out[3] = static_cast<uint8_t>(d);
    return true;
}

// ---------------------------------------------------------------------------
// Case-insensitive string comparison for DNS names
// ---------------------------------------------------------------------------

static bool dns_name_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// MdnsResponder public interface
// ---------------------------------------------------------------------------

bool MdnsResponder::start(const std::string& hostname) {
    if (running_) return false;

    hostname_ = hostname;
    fqdn_ = hostname + ".local";

#ifdef _WIN32
    fprintf(stderr, "[mdns] mDNS not supported on Windows (dev stub)\n");
    return false;
#else
    // Create UDP socket
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) {
        perror("[mdns] socket");
        return false;
    }

    // Allow address reuse
    int optval = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
#ifdef SO_REUSEPORT
    setsockopt(sock_, SOL_SOCKET, SO_REUSEPORT, &optval, sizeof(optval));
#endif

    // Bind to mDNS port
    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(MDNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock_, reinterpret_cast<struct sockaddr*>(&bind_addr),
             sizeof(bind_addr)) < 0) {
        perror("[mdns] bind");
        close_socket(sock_);
        sock_ = -1;
        return false;
    }

    // Join multicast group
    struct ip_mreq mreq = {};
    inet_pton(AF_INET, MDNS_MCAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        perror("[mdns] IP_ADD_MEMBERSHIP");
        close_socket(sock_);
        sock_ = -1;
        return false;
    }

    // Set multicast TTL to 255 (standard for mDNS, link-local only)
    uint8_t mcast_ttl = 255;
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL,
               &mcast_ttl, sizeof(mcast_ttl));

    running_ = true;
    thread_ = std::thread(&MdnsResponder::run_loop, this);

    fprintf(stderr, "[mdns] Responding as %s (socket fd=%d)\n",
            fqdn_.c_str(), sock_);
    return true;
#endif
}

void MdnsResponder::stop() {
    if (!running_) return;
    running_ = false;

#ifndef _WIN32
    if (sock_ >= 0) {
        // Closing the socket will unblock the poll() in run_loop
        shutdown(sock_, SHUT_RDWR);
    }
#endif

    if (thread_.joinable()) {
        thread_.join();
    }

#ifndef _WIN32
    if (sock_ >= 0) {
        close_socket(sock_);
        sock_ = -1;
    }
#endif

    fprintf(stderr, "[mdns] Stopped\n");
}

bool MdnsResponder::is_running() const {
    return running_;
}

MdnsResponder::~MdnsResponder() {
    stop();
}

// ---------------------------------------------------------------------------
// Background thread: listen for queries and respond
// ---------------------------------------------------------------------------

void MdnsResponder::run_loop() {
#ifndef _WIN32
    uint8_t buf[1500];  // MTU-sized buffer

    // Detect our IP once (re-detect periodically if needed)
    std::string local_ip = get_local_ip();
    uint8_t ip_bytes[4] = {};
    if (!parse_ipv4(local_ip, ip_bytes)) {
        fprintf(stderr, "[mdns] Warning: could not parse local IP '%s'\n",
                local_ip.c_str());
    }
    fprintf(stderr, "[mdns] Local IP: %s\n", local_ip.c_str());

    struct pollfd pfd;
    pfd.fd = sock_;
    pfd.events = POLLIN;

    while (running_) {
        // Poll with 1-second timeout so we can check running_ flag
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_in sender = {};
        socklen_t sender_len = sizeof(sender);
        ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&sender),
                             &sender_len);
        if (n < 12) continue;  // Too short for DNS header

        size_t pkt_len = static_cast<size_t>(n);

        // --- Parse DNS header ---
        uint16_t tx_id = get_u16(buf);
        uint16_t flags = get_u16(buf + 2);
        uint16_t qdcount = get_u16(buf + 4);
        // uint16_t ancount = get_u16(buf + 6);  // unused

        // Ignore responses (we only care about queries)
        if (flags & DNS_FLAG_RESPONSE) continue;

        // Walk through the question section
        size_t offset = 12;  // Past header
        for (uint16_t i = 0; i < qdcount && offset < pkt_len; ++i) {
            std::string qname = decode_dns_name(buf, pkt_len, offset);
            if (offset + 4 > pkt_len) break;
            uint16_t qtype  = get_u16(buf + offset); offset += 2;
            uint16_t qclass = get_u16(buf + offset); offset += 2;

            // Strip cache-flush bit from class
            qclass &= 0x7FFF;

            // Check if this is an A-record query for our name
            if (qtype == DNS_TYPE_A && qclass == DNS_CLASS_IN &&
                dns_name_eq(qname, fqdn_)) {

                // Re-detect IP if we got 0.0.0.0 (interface may not have
                // been up when we started)
                if (ip_bytes[0] == 0 && ip_bytes[1] == 0 &&
                    ip_bytes[2] == 0 && ip_bytes[3] == 0) {
                    local_ip = get_local_ip();
                    parse_ipv4(local_ip, ip_bytes);
                }

                // Build and send response
                auto resp = build_response(fqdn_, ip_bytes, tx_id);

                // Send to the mDNS multicast address (standard behavior)
                struct sockaddr_in mcast_dest = {};
                mcast_dest.sin_family = AF_INET;
                mcast_dest.sin_port = htons(MDNS_PORT);
                inet_pton(AF_INET, MDNS_MCAST_ADDR, &mcast_dest.sin_addr);

                sendto(sock_, resp.data(), resp.size(), 0,
                       reinterpret_cast<struct sockaddr*>(&mcast_dest),
                       sizeof(mcast_dest));
            }
        }
    }
#endif  // _WIN32
}
