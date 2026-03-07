// tests/test_updater.cpp -- Updater unit tests: slot detection, grubenv, version, manifest
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <fstream>
#include "updater.h"

static int tests_run = 0, tests_passed = 0;
#define TEST(name) do { tests_run++; printf("  TEST: %s ... ", #name); name(); tests_passed++; printf("PASS\n"); } while(0)

// ---------------------------------------------------------------------------
// Slot detection tests
// ---------------------------------------------------------------------------

static void test_parse_slot_A() {
    assert(parse_active_slot("root=/dev/sda3 llamaste.slot=A quiet") == "A");
}

static void test_parse_slot_B() {
    assert(parse_active_slot("root=/dev/sda4 llamaste.slot=B quiet") == "B");
}

static void test_parse_slot_missing() {
    assert(parse_active_slot("root=/dev/sda3 quiet") == "A");
}

static void test_inactive_slot_A() {
    assert(inactive_slot("A") == "B");
}

static void test_inactive_slot_B() {
    assert(inactive_slot("B") == "A");
}

static void test_inactive_partition_A() {
    // A active -> inactive is B (partition 4)
    assert(inactive_partition_num("A") == 4);
}

static void test_inactive_partition_B() {
    // B active -> inactive is A (partition 3)
    assert(inactive_partition_num("B") == 3);
}

// ---------------------------------------------------------------------------
// GRUB environment block tests
// ---------------------------------------------------------------------------

static void test_grubenv_parse() {
    std::string content =
        "# GRUB Environment Block\n"
        "saved_entry=llamaste-a\n"
        "llamaste_slot=A\n"
        "boot_success=1\n"
        "###########################";
    auto vars = grubenv_parse(content);
    assert(vars.size() == 3);
    assert(vars["saved_entry"] == "llamaste-a");
    assert(vars["llamaste_slot"] == "A");
    assert(vars["boot_success"] == "1");
}

static void test_grubenv_serialize() {
    std::map<std::string, std::string> vars;
    vars["llamaste_slot"] = "A";
    vars["boot_success"] = "1";

    std::string block = grubenv_serialize(vars);
    assert(block.size() == 1024);
    // Must start with header
    assert(block.find("# GRUB Environment Block\n") == 0);
    // Must contain key=value pairs
    assert(block.find("boot_success=1\n") != std::string::npos);
    assert(block.find("llamaste_slot=A\n") != std::string::npos);
    // Must be padded with '#'
    assert(block[1023] == '#');
}

static void test_grubenv_roundtrip() {
    std::map<std::string, std::string> original;
    original["saved_entry"] = "llamaste-b";
    original["llamaste_slot"] = "B";
    original["boot_success"] = "0";
    original["boot_counter"] = "3";

    std::string block = grubenv_serialize(original);
    assert(block.size() == 1024);

    auto parsed = grubenv_parse(block);
    assert(parsed.size() == original.size());
    for (const auto& [key, val] : original) {
        assert(parsed[key] == val);
    }
}

// ---------------------------------------------------------------------------
// Version comparison tests
// ---------------------------------------------------------------------------

static void test_version_compare() {
    // Equal
    assert(version_compare("1.0.0", "1.0.0") == 0);
    // Less than
    assert(version_compare("1.0.0", "1.0.1") < 0);
    assert(version_compare("1.0.0", "1.1.0") < 0);
    assert(version_compare("1.0.0", "2.0.0") < 0);
    // Greater than
    assert(version_compare("1.0.1", "1.0.0") > 0);
    assert(version_compare("2.0.0", "1.9.9") > 0);
    // Multi-digit
    assert(version_compare("1.10.0", "1.9.0") > 0);
    assert(version_compare("10.0.0", "9.99.99") > 0);
}

// ---------------------------------------------------------------------------
// Manifest parsing tests
// ---------------------------------------------------------------------------

