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
static constexpr uint16_t DNS_TYPE_PTR = 12;
static constexpr uint16_t DNS_TYPE_TXT = 16;
static constexpr uint16_t DNS_TYPE_SRV = 33;
static constexpr uint16_t DNS_TYPE_ANY = 255;
static constexpr uint16_t DNS_CLASS_IN = 1;

// DNS header flags for an authoritative response
static constexpr uint16_t DNS_FLAG_RESPONSE       = 0x8000;
static constexpr uint16_t DNS_FLAG_AUTHORITATIVE   = 0x0400;

// TTL values: short for address/SRV, long for PTR/TXT (per RFC 6762)
static constexpr uint32_t MDNS_ADDR_TTL = 120;   // 2 min — address records
static constexpr uint32_t MDNS_SVC_TTL  = 4500;  // 75 min — service records

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
// Per-record append helpers (used by build_service_response)
// ---------------------------------------------------------------------------

// Append a PTR record: <browse_name> PTR <instance_name>
static void append_ptr(std::vector<uint8_t>& p,
                       const std::string& browse_name,
                       const std::string& instance_name,
                       uint32_t ttl = MDNS_SVC_TTL) {
    auto n = MdnsResponder::encode_dns_name(browse_name);
    p.insert(p.end(), n.begin(), n.end());
    put_u16(p, DNS_TYPE_PTR);
    put_u16(p, DNS_CLASS_IN);        // PTR is shared, no cache-flush bit
    put_u32(p, ttl);
    auto target = MdnsResponder::encode_dns_name(instance_name);
    put_u16(p, static_cast<uint16_t>(target.size()));
    p.insert(p.end(), target.begin(), target.end());
}

// Append an SRV record: <instance_name> SRV 0 0 <port> <host>
static void append_srv(std::vector<uint8_t>& p,
                       const std::string& instance_name,
                       const std::string& host_fqdn,
                       uint16_t port,
                       uint32_t ttl = MDNS_ADDR_TTL) {
    auto n = MdnsResponder::encode_dns_name(instance_name);
    p.insert(p.end(), n.begin(), n.end());
    put_u16(p, DNS_TYPE_SRV);
    put_u16(p, DNS_CLASS_IN | 0x8000);  // cache-flush
    put_u32(p, ttl);
    auto target = MdnsResponder::encode_dns_name(host_fqdn);
    put_u16(p, static_cast<uint16_t>(6 + target.size()));
    put_u16(p, 0);    // priority
    put_u16(p, 0);    // weight
    put_u16(p, port);
    p.insert(p.end(), target.begin(), target.end());
}

// Append a TXT record: <instance_name> TXT ["key=val", ...]
static void append_txt(std::vector<uint8_t>& p,
                       const std::string& instance_name,
                       const std::vector<std::string>& kvs,
                       uint32_t ttl = MDNS_SVC_TTL) {
    auto n = MdnsResponder::encode_dns_name(instance_name);
    p.insert(p.end(), n.begin(), n.end());
    put_u16(p, DNS_TYPE_TXT);
    put_u16(p, DNS_CLASS_IN | 0x8000);
    put_u32(p, ttl);
    // TXT RDATA: each string prefixed by 1-byte length
    std::vector<uint8_t> rdata;
    if (kvs.empty()) {
        rdata.push_back(0);  // empty TXT string (required by RFC)
    } else {
        for (const auto& kv : kvs) {
            uint8_t len = static_cast<uint8_t>(std::min(kv.size(), (size_t)255));
            rdata.push_back(len);
            rdata.insert(rdata.end(), kv.begin(), kv.begin() + len);
        }
    }
    put_u16(p, static_cast<uint16_t>(rdata.size()));
    p.insert(p.end(), rdata.begin(), rdata.end());
}

// Append an A record: <fqdn> A <ip[4]>
static void append_a(std::vector<uint8_t>& p,
                     const std::string& fqdn,
                     const uint8_t ip[4],
                     uint32_t ttl = MDNS_ADDR_TTL) {
    auto n = MdnsResponder::encode_dns_name(fqdn);
    p.insert(p.end(), n.begin(), n.end());
    put_u16(p, DNS_TYPE_A);
    put_u16(p, DNS_CLASS_IN | 0x8000);
    put_u32(p, ttl);
    put_u16(p, 4);
    p.push_back(ip[0]); p.push_back(ip[1]);
    p.push_back(ip[2]); p.push_back(ip[3]);
}

