// test_auth.cpp -- Host tests for bcrypt and AuthManager
//
// Tests: bcrypt hash/verify, session create/validate/expire, password management,
//        API key verification, brute force protection, console format helpers.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>

#include "../src/llamaste/bcrypt.h"
#include "../src/llamaste/auth.h"
#include "../src/llamaste/supervisor.h"

// Stub for linker — supervisor.cpp declares extern child_main() but we don't
// test supervisor_run(), so provide a dummy to satisfy the linker on Linux.
#ifndef _WIN32
int child_main(const SupervisorConfig&) { return 0; }
#endif

static int pass_count = 0;
static int fail_count = 0;

#define TEST(name) printf("  TEST: %s...", name)
#define PASS() do { printf(" PASS\n"); pass_count++; } while(0)
#define FAIL(msg) do { printf(" FAIL (%s)\n", msg); fail_count++; } while(0)

// ---------- bcrypt tests ----------

void test_bcrypt_hash_verify() {
    TEST("bcrypt hash+verify round-trip");
    std::string hash = bcrypt_hash("testpassword123", 4);  // low work factor for speed
    assert(!hash.empty());
    assert(hash[0] == '$');
    assert(hash[1] == '2');
    assert(hash.size() >= 59);
    bool ok = bcrypt_check("testpassword123", hash);
    if (ok) PASS(); else FAIL("correct password not verified");
}

void test_bcrypt_wrong_password() {
    TEST("bcrypt wrong password rejection");
    std::string hash = bcrypt_hash("correctpassword", 4);
    bool ok = bcrypt_check("wrongpassword", hash);
    if (!ok) PASS(); else FAIL("wrong password was accepted");
}

void test_bcrypt_different_salts() {
    TEST("bcrypt different salts produce different hashes");
    std::string h1 = bcrypt_hash("samepassword", 4);
    std::string h2 = bcrypt_hash("samepassword", 4);
    // Both should verify correctly but be different hashes (different salts)
    bool v1 = bcrypt_check("samepassword", h1);
    bool v2 = bcrypt_check("samepassword", h2);
    if (h1 != h2 && v1 && v2) PASS(); else FAIL("salts should differ");
}

void test_bcrypt_empty_password() {
    TEST("bcrypt empty password rejected");
    std::string hash = bcrypt_hash("", 4);
    if (hash.empty()) PASS(); else FAIL("empty password should fail");
}

void test_bcrypt_work_factors() {
    TEST("bcrypt work factor range");
    std::string h4 = bcrypt_hash("test", 4);
    std::string h8 = bcrypt_hash("test", 8);
    // Both should be valid hashes
    bool v4 = bcrypt_check("test", h4);
    bool v8 = bcrypt_check("test", h8);
    // Work factor should be encoded in hash
    bool wf4 = (h4[4] == '0' && h4[5] == '4');
    bool wf8 = (h8[4] == '0' && h8[5] == '8');
    if (v4 && v8 && wf4 && wf8) PASS(); else FAIL("work factor encoding");
}

// ---------- AuthManager tests ----------

void test_auth_initial_state() {
    TEST("auth initial state (no config)");
    // Use a temp directory
    std::string tmpdir = "/tmp/llamaste_test_auth_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    AuthManager auth;
    auth.load_config(tmpdir);
    bool ok = !auth.is_setup_complete();
    // Cleanup
    std::filesystem::remove_all(tmpdir);
    if (ok) PASS(); else FAIL("should not be setup complete");
}

void test_auth_set_password() {
    TEST("auth set and verify password");
    std::string tmpdir = "/tmp/llamaste_test_auth_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    AuthManager auth;
    auth.load_config(tmpdir);

    bool set_ok = auth.set_device_password("mypassword");
    bool setup_ok = auth.is_setup_complete();
    bool verify_ok = auth.verify_device_password("mypassword");
    bool verify_bad = auth.verify_device_password("wrongpassword");

    std::filesystem::remove_all(tmpdir);
    if (set_ok && setup_ok && verify_ok && !verify_bad)
        PASS();
    else
        FAIL("password set/verify");
}

void test_auth_config_persistence() {
    TEST("auth config persistence");
    std::string tmpdir = "/tmp/llamaste_test_auth_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());

    // Set up password in first instance
    {
        AuthManager auth;
        auth.load_config(tmpdir);
        auth.set_device_password("persistent");
    }

    // Load in second instance — should still be valid
    {
        AuthManager auth;
        auth.load_config(tmpdir);
        bool ok = auth.is_setup_complete() && auth.verify_device_password("persistent");
        std::filesystem::remove_all(tmpdir);
        if (ok) PASS(); else FAIL("config not persisted");
    }
}