static void test_manifest_parse() {
    std::string manifest_json = R"json({
        "format_version": 1,
        "version": "1.1.0",
        "build_date": "2026-03-06",
        "arch": "x86_64",
        "min_version": "1.0.0",
        "components": {
            "system": {
                "file": "system-1.1.0.squashfs.zst",
                "sha256": "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890",
                "size_compressed": 52428800,
                "size_uncompressed": 83886080
            }
        },
        "changelog": [
            "Added A/B update support",
            "Improved boot speed"
        ]
    })json";

    auto m = parse_manifest(manifest_json);
    assert(m.valid == true);
    assert(m.format_version == 1);
    assert(m.version == "1.1.0");
    assert(m.build_date == "2026-03-06");
    assert(m.arch == "x86_64");
    assert(m.min_version == "1.0.0");
    assert(m.system.file == "system-1.1.0.squashfs.zst");
    assert(m.system.sha256 == "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    assert(m.system.size_compressed == 52428800);
    assert(m.system.size_uncompressed == 83886080);
    assert(m.changelog.size() == 2);
    assert(m.changelog[0] == "Added A/B update support");
    assert(m.changelog[1] == "Improved boot speed");
}

static void test_manifest_parse_invalid() {
    // Invalid JSON
    auto m1 = parse_manifest("{not valid json");
    assert(m1.valid == false);

    // Valid JSON but missing required fields
    auto m2 = parse_manifest(R"json({"format_version": 0, "version": ""})json");
    assert(m2.valid == false);

    // format_version=0 -> invalid
    auto m3 = parse_manifest(R"json({"format_version": 0, "version": "1.0.0"})json");
    assert(m3.valid == false);

    // version empty -> invalid
    auto m4 = parse_manifest(R"json({"format_version": 1, "version": ""})json");
    assert(m4.valid == false);
}

// ---------------------------------------------------------------------------
// Ed25519 signature verification tests
// ---------------------------------------------------------------------------

static void test_verify_signature_invalid() {
    // Random garbage signature should fail verification against non-zero key
    unsigned char sig[64];
    for (int i = 0; i < 64; i++) sig[i] = (unsigned char)(i * 7 + 13);
    unsigned char msg[] = "hello world update bundle";
    unsigned char pk[32];
    for (int i = 0; i < 32; i++) pk[i] = (unsigned char)(i * 3 + 5);
    assert(!verify_update_signature(sig, 64, msg, 24, pk));
}

static void test_verify_signature_wrong_len() {
    // Signature must be exactly 64 bytes
    unsigned char sig[32] = {0};
    unsigned char msg[] = "hello";
    unsigned char pk[32] = {0};
    assert(!verify_update_signature(sig, 32, msg, 5, pk));
}

// ---------------------------------------------------------------------------
// SHA-256 tests
// ---------------------------------------------------------------------------

