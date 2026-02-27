#pragma once
// net_mdns.h -- Minimal mDNS responder for Llamaste
//
// Responds to mDNS A-record queries for "<hostname>.local" on the standard
// multicast group 224.0.0.251:5353.  This lets other machines on the LAN
// discover the Llamaste box by name (e.g., http://llamaste.local/).
//
// The responder runs in a background thread and uses raw UDP sockets.
// It only answers queries for our own name -- it does not implement a full
// mDNS stack (no browsing, no service discovery, no NSEC, etc.).

#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <cstdint>

class MdnsResponder {
public:
    // Start responding to mDNS queries for the given hostname.
    // hostname should NOT include the ".local" suffix.
    // Returns true if the socket was opened and the thread started.
    bool start(const std::string& hostname);

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

    // Get the IPv4 address of the first non-loopback interface.
    // Returns "0.0.0.0" if none found.
    static std::string get_local_ip();

private:
    std::atomic<bool> running_{false};
    std::thread thread_;
    int sock_ = -1;
    std::string hostname_;   // without ".local"
    std::string fqdn_;       // hostname_ + ".local"

    void run_loop();
};