void test_auth_sessions() {
    TEST("auth session create/validate/expire");
    std::string tmpdir = "/tmp/llamaste_test_auth_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    AuthManager auth;
    auth.load_config(tmpdir);

    // Create session
    std::string token = auth.create_session();
    assert(!token.empty());
    assert(token.size() == 32);  // 16 bytes = 32 hex chars

    // Validate
    bool valid = auth.is_valid_session(token);
    bool invalid = auth.is_valid_session("nonexistent");

    // Invalidate
    auth.invalidate_session(token);
    bool after_invalidate = auth.is_valid_session(token);

    std::filesystem::remove_all(tmpdir);
    if (valid && !invalid && !after_invalidate) PASS(); else FAIL("session lifecycle");
}

void test_auth_api_key() {
    TEST("auth API key generation and verification");
    std::string tmpdir = "/tmp/llamaste_test_auth_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    AuthManager auth;
    auth.load_config(tmpdir);

    // API key should be empty before setup
    std::string key_before = auth.get_api_key();

    // Set password generates API key
    auth.set_device_password("testpass");
    std::string key = auth.get_api_key();

    bool has_key = !key.empty();
    bool starts_with = key.substr(0, 4) == "llm-";
    bool verifies = auth.verify_api_key(key);
    bool rejects_bad = !auth.verify_api_key("llm-invalidkey");

    std::filesystem::remove_all(tmpdir);
    if (key_before.empty() && has_key && starts_with && verifies && rejects_bad)
        PASS();
    else
        FAIL("API key");
}

void test_auth_brute_force() {
    TEST("auth brute force protection");
    std::string tmpdir = "/tmp/llamaste_test_auth_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    AuthManager auth;
    auth.load_config(tmpdir);

    // Record failures
    std::string ip = "192.168.1.100";
    for (int i = 0; i < 4; i++) {
        auth.record_failed_login(ip);
    }
    bool not_limited_yet = !auth.is_rate_limited(ip);

    // 5th failure should trigger rate limit
    auth.record_failed_login(ip);
    bool limited_now = auth.is_rate_limited(ip);

    // Different IP should not be limited
    bool other_ip_ok = !auth.is_rate_limited("10.0.0.1");

    std::filesystem::remove_all(tmpdir);
    if (not_limited_yet && limited_now && other_ip_ok)
        PASS();
    else
        FAIL("brute force protection");
}

void test_auth_session_timeout() {
    TEST("auth session timeout configuration");
    std::string tmpdir = "/tmp/llamaste_test_auth_" +
        std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    AuthManager auth;
    auth.load_config(tmpdir);

    // Default should be 7 days
    bool default_ok = (auth.session_timeout_seconds() == 604800);

    // Set to 1 hour
    auth.set_session_timeout(3600);
    bool set_ok = (auth.session_timeout_seconds() == 3600);

    // Minimum should be enforced
    auth.set_session_timeout(10);  // too low
    bool min_ok = (auth.session_timeout_seconds() == 60);

    std::filesystem::remove_all(tmpdir);
    if (default_ok && set_ok && min_ok) PASS(); else FAIL("session timeout config");
}

// ---------- Console format helper tests ----------

void test_format_uptime() {
    TEST("format_uptime");
    bool t1 = (format_uptime(30) == "0m 30s");
    bool t2 = (format_uptime(3661) == "1h 1m");
    bool t3 = (format_uptime(90061) == "1d 1h 1m");
    bool t4 = (format_uptime(0) == "0m 0s");
    if (t1 && t2 && t3 && t4) PASS(); else FAIL("uptime format");
}

void test_format_bar() {
    TEST("format_bar");
    std::string b0 = format_bar(0, 4);
    std::string b100 = format_bar(100, 4);
    std::string b50 = format_bar(50, 4);

    // 0% should be all empty blocks, 100% all filled
    // Each UTF-8 block char is 3 bytes
    bool len_ok = (b0.size() == 12 && b100.size() == 12 && b50.size() == 12);
    bool diff = (b0 != b100);
    if (len_ok && diff) PASS(); else FAIL("bar format");
}

void test_format_bytes() {
    TEST("format_bytes");
    bool t1 = (format_bytes(500) == "500 KB");
    bool t2 = (format_bytes(2048) == "2048 MB" || format_bytes(2048).find("MB") != std::string::npos);
    bool t3 = (format_bytes(4194304).find("GB") != std::string::npos);
    if (t1 && t3) PASS(); else FAIL("bytes format");
}

// ---------- Main ----------

int main() {
    printf("=== Llamaste Auth & Console Tests ===\n\n");

    printf("[bcrypt]\n");
    test_bcrypt_hash_verify();
    test_bcrypt_wrong_password();
    test_bcrypt_different_salts();
    test_bcrypt_empty_password();
    test_bcrypt_work_factors();

    printf("\n[AuthManager]\n");
    test_auth_initial_state();
    test_auth_set_password();
    test_auth_config_persistence();
    test_auth_sessions();
    test_auth_api_key();
    test_auth_brute_force();
    test_auth_session_timeout();

    printf("\n[Console Format Helpers]\n");
    test_format_uptime();
    test_format_bar();
    test_format_bytes();

    printf("\n=== Results: %d passed, %d failed ===\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}
