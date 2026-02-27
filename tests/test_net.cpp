// test_net.cpp -- Unit tests for the mDNS responder
//
// Tests DNS name encoding/decoding, packet construction, and IP detection.
// Since we can't easily test full multicast I/O in a unit test, we focus on
// the helper functions and packet structure.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cstdint>
#include "../src/llamaste/net_mdns.h"

// Helper: read a big-endian uint16_t from a byte buffer
static uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static uint32_t read_u32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

// ---------------------------------------------------------------------------
// Test: DNS name encoding
// ---------------------------------------------------------------------------

static void test_encode_dns_name() {
    printf("test_encode_dns_name...\n");

    // "llamaste.local" -> \x08llamaste\x05local\x00
    auto encoded = MdnsResponder::encode_dns_name("llamaste.local");

    assert(encoded.size() == 16);  // 1 + 8 + 1 + 5 + 1 = 16
    assert(encoded[0] == 8);       // length of "llamaste"
    assert(encoded[1] == 'l');
    assert(encoded[2] == 'l');
    assert(encoded[3] == 'a');
    assert(encoded[4] == 'm');
    assert(encoded[5] == 'a');
    assert(encoded[6] == 's');
    assert(encoded[7] == 't');
    assert(encoded[8] == 'e');
    assert(encoded[9] == 5);       // length of "local"
    assert(encoded[10] == 'l');
    assert(encoded[11] == 'o');
    assert(encoded[12] == 'c');
    assert(encoded[13] == 'a');
    assert(encoded[14] == 'l');
    assert(encoded[15] == 0);      // root label

    printf("  PASS: llamaste.local encoding correct\n");

    // Single label "test"
    auto enc2 = MdnsResponder::encode_dns_name("test");
    assert(enc2.size() == 6);  // 1 + 4 + 1
    assert(enc2[0] == 4);
    assert(enc2[5] == 0);

    printf("  PASS: single label encoding correct\n");

    // Three labels "a.b.c"
    auto enc3 = MdnsResponder::encode_dns_name("a.b.c");
    assert(enc3.size() == 7);  // 1+1 + 1+1 + 1+1 + 1
    assert(enc3[0] == 1);   // 'a'
    assert(enc3[1] == 'a');
    assert(enc3[2] == 1);   // 'b'
    assert(enc3[3] == 'b');
    assert(enc3[4] == 1);   // 'c'
    assert(enc3[5] == 'c');
    assert(enc3[6] == 0);

    printf("  PASS: three-label encoding correct\n");
}

// ---------------------------------------------------------------------------
// Test: DNS name decoding
// ---------------------------------------------------------------------------

static void test_decode_dns_name() {
    printf("test_decode_dns_name...\n");

    // Decode from a manually constructed packet
    // \x08llamaste\x05local\x00
    uint8_t pkt[] = {
        8, 'l','l','a','m','a','s','t','e',
        5, 'l','o','c','a','l',
        0
    };
    size_t offset = 0;
    std::string name = MdnsResponder::decode_dns_name(pkt, sizeof(pkt), offset);
    assert(name == "llamaste.local");
    assert(offset == 16);  // Past the trailing zero

    printf("  PASS: basic name decoding correct\n");

    // Decode with compression pointer
    // Packet has a name at offset 0, then a pointer at offset 16 -> offset 0
    uint8_t pkt2[] = {
        // offset 0: "llamaste.local\0"
        8, 'l','l','a','m','a','s','t','e',
        5, 'l','o','c','a','l',
        0,
        // offset 16: compression pointer to offset 0
        0xC0, 0x00
    };
    size_t offset2 = 16;
    std::string name2 = MdnsResponder::decode_dns_name(pkt2, sizeof(pkt2), offset2);
    assert(name2 == "llamaste.local");
    assert(offset2 == 18);  // Past the 2-byte pointer

    printf("  PASS: compression pointer decoding correct\n");

    // Single label
    uint8_t pkt3[] = { 4, 't','e','s','t', 0 };
    size_t offset3 = 0;
    std::string name3 = MdnsResponder::decode_dns_name(pkt3, sizeof(pkt3), offset3);
    assert(name3 == "test");

    printf("  PASS: single label decoding correct\n");
}

// ---------------------------------------------------------------------------
// Test: build_response packet structure
// ---------------------------------------------------------------------------