static void test_sha256_hex() {
    // SHA-256 of empty string = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    unsigned char empty[1] = {0};
    std::string hash = sha256_hex(empty, 0);
    assert(hash == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

static void test_sha256_hello() {
    // SHA-256 of "hello" = 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    const char* msg = "hello";
    std::string hash = sha256_hex((const unsigned char*)msg, 5);
    assert(hash == "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}

static void test_sha256_file_fn() {
    // Write a known file, compute sha256, verify
    const char* path = "test_sha256_tmp";
    FILE* f = fopen(path, "wb");
    const char* data = "hello world";
    fwrite(data, 1, 11, f);
    fclose(f);

    // SHA-256 of "hello world" = b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
    std::string hash = sha256_file(path);
    assert(hash == "b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9");

    // With offset — sha256 of "world" (offset 6) = 486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7
    hash = sha256_file(path, 6);
    assert(hash == "486ea46224d1bb4fb680f34f7c9ad96a8f24ec88be73ea8e5a6c65260e9cb8a7");

    std::remove(path);
}

// ---------------------------------------------------------------------------
// .update file parsing tests
// ---------------------------------------------------------------------------

static void test_parse_update_file() {
    // Build a minimal .update file in memory and write to disk
    const char* path = "test_update_tmp.update";
    FILE* f = fopen(path, "wb");
    assert(f);

    // Manifest JSON
    std::string manifest = R"json({"format_version":1,"version":"1.1.0","build_date":"2026-03-08","arch":"x86-64","min_version":"1.0.0","components":{"system":{"file":"system.squashfs","sha256":"0000000000000000000000000000000000000000000000000000000000000000","size_compressed":4,"size_uncompressed":4}},"changelog":[]})json";
    uint32_t manifest_size = (uint32_t)manifest.size();

    // Header: magic + format_version + manifest_size
    fwrite("LMUP", 1, 4, f);
    unsigned char ver[4] = {1, 0, 0, 0};  // uint32 LE = 1
    fwrite(ver, 1, 4, f);
    unsigned char ms[4] = {
        (unsigned char)(manifest_size & 0xFF),
        (unsigned char)((manifest_size >> 8) & 0xFF),
        (unsigned char)((manifest_size >> 16) & 0xFF),
        (unsigned char)((manifest_size >> 24) & 0xFF)
    };
    fwrite(ms, 1, 4, f);

    // Manifest
    fwrite(manifest.data(), 1, manifest.size(), f);

    // Fake signature (64 bytes of zeros)
    unsigned char sig[64] = {0};
    fwrite(sig, 1, 64, f);

    // Fake payload (4 bytes)
    fwrite("TEST", 1, 4, f);
    fclose(f);

    // Parse it
    auto info = parse_update_file(path);
    assert(info.valid);
    assert(info.manifest.version == "1.1.0");
    assert(info.manifest.format_version == 1);
    assert(info.payload_offset == 12 + manifest_size + 64);
    assert(info.payload_size == 4);

    std::remove(path);
}

static void test_parse_update_file_bad_magic() {
    const char* path = "test_update_badmagic.update";
    FILE* f = fopen(path, "wb");
    fwrite("NOPE", 1, 4, f);
    fwrite("12345678901234567890", 1, 20, f);
    fclose(f);

    auto info = parse_update_file(path);
    assert(!info.valid);
    assert(info.error.find("invalid magic") != std::string::npos);

    std::remove(path);
}

// ---------------------------------------------------------------------------
// File I/O roundtrip test (uses temp files)
// ---------------------------------------------------------------------------

static void test_grubenv_file_roundtrip() {
    std::string path = "test_grubenv_tmp";
    std::map<std::string, std::string> vars;
    vars["llamaste_slot"] = "A";
    vars["boot_success"] = "1";
    vars["saved_entry"] = "llamaste-a";

    // Write
    bool ok = grubenv_write(path, vars);
    assert(ok);

    // Read back
    auto read_vars = grubenv_read(path);
    assert(read_vars.size() == vars.size());
    for (const auto& [key, val] : vars) {
        assert(read_vars[key] == val);
    }

    // Verify file is exactly 1024 bytes
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    assert(f.is_open());
    assert(f.tellg() == 1024);
    f.close();

    // Clean up
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    printf("=== Updater Tests ===\n");

    TEST(test_parse_slot_A);
    TEST(test_parse_slot_B);
    TEST(test_parse_slot_missing);
    TEST(test_inactive_slot_A);
    TEST(test_inactive_slot_B);
    TEST(test_inactive_partition_A);
    TEST(test_inactive_partition_B);
    TEST(test_grubenv_parse);
    TEST(test_grubenv_serialize);
    TEST(test_grubenv_roundtrip);
    TEST(test_version_compare);
    TEST(test_manifest_parse);
    TEST(test_manifest_parse_invalid);
    TEST(test_verify_signature_invalid);
    TEST(test_verify_signature_wrong_len);
    TEST(test_sha256_hex);
    TEST(test_sha256_hello);
    TEST(test_sha256_file_fn);
    TEST(test_parse_update_file);
    TEST(test_parse_update_file_bad_magic);
    TEST(test_grubenv_file_roundtrip);

    printf("\n%d/%d updater tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