// ---------------------------------------------------------------------------
// Build service response: PTR in Answer, SRV+TXT+A in Additional
// ---------------------------------------------------------------------------

std::vector<uint8_t> MdnsResponder::build_service_response(
        const MdnsServiceRecord& svc,
        const std::string& host_fqdn,
        const uint8_t ip[4],
        uint16_t query_id) {
    std::vector<uint8_t> pkt;
    pkt.reserve(300);

    // Header: 0 questions, 1 answer (PTR), 3 additional (SRV+TXT+A)
    put_u16(pkt, query_id);
    put_u16(pkt, DNS_FLAG_RESPONSE | DNS_FLAG_AUTHORITATIVE);
    put_u16(pkt, 0);  // QDCOUNT
    put_u16(pkt, 1);  // ANCOUNT
    put_u16(pkt, 0);  // NSCOUNT
    put_u16(pkt, 3);  // ARCOUNT: SRV + TXT + A

    // Answer: PTR _mcp._tcp.local -> llamaste._mcp._tcp.local
    append_ptr(pkt, svc.browse_name, svc.instance_name);

    // Additional: SRV  llamaste._mcp._tcp.local -> host:port
    append_srv(pkt, svc.instance_name, host_fqdn, svc.port);

    // Additional: TXT  llamaste._mcp._tcp.local -> {path=/mcp, ...}
    append_txt(pkt, svc.instance_name, svc.txt);

    // Additional: A  llamaste.local -> IP
    append_a(pkt, host_fqdn, ip);

    return pkt;
}

// ---------------------------------------------------------------------------
// Build mDNS PTR query packet
// ---------------------------------------------------------------------------

std::vector<uint8_t> MdnsResponder::build_ptr_query(const std::string& service_fqdn) {
    std::vector<uint8_t> pkt;
    pkt.reserve(64);

    // DNS Header (12 bytes): QR=0 (query), QDCOUNT=1
    put_u16(pkt, 0);      // Transaction ID (mDNS uses 0)
    put_u16(pkt, 0);      // Flags: QR=0 (standard query)
    put_u16(pkt, 1);      // QDCOUNT = 1
    put_u16(pkt, 0);      // ANCOUNT = 0
    put_u16(pkt, 0);      // NSCOUNT = 0
    put_u16(pkt, 0);      // ARCOUNT = 0

    // Question section: service name + QTYPE=PTR + QCLASS=IN|unicast
    auto name = encode_dns_name(service_fqdn);
    pkt.insert(pkt.end(), name.begin(), name.end());
    put_u16(pkt, DNS_TYPE_PTR);    // QTYPE = PTR (12)
    put_u16(pkt, 0x8001);          // QCLASS = IN with unicast-response bit

    return pkt;
}

// ---------------------------------------------------------------------------
// Parse service responses from a DNS response packet
// ---------------------------------------------------------------------------