static void test_build_response() {
    printf("test_build_response...\n");

    uint8_t ip[4] = {192, 168, 1, 42};
    auto pkt = MdnsResponder::build_response("llamaste.local", ip, 0x1234);

    // Minimum expected size: 12 (header) + 16 (name) + 2+2+4+2 (type,class,ttl,rdlen) + 4 (ip)
    assert(pkt.size() >= 40);

    // --- Verify header ---
    // Transaction ID
    assert(read_u16(pkt.data()) == 0x1234);

    // Flags: response (0x8000) + authoritative (0x0400) = 0x8400
    assert(read_u16(pkt.data() + 2) == 0x8400);

    // Questions = 0
    assert(read_u16(pkt.data() + 4) == 0);

    // Answers = 1
    assert(read_u16(pkt.data() + 6) == 1);

    // Authority = 0, Additional = 0
    assert(read_u16(pkt.data() + 8) == 0);
    assert(read_u16(pkt.data() + 10) == 0);

    printf("  PASS: header structure correct\n");

    // --- Verify answer section ---
    // Name starts at offset 12
    size_t off = 12;
    std::string name = MdnsResponder::decode_dns_name(pkt.data(), pkt.size(), off);
    assert(name == "llamaste.local");

    printf("  PASS: answer name correct\n");

    // Type A = 1
    assert(read_u16(pkt.data() + off) == 1);
    off += 2;

    // Class IN with cache-flush bit = 0x8001
    assert(read_u16(pkt.data() + off) == 0x8001);
    off += 2;

    // TTL = 120
    assert(read_u32(pkt.data() + off) == 120);
    off += 4;

    // RDLENGTH = 4
    assert(read_u16(pkt.data() + off) == 4);
    off += 2;

    // RDATA = 192.168.1.42
    assert(pkt[off + 0] == 192);
    assert(pkt[off + 1] == 168);
    assert(pkt[off + 2] == 1);
    assert(pkt[off + 3] == 42);

    printf("  PASS: answer A record correct (192.168.1.42)\n");

    // Test with different IP and query ID
    uint8_t ip2[4] = {10, 0, 0, 1};
    auto pkt2 = MdnsResponder::build_response("myhost.local", ip2, 0);
    assert(read_u16(pkt2.data()) == 0);
    assert(read_u16(pkt2.data() + 2) == 0x8400);

    printf("  PASS: alternate IP/ID works\n");
}

// ---------------------------------------------------------------------------
// Test: get_local_ip returns something reasonable
// ---------------------------------------------------------------------------

static void test_get_local_ip() {
    printf("test_get_local_ip...\n");

    std::string ip = MdnsResponder::get_local_ip();
    assert(!ip.empty());
    printf("  Local IP: %s\n", ip.c_str());

    // Should be a valid dotted-quad
    int parts = 0;
    unsigned a, b, c, d;
    if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        parts = 4;
    }
    assert(parts == 4);
    assert(a <= 255 && b <= 255 && c <= 255 && d <= 255);

    printf("  PASS: valid IPv4 format\n");
}

// ---------------------------------------------------------------------------
// Test: start/stop lifecycle (without actually needing multicast)
// ---------------------------------------------------------------------------

static void test_start_stop() {
    printf("test_start_stop...\n");

    MdnsResponder mdns;
    assert(!mdns.is_running());
    printf("  PASS: initially not running\n");

    // On Linux, this will try to bind to port 5353.
    // If we're not root, it may fail -- that's OK for testing.
    bool started = mdns.start("test-llamaste");

#ifdef _WIN32
    // Windows stub always returns false
    assert(!started);
    printf("  PASS: Windows stub correctly returns false\n");
#else
    if (started) {
        assert(mdns.is_running());
        printf("  PASS: started successfully\n");

        mdns.stop();
        assert(!mdns.is_running());
        printf("  PASS: stopped successfully\n");
    } else {
        // Not root or port in use -- skip gracefully
        printf("  SKIP: could not bind to port 5353 (need root or port in use)\n");
    }
#endif
}

// ---------------------------------------------------------------------------
// Test: encode then decode roundtrip
// ---------------------------------------------------------------------------

static void test_encode_decode_roundtrip() {
    printf("test_encode_decode_roundtrip...\n");

    std::vector<std::string> names = {
        "llamaste.local",
        "test.local",
        "a.b.c.d.e.local",
        "singlename",
    };

    for (const auto& name : names) {
        auto encoded = MdnsResponder::encode_dns_name(name);
        size_t offset = 0;
        std::string decoded = MdnsResponder::decode_dns_name(
            encoded.data(), encoded.size(), offset);
        assert(decoded == name);
        printf("  PASS: roundtrip '%s'\n", name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    printf("=== mDNS Responder Tests ===\n\n");

    test_encode_dns_name();
    test_decode_dns_name();
    test_build_response();
    test_get_local_ip();
    test_encode_decode_roundtrip();
    test_start_stop();

    printf("\nAll network tests passed.\n");
    return 0;
}
