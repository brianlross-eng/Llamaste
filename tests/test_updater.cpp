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
    TEST(test_grubenv_file_roundtrip);

    printf("\n%d/%d updater tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
