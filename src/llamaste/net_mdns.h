#pragma once
// net_mdns.h -- Minimal mDNS responder for Llamaste
//
// Responds to mDNS A-record queries for "<hostname>.local" AND DNS-SD
// service browsing queries (_<service>._tcp.local PTR) so that tools like
// avahi-browse can find the Llamaste MCP server without manual config.
//
// Multicast group 224.0.0.251:5353 (standard mDNS).
// Runs in a background thread, raw UDP sockets, no full mDNS stack.

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

// DNS-SD service record (populated by advertise_service).
struct MdnsServiceRecord {
    std::string browse_name;    // e.g. "_mcp._tcp.local"
    std::string instance_name;  // e.g. "llamaste._mcp._tcp.local"
    uint16_t    port = 80;
    std::vector<std::string> txt; // e.g. {"path=/mcp","version=2025-03-26"}
};

class MdnsResponder {
public:
    // Start responding to mDNS A-record queries for the given hostname.
    // hostname should NOT include the ".local" suffix.
    // Returns true if the socket was opened and the thread started.
    bool start(const std::string& hostname);

    // Announce a DNS-SD service over mDNS (PTR + SRV + TXT + A).
    // Must be called after start().  Sends a proactive announcement and
    // begins responding to PTR queries for "<service_type>.local".
    // service_type: e.g. "_mcp._tcp" (no ".local")
    void advertise_service(const std::string& service_type,
                           uint16_t port,
                           const std::vector<std::string>& txt = {});

    // Stop the responder (joins the background thread).
    void stop();

    // Check if the responder is running.
    bool is_running() const;

    ~MdnsResponder();

    // -- Public helpers (exposed for unit testing) --

    // Encode a DNS name into label format.
    // "llamaste.local" -> "\x08llamaste\x05local\x00"
    static std::vector<uint8_t> encode_dns_name(const std::string& fqdn);

    // Decode a DNS name from a raw packet starting at offset.
    // Returns the decoded dotted name and advances offset past it.
    static std::string decode_dns_name(const uint8_t* pkt, size_t pkt_len,
                                       size_t& offset);

    // Build a complete mDNS A-record response packet for the given
    // fully-qualified name and IPv4 address (4 bytes, network order).
    static std::vector<uint8_t> build_response(const std::string& fqdn,
                                                const uint8_t ip[4],
                                                uint16_t query_id = 0);

    // Build a PTR+SRV+TXT+A service response packet.
    static std::vector<uint8_t> build_service_response(
        const MdnsServiceRecord& svc,
        const std::string& host_fqdn,
        const uint8_t ip[4],
        uint16_t query_id = 0);

    // Get the IPv4 address of the first non-loopback interface.
    // Returns "0.0.0.0" if none found.
    static std::string get_local_ip();

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    int sock_ = -1;
    std::string hostname_;   // without ".local"
    std::string fqdn_;       // hostname_ + ".local"

    // Service advertisement (set once by advertise_service, read by run_loop)
    MdnsServiceRecord service_;
    bool              has_service_ = false;
    mutable std::mutex service_mu_;

    void run_loop();
    void send_multicast(const std::vector<uint8_t>& pkt);
};