std::vector<MdnsResponder::DiscoveredService> MdnsResponder::parse_service_responses(
        const uint8_t* pkt, size_t pkt_len) {
    std::vector<DiscoveredService> results;

    if (pkt_len < 12) return results;

    // Parse header
    uint16_t flags   = get_u16(pkt + 2);
    uint16_t qdcount = get_u16(pkt + 4);
    uint16_t ancount = get_u16(pkt + 6);
    // uint16_t nscount = get_u16(pkt + 8);  // unused
    uint16_t arcount = get_u16(pkt + 10);

    // Must be a response
    if (!(flags & DNS_FLAG_RESPONSE)) return results;

    size_t offset = 12;

    // Skip question section
    for (uint16_t i = 0; i < qdcount && offset < pkt_len; ++i) {
        decode_dns_name(pkt, pkt_len, offset);  // skip name
        if (offset + 4 > pkt_len) return results;
        offset += 4;  // skip QTYPE + QCLASS
    }

    // We'll collect SRV, TXT, and A records, then merge them.
    // Key for SRV/TXT records is the instance name; for A records it's hostname.
    struct SrvInfo {
        std::string target;  // hostname (without .local)
        uint16_t port = 0;
    };

    // Maps keyed by record owner name
    std::vector<std::pair<std::string, SrvInfo>> srv_records;
    std::vector<std::pair<std::string, std::vector<std::string>>> txt_records;
    std::vector<std::pair<std::string, std::string>> a_records;  // fqdn -> ip

    // Parse answer + additional sections
    uint16_t total_rr = ancount + arcount;
    for (uint16_t i = 0; i < total_rr && offset < pkt_len; ++i) {
        // Record name
        std::string rr_name = decode_dns_name(pkt, pkt_len, offset);

        if (offset + 10 > pkt_len) break;
        uint16_t rr_type  = get_u16(pkt + offset); offset += 2;
        uint16_t rr_class = get_u16(pkt + offset); offset += 2;
        (void)rr_class;
        /* uint32_t rr_ttl = */ offset += 4;  // skip TTL
        uint16_t rdlength = get_u16(pkt + offset); offset += 2;

        if (offset + rdlength > pkt_len) break;

        size_t rdata_start = offset;

        if (rr_type == DNS_TYPE_SRV && rdlength >= 6) {
            // SRV RDATA: priority(2) + weight(2) + port(2) + target(variable)
            // uint16_t priority = get_u16(pkt + offset);
            offset += 2;  // skip priority
            // uint16_t weight = get_u16(pkt + offset);
            offset += 2;  // skip weight
            uint16_t port = get_u16(pkt + offset);
            offset += 2;

            std::string target = decode_dns_name(pkt, pkt_len, offset);

            SrvInfo info;
            info.port = port;
            // Strip ".local" suffix from target
            const std::string suffix = ".local";
            if (target.size() > suffix.size() &&
                target.compare(target.size() - suffix.size(), suffix.size(), suffix) == 0) {
                info.target = target.substr(0, target.size() - suffix.size());
            } else {
                info.target = target;
            }
            srv_records.push_back({rr_name, info});

        } else if (rr_type == DNS_TYPE_TXT && rdlength > 0) {
            // TXT RDATA: sequence of length-prefixed strings
            std::vector<std::string> kvs;
            size_t txt_end = rdata_start + rdlength;
            size_t pos = rdata_start;
            while (pos < txt_end) {
                uint8_t str_len = pkt[pos];
                ++pos;
                if (pos + str_len > txt_end) break;
                if (str_len > 0) {
                    kvs.emplace_back(reinterpret_cast<const char*>(pkt + pos), str_len);
                }
                pos += str_len;
            }
            txt_records.push_back({rr_name, kvs});
            offset = rdata_start + rdlength;

        } else if (rr_type == DNS_TYPE_A && rdlength == 4) {
            // A RDATA: 4-byte IPv4
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                     pkt[rdata_start], pkt[rdata_start + 1],
                     pkt[rdata_start + 2], pkt[rdata_start + 3]);
            a_records.push_back({rr_name, std::string(ip_str)});
            offset = rdata_start + rdlength;

        } else {
            // Skip unknown record types
            offset = rdata_start + rdlength;
        }
    }

    // Merge: each SRV record becomes a DiscoveredService, enriched with
    // matching TXT and A records.
    for (const auto& [srv_name, srv_info] : srv_records) {
        DiscoveredService ds;
        ds.hostname = srv_info.target;
        ds.port = srv_info.port;

        // Find matching TXT record (same owner name as SRV)
        for (const auto& [txt_name, txt_kvs] : txt_records) {
            if (dns_name_eq(txt_name, srv_name)) {
                ds.txt = txt_kvs;
                break;
            }
        }

        // Find matching A record for the SRV target hostname
        std::string target_fqdn = srv_info.target + ".local";
        for (const auto& [a_name, a_ip] : a_records) {
            if (dns_name_eq(a_name, target_fqdn)) {
                ds.ip = a_ip;
                break;
            }
        }

        results.push_back(ds);
    }

    return results;
}

// ---------------------------------------------------------------------------
// Discover services on the LAN via mDNS PTR queries
// ---------------------------------------------------------------------------

std::vector<MdnsResponder::DiscoveredService> MdnsResponder::discover_services(
        const std::string& service_type, int timeout_ms) {
    std::vector<DiscoveredService> results;

#ifndef _WIN32
    if (sock_ < 0 || !running_) return results;

    std::string service_fqdn = service_type + ".local";
    auto query = build_ptr_query(service_fqdn);

    // Send query twice (100ms gap for UDP loss tolerance)
    send_multicast(query);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    send_multicast(query);

    // Collect responses until timeout
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    uint8_t buf[1500];
    struct pollfd pfd;
    pfd.fd = sock_;
    pfd.events = POLLIN;

    while (std::chrono::steady_clock::now() < deadline) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;

        int ret = poll(&pfd, 1, static_cast<int>(remaining));
        if (ret <= 0) break;
        if (!(pfd.revents & POLLIN)) continue;

        struct sockaddr_in sender = {};
        socklen_t sender_len = sizeof(sender);
        ssize_t n = recvfrom(sock_, buf, sizeof(buf), 0,
                             reinterpret_cast<struct sockaddr*>(&sender),
                             &sender_len);
        if (n < 12) continue;

        size_t pkt_len = static_cast<size_t>(n);

        // Only parse response packets (QR=1)
        uint16_t flags = get_u16(buf + 2);
        if (!(flags & DNS_FLAG_RESPONSE)) continue;

        auto peers = parse_service_responses(buf, pkt_len);
        for (auto& peer : peers) {
            results.push_back(std::move(peer));
        }
    }

    // Deduplicate by IP + port
    std::vector<DiscoveredService> deduped;
    for (const auto& svc : results) {
        bool dup = false;
        for (const auto& existing : deduped) {
            if (existing.ip == svc.ip && existing.port == svc.port) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            deduped.push_back(svc);
        }
    }

    return deduped;
#else
    (void)service_type;
    (void)timeout_ms;
    return results;
#endif
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
// MdnsResponder public interface
// ---------------------------------------------------------------------------

// Send a packet to the mDNS multicast address (224.0.0.251:5353).
// May be called from any thread; sock_ must be valid.
void MdnsResponder::send_multicast(const std::vector<uint8_t>& pkt) {
#ifndef _WIN32
    if (sock_ < 0 || pkt.empty()) return;
    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port   = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_MCAST_ADDR, &dest.sin_addr);
    sendto(sock_, pkt.data(), pkt.size(), 0,
           reinterpret_cast<struct sockaddr*>(&dest), sizeof(dest));
#endif
}

void MdnsResponder::advertise_service(const std::string& service_type,
                                       uint16_t port,
                                       const std::vector<std::string>& txt) {
    MdnsServiceRecord svc;
    svc.browse_name    = service_type + ".local";         // _mcp._tcp.local
    svc.instance_name  = hostname_ + "." + svc.browse_name; // llamaste._mcp._tcp.local
    svc.port           = port;
    svc.txt            = txt;

    {
        std::lock_guard<std::mutex> lk(service_mu_);
        // Update existing or add new
        bool found = false;
        for (auto& s : services_) {
            if (s.browse_name == svc.browse_name) {
                s = svc;
                found = true;
                break;
            }
        }
        if (!found) {
            services_.push_back(svc);
        }
    }

    // Send proactive announcement so listeners don't have to query
    std::string local_ip = get_local_ip();
    uint8_t ip[4] = {};
    if (!parse_ipv4(local_ip, ip)) return;

    auto pkt = build_service_response(svc, fqdn_, ip);
    send_multicast(pkt);  // send twice to handle packet loss
    send_multicast(pkt);

    fprintf(stderr, "[mdns] Advertising %s on port %u (txt: %zu entries)\n",
            svc.instance_name.c_str(), port, txt.size());
}

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

            // Helper: refresh IP if still 0.0.0.0
            auto ensure_ip = [&]() {
                if (ip_bytes[0] == 0 && ip_bytes[1] == 0 &&
                    ip_bytes[2] == 0 && ip_bytes[3] == 0) {
                    local_ip = get_local_ip();
                    parse_ipv4(local_ip, ip_bytes);
                }
            };

            // --- A-record query for our hostname ---
            if ((qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY) &&
                qclass == DNS_CLASS_IN &&
                dns_name_eq(qname, fqdn_)) {

                ensure_ip();
                auto resp = build_response(fqdn_, ip_bytes, tx_id);
                send_multicast(resp);
            }

            // --- PTR query for a service we're advertising ---
            if ((qtype == DNS_TYPE_PTR || qtype == DNS_TYPE_ANY) &&
                qclass == DNS_CLASS_IN) {

                MdnsServiceRecord svc_copy;
                bool got_svc = false;
                {
                    std::lock_guard<std::mutex> lk(service_mu_);
                    for (const auto& s : services_) {
                        if (dns_name_eq(qname, s.browse_name)) {
                            svc_copy = s;
                            got_svc  = true;
                            break;
                        }
                    }
                }
                if (got_svc) {
                    ensure_ip();
                    auto resp = build_service_response(svc_copy, fqdn_, ip_bytes, tx_id);
                    send_multicast(resp);
                }
            }
        }
    }
#endif  // _WIN32
}
