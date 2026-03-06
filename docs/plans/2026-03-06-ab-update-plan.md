# Phase 4: A/B Update System — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable atomic system updates with automatic rollback via A/B partition switching, online update checks, and a web UI update card.

**Architecture:** Custom A/B updater compiled into the llamaste binary. GRUB reads `active_slot` from grubenv to select SYS-A or SYS-B. Ed25519 signature verification via vendored TweetNaCl. Update bundles are tar archives with manifest + XZ-compressed squashfs.

**Tech Stack:** C++17, TweetNaCl (Ed25519), liblzma (XZ), libcurl (downloads), GRUB2 grubenv, tar archive parsing

**Design doc:** `docs/plans/2026-03-06-ab-update-design.md`
**Research:** `research/20-update-mechanisms.md`

---

## Task 1: Version Header + Slot Detection

**Files:**
- Create: `src/llamaste/version.h`
- Create: `src/llamaste/updater.h`
- Create: `src/llamaste/updater.cpp`
- Create: `tests/test_updater.cpp`
- Modify: `scripts/host-test.sh` — add Suite 12

This task creates the version constant, slot detection from `/proc/cmdline`, and grubenv reader/writer.

**Step 1: Create `version.h`**

```cpp
// src/llamaste/version.h
#pragma once
constexpr const char* LLAMASTE_VERSION = "1.0.0";
```

**Step 2: Write failing tests in `tests/test_updater.cpp`**

Tests for:
- `parse_active_slot()` — extracts `llamaste.slot=X` from cmdline string
- `parse_active_slot()` — returns "A" when no slot param present
- `inactive_slot()` — A→B, B→A
- `inactive_partition_num()` — A→4, B→3
- `grubenv_parse()` — parses key=value pairs from grubenv content
- `grubenv_serialize()` — produces 1024-byte block with header + padding
- `grubenv_roundtrip()` — parse then serialize preserves values
- `version_compare()` — "1.0.0" < "1.1.0" < "2.0.0", equal returns 0

```cpp
// tests/test_updater.cpp
#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <map>

// Include implementation
#include "updater.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
} while(0)

void test_parse_slot_A() {
    assert(parse_active_slot("root=/dev/sda3 llamaste.slot=A ip=dhcp") == "A");
}
void test_parse_slot_B() {
    assert(parse_active_slot("root=/dev/sda4 llamaste.slot=B ip=dhcp") == "B");
}
void test_parse_slot_missing() {
    assert(parse_active_slot("root=/dev/sda3 ip=dhcp") == "A");
}
void test_inactive_slot_A() {
    assert(inactive_slot("A") == "B");
}
void test_inactive_slot_B() {
    assert(inactive_slot("B") == "A");
}
void test_inactive_partition_A() {
    assert(inactive_partition_num("A") == 4);
}
void test_inactive_partition_B() {
    assert(inactive_partition_num("B") == 3);
}
void test_grubenv_parse() {
    // Build a 1024-byte grubenv
    std::string env = "# GRUB Environment Block\n";
    env += "active_slot=A\n";
    env += "boot_success=1\n";
    env.resize(1024, '#');
    auto vars = grubenv_parse(env);
    assert(vars["active_slot"] == "A");
    assert(vars["boot_success"] == "1");
    assert(vars.size() == 2);
}
void test_grubenv_serialize() {
    std::map<std::string, std::string> vars;
    vars["active_slot"] = "B";
    vars["boot_counter"] = "3";
    std::string out = grubenv_serialize(vars);
    assert(out.size() == 1024);
    assert(out.substr(0, 26) == "# GRUB Environment Block\n");
    assert(out.find("active_slot=B\n") != std::string::npos);
    assert(out.find("boot_counter=3\n") != std::string::npos);
    // Rest should be '#' padding
    assert(out.back() == '#');
}
void test_grubenv_roundtrip() {
    std::map<std::string, std::string> vars;
    vars["active_slot"] = "A";
    vars["boot_success"] = "1";
    std::string serialized = grubenv_serialize(vars);
    auto parsed = grubenv_parse(serialized);
    assert(parsed["active_slot"] == "A");
    assert(parsed["boot_success"] == "1");
    assert(parsed.size() == 2);
}
void test_version_compare() {
    assert(version_compare("1.0.0", "1.0.0") == 0);
    assert(version_compare("1.0.0", "1.1.0") < 0);
    assert(version_compare("1.1.0", "1.0.0") > 0);
    assert(version_compare("2.0.0", "1.9.9") > 0);
    assert(version_compare("1.0.0", "1.0.1") < 0);
    assert(version_compare("0.9.0", "1.0.0") < 0);
}

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
    printf("=== %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_run - tests_passed;
}
```

**Step 3: Run tests — verify they fail (compile error, updater.h doesn't exist)**

```bash
wsl -d Ubuntu -- bash -c "cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -o tests/build/test_updater tests/test_updater.cpp src/llamaste/updater.cpp 2>&1"
```
Expected: FAIL — `updater.h: No such file or directory`

**Step 4: Create `updater.h`**

```cpp
// src/llamaste/updater.h
#pragma once
#include <string>
#include <map>

#include "version.h"

// --- Slot detection ---

// Parse active_slot from kernel cmdline string (e.g., "llamaste.slot=A")
// Returns "A" if not found (default slot)
std::string parse_active_slot(const std::string& cmdline);

// Get the inactive slot given the active one
std::string inactive_slot(const std::string& active);

// Get the partition number for the inactive slot (A=3, B=4)
int inactive_partition_num(const std::string& active);

// Read current slot from /proc/cmdline (Linux only)
std::string detect_current_slot();

// --- grubenv ---

// Parse grubenv file content (1024-byte block) into key-value map
std::map<std::string, std::string> grubenv_parse(const std::string& content);

// Serialize key-value map into grubenv format (1024-byte block)
std::string grubenv_serialize(const std::map<std::string, std::string>& vars);

// Read grubenv from file path
std::map<std::string, std::string> grubenv_read(const std::string& path);

// Write grubenv atomically (write .tmp, fsync, rename)
bool grubenv_write(const std::string& path, const std::map<std::string, std::string>& vars);

// --- Version comparison ---

// Compare two semver strings. Returns <0, 0, or >0 like strcmp.
int version_compare(const std::string& a, const std::string& b);

// --- Manifest parsing ---

struct UpdateManifest {
    int format_version = 0;
    std::string version;
    std::string build_date;
    std::string arch;
    std::string min_version;

    // system component
    std::string system_file;
    std::string system_sha256;
    size_t system_size_compressed = 0;
    size_t system_size_uncompressed = 0;

    std::vector<std::string> changelog;

    bool valid = false;
};

// Parse manifest JSON string into UpdateManifest struct
UpdateManifest parse_manifest(const std::string& json_str);
```

**Step 5: Create `updater.cpp` with implementations**

```cpp
// src/llamaste/updater.cpp
#include "updater.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <algorithm>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "json.hpp"
using json = nlohmann::json;

// --- Slot detection ---

std::string parse_active_slot(const std::string& cmdline) {
    const std::string key = "llamaste.slot=";
    auto pos = cmdline.find(key);
    if (pos == std::string::npos) return "A";  // default
    pos += key.size();
    if (pos < cmdline.size()) {
        char c = cmdline[pos];
        if (c == 'A' || c == 'a') return "A";
        if (c == 'B' || c == 'b') return "B";
    }
    return "A";
}

std::string inactive_slot(const std::string& active) {
    return (active == "A") ? "B" : "A";
}

int inactive_partition_num(const std::string& active) {
    // SYS-A = partition 3, SYS-B = partition 4
    return (active == "A") ? 4 : 3;
}

std::string detect_current_slot() {
#ifndef _WIN32
    std::ifstream f("/proc/cmdline");
    if (!f.is_open()) return "A";
    std::string cmdline;
    std::getline(f, cmdline);
    return parse_active_slot(cmdline);
#else
    return "A";
#endif
}

// --- grubenv ---

std::map<std::string, std::string> grubenv_parse(const std::string& content) {
    std::map<std::string, std::string> vars;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // Skip header and padding
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        vars[key] = val;
    }
    return vars;
}

std::string grubenv_serialize(const std::map<std::string, std::string>& vars) {
    std::string out = "# GRUB Environment Block\n";
    for (const auto& kv : vars) {
        if (kv.first.empty()) continue;
        out += kv.first + "=" + kv.second + "\n";
    }
    // Pad to 1024 bytes with '#'
    if (out.size() < 1024) {
        out.resize(1024, '#');
    }
    return out;
}

std::map<std::string, std::string> grubenv_read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::string content(1024, '\0');
    f.read(&content[0], 1024);
    auto bytes_read = f.gcount();
    content.resize(bytes_read);
    return grubenv_parse(content);
}

bool grubenv_write(const std::string& path, const std::map<std::string, std::string>& vars) {
    std::string data = grubenv_serialize(vars);
    std::string tmp_path = path + ".tmp";

    // Write to temp file
    FILE* f = fopen(tmp_path.c_str(), "wb");
    if (!f) return false;
    size_t written = fwrite(data.data(), 1, data.size(), f);
    fflush(f);
#ifndef _WIN32
    fsync(fileno(f));
#endif
    fclose(f);
    if (written != data.size()) return false;

    // Atomic rename
    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        remove(tmp_path.c_str());
        return false;
    }
    return true;
}

// --- Version comparison ---

static void parse_semver(const std::string& ver, int& major, int& minor, int& patch) {
    major = minor = patch = 0;
    if (sscanf(ver.c_str(), "%d.%d.%d", &major, &minor, &patch) < 1) {
        major = minor = patch = 0;
    }
}

int version_compare(const std::string& a, const std::string& b) {
    int a_major, a_minor, a_patch;
    int b_major, b_minor, b_patch;
    parse_semver(a, a_major, a_minor, a_patch);
    parse_semver(b, b_major, b_minor, b_patch);
    if (a_major != b_major) return a_major - b_major;
    if (a_minor != b_minor) return a_minor - b_minor;
    return a_patch - b_patch;
}

// --- Manifest parsing ---

UpdateManifest parse_manifest(const std::string& json_str) {
    UpdateManifest m;
    try {
        auto j = json::parse(json_str);
        m.format_version = j.value("format_version", 0);
        m.version = j.value("version", "");
        m.build_date = j.value("build_date", "");
        m.arch = j.value("arch", "");
        m.min_version = j.value("min_version", "");

        if (j.contains("components") && j["components"].contains("system")) {
            auto& sys = j["components"]["system"];
            m.system_file = sys.value("file", "");
            m.system_sha256 = sys.value("sha256", "");
            m.system_size_compressed = sys.value("size_compressed", (size_t)0);
            m.system_size_uncompressed = sys.value("size_uncompressed", (size_t)0);
        }

        if (j.contains("changelog") && j["changelog"].is_array()) {
            for (const auto& entry : j["changelog"]) {
                m.changelog.push_back(entry.get<std::string>());
            }
        }

        m.valid = !m.version.empty() && m.format_version > 0;
    } catch (...) {
        m.valid = false;
    }
    return m;
}
```

**Step 6: Add Suite 12 to `scripts/host-test.sh`**

Insert before the Results section. Update all suite counters from `/11]` to `/12]`.

```bash
# Suite 12: Updater
echo -e "${BOLD}--- [12/12] Updater ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_updater" \
    "${TESTS}/test_updater.cpp" \
    "${SRC}/updater.cpp" 2>&1; then
    if "${BUILD_DIR}/test_updater"; then
        suite_pass "Updater"
    else
        suite_fail "Updater (runtime)"
    fi
else
    suite_fail "Updater (compile)"
fi
echo ""
```

**Step 7: Run tests — verify they pass**

```bash
wsl -d Ubuntu -- bash -c "cd /mnt/d/Llamaste && bash scripts/host-test.sh 2>&1 | tail -20"
```
Expected: Suite 12 PASS, all 12 suites pass

**Step 8: Commit**

```bash
git add src/llamaste/version.h src/llamaste/updater.h src/llamaste/updater.cpp tests/test_updater.cpp scripts/host-test.sh
git commit -m "feat: updater foundation — version header, slot detection, grubenv reader/writer"
```

---

## Task 2: Ed25519 Signature Verification (TweetNaCl)

**Files:**
- Create: `src/llamaste/tweetnacl.h`
- Create: `src/llamaste/tweetnacl.c`
- Modify: `tests/test_updater.cpp` — add Ed25519 verify tests
- Modify: `src/llamaste/updater.h` — add verify_signature()
- Modify: `src/llamaste/updater.cpp` — add verify_signature()
- Modify: `src/llamaste/CMakeLists.txt` — add tweetnacl.c

TweetNaCl is a public-domain, auditable, single-file implementation of NaCl cryptography. We only need `crypto_sign_ed25519_verify_detached()` but TweetNaCl provides it as part of `crypto_sign_open`. We vendor the entire file (~700 LOC C) and wrap it.

**Step 1: Vendor TweetNaCl**

Download `tweetnacl.h` and `tweetnacl.c` from https://tweetnacl.cr.yp.to/. These are public domain.

Alternatively, write a minimal Ed25519 verify wrapper using TweetNaCl's API:

```c
// src/llamaste/tweetnacl.h — Header for vendored TweetNaCl
// Public domain. Source: https://tweetnacl.cr.yp.to/
#ifndef TWEETNACL_H
#define TWEETNACL_H

#ifdef __cplusplus
extern "C" {
#endif

// Ed25519 signature: verify a detached signature
// Returns 0 on success, -1 on failure
// sig: 64-byte signature
// msg: message bytes
// msg_len: message length
// pk: 32-byte public key
int crypto_sign_ed25519_verify_detached(
    const unsigned char *sig,
    const unsigned char *msg,
    unsigned long long msg_len,
    const unsigned char *pk
);

// SHA-512 hash (needed by Ed25519 internally)
int crypto_hash_sha512(unsigned char *out, const unsigned char *msg, unsigned long long n);

#ifdef __cplusplus
}
#endif

#endif
```

The `.c` file is the full TweetNaCl implementation. Since it's ~700 LOC, download it directly rather than typing it out. The subagent should:
1. Download `tweetnacl.c` from the official source OR
2. Use a known working copy from the project's research

**Step 2: Add tests for Ed25519 verify**

Add to `tests/test_updater.cpp`:

```cpp
void test_verify_signature_invalid() {
    // An invalid signature should fail
    unsigned char sig[64] = {0};
    unsigned char msg[] = "hello";
    unsigned char pk[32] = {0};
    assert(!verify_update_signature(sig, 64, msg, 5, pk));
}

void test_manifest_parse() {
    std::string json = R"json({
        "format_version": 1,
        "version": "1.1.0",
        "build_date": "2026-03-15T10:30:00Z",
        "arch": "x86-64",
        "min_version": "1.0.0",
        "components": {
            "system": {
                "file": "system.squashfs.xz",
                "sha256": "abc123",
                "size_compressed": 45000000,
                "size_uncompressed": 162000000
            }
        },
        "changelog": ["Added mesh clustering", "Fixed TTS bug"]
    })json";
    auto m = parse_manifest(json);
    assert(m.valid);
    assert(m.version == "1.1.0");
    assert(m.arch == "x86-64");
    assert(m.min_version == "1.0.0");
    assert(m.system_file == "system.squashfs.xz");
    assert(m.system_sha256 == "abc123");
    assert(m.changelog.size() == 2);
    assert(m.changelog[0] == "Added mesh clustering");
}

void test_manifest_parse_invalid() {
    auto m = parse_manifest("not json");
    assert(!m.valid);
}
```

**Step 3: Add `verify_update_signature()` to updater.h/cpp**

```cpp
// In updater.h:
// Verify Ed25519 signature. Returns true if valid.
bool verify_update_signature(
    const unsigned char* sig, size_t sig_len,
    const unsigned char* msg, size_t msg_len,
    const unsigned char* pubkey
);
```

```cpp
// In updater.cpp:
#include "tweetnacl.h"

bool verify_update_signature(
    const unsigned char* sig, size_t sig_len,
    const unsigned char* msg, size_t msg_len,
    const unsigned char* pubkey
) {
    if (sig_len != 64) return false;
    return crypto_sign_ed25519_verify_detached(sig, msg, msg_len, pubkey) == 0;
}
```

**Step 4: Update CMakeLists.txt**

Add `updater.cpp` and `tweetnacl.c` to `LLAMASTE_SOURCES`.

**Step 5: Update host-test.sh Suite 12**

Add `tweetnacl.c` to the compile line for Suite 12.

**Step 6: Run tests, verify pass, commit**

```bash
git commit -m "feat: Ed25519 signature verification via vendored TweetNaCl"
```

---

## Task 3: GRUB Config for A/B Boot

**Files:**
- Modify: `br2-external/board/llamaste/grub.cfg` — add A/B slot switching
- Modify: `br2-external/board/llamaste/post_image.sh` — create initial grubenv on ESP

**Step 1: Update grub.cfg**

Replace current hardcoded `root=/dev/sda3` with dynamic slot selection:

```grub
set timeout=3
set default=0

# Load saved environment variables from grubenv
load_env

# Default to slot A if active_slot not set
if [ -z "${active_slot}" ]; then
    set active_slot=A
fi

# Boot counter rollback logic:
# After an update, boot_counter=3 and boot_success=0.
# Each failed boot decrements counter. At 0, switch to other slot.
if [ "${boot_success}" = "0" ]; then
    if [ -n "${boot_counter}" ]; then
        if [ "${boot_counter}" = "0" ]; then
            # Exhausted attempts — switch slot
            if [ "${active_slot}" = "A" ]; then
                set active_slot=B
            else
                set active_slot=A
            fi
            save_env active_slot
            # Clear counter to stop rollback loop
            set boot_counter=
            save_env boot_counter
        else
            # Decrement (GRUB has no arithmetic)
            if [ "${boot_counter}" = "3" ]; then set boot_counter=2; fi
            if [ "${boot_counter}" = "2" ]; then set boot_counter=1; fi
            if [ "${boot_counter}" = "1" ]; then set boot_counter=0; fi
            save_env boot_counter
        fi
    fi
fi

# Select root device based on active slot
if [ "${active_slot}" = "B" ]; then
    set rootdev=/dev/sda4
else
    set rootdev=/dev/sda3
fi

menuentry "Llamaste Server" {
    linux /bzImage root=${rootdev} rootfstype=squashfs ro quiet \
        console=tty0 console=ttyS0,115200 \
        init=/opt/llamaste/llamaste \
        llamaste.mode=server llamaste.slot=${active_slot} \
        ip=dhcp
}

menuentry "Llamaste Desktop" {
    linux /bzImage root=${rootdev} rootfstype=squashfs ro quiet \
        console=tty0 console=ttyS0,115200 \
        init=/opt/llamaste/llamaste \
        llamaste.mode=desktop llamaste.slot=${active_slot} \
        ip=dhcp
}
```

**Step 2: Create initial grubenv in post_image.sh**

Find the section in `post_image.sh` that creates the ESP and add grubenv creation:

```bash
# Create initial grubenv (1024-byte file with slot A as default)
GRUBENV="${BINARIES_DIR}/efi-part/EFI/BOOT/grubenv"
printf '# GRUB Environment Block\nactive_slot=A\nboot_success=1\n' > "${GRUBENV}"
# Pad to exactly 1024 bytes with '#'
truncate -s 1024 "${GRUBENV}"
dd if=/dev/zero bs=1 count=$((1024 - $(stat -c%s "${GRUBENV}"))) 2>/dev/null | tr '\0' '#' >> "${GRUBENV}" 2>/dev/null || true
truncate -s 1024 "${GRUBENV}"
# Also place in /boot/grub/ for BIOS boot path
cp "${GRUBENV}" "${BINARIES_DIR}/efi-part/boot/grub/grubenv"
```

Note: The actual implementation may need to adjust based on how `post_image.sh` currently structures the ESP. Read the file first.

**Step 3: Commit**

```bash
git commit -m "feat: GRUB A/B boot switching with boot counter rollback"
```

---

## Task 4: Boot Success Health Check

**Files:**
- Modify: `src/llamaste/child_main.cpp` — add boot success marking after server starts
- Modify: `src/llamaste/updater.h` — add `mark_boot_success()` declaration
- Modify: `src/llamaste/updater.cpp` — add `mark_boot_success()` + ESP mount helpers

**Step 1: Add ESP mount + boot success functions to updater**

```cpp
// In updater.h:
// Find the ESP mount path (checks /boot/efi, /boot, /mnt/esp)
std::string find_esp_mount();

// Mount ESP read-write, returns true on success
bool mount_esp_rw(const std::string& mount_point);

// Remount ESP read-only
bool remount_esp_ro(const std::string& mount_point);

// Find grubenv path on ESP
std::string find_grubenv_path();

// Mark current boot as successful (sets boot_success=1, clears boot_counter)
bool mark_boot_success();
```

```cpp
// In updater.cpp:

std::string find_esp_mount() {
    // The ESP might be mounted at /boot/efi, /boot, or not mounted at all
    // In Llamaste, init.cpp doesn't mount the ESP by default, so we may need to mount it
    static const char* candidates[] = {"/boot/efi", "/boot", "/mnt/esp"};
    for (const auto& p : candidates) {
        struct stat st;
        if (stat(p, &st) == 0) return p;
    }
    return "";
}

std::string find_grubenv_path() {
    // Try common locations on ESP
    static const char* candidates[] = {
        "/boot/efi/EFI/BOOT/grubenv",
        "/boot/efi/boot/grub/grubenv",
        "/boot/grub/grubenv",
        "/mnt/esp/EFI/BOOT/grubenv",
        "/mnt/esp/boot/grub/grubenv"
    };
    for (const auto& p : candidates) {
        if (access(p, F_OK) == 0) return p;
    }
    return "";
}

bool mark_boot_success() {
    // Mount ESP if needed, find grubenv, set boot_success=1
    // This is a no-op if boot_counter is not set (normal boot, not post-update)

#ifdef _WIN32
    return true;  // No-op on Windows
#else
    // Try to mount ESP at /mnt/esp if not already mounted
    std::string esp_mount = find_esp_mount();
    bool we_mounted = false;
    if (esp_mount.empty()) {
        // Try to mount partition 2
        mkdir("/mnt/esp", 0755);
        // Try common ESP devices
        static const char* esp_devs[] = {
            "/dev/sda2", "/dev/vda2", "/dev/nvme0n1p2"
        };
        for (const auto& dev : esp_devs) {
            if (access(dev, F_OK) != 0) continue;
            if (mount(dev, "/mnt/esp", "vfat", 0, nullptr) == 0) {
                esp_mount = "/mnt/esp";
                we_mounted = true;
                break;
            }
        }
        if (esp_mount.empty()) {
            fprintf(stderr, "[update] Could not mount ESP\n");
            return false;
        }
    }

    std::string grubenv_path = find_grubenv_path();
    if (grubenv_path.empty()) {
        fprintf(stderr, "[update] grubenv not found on ESP\n");
        if (we_mounted) umount("/mnt/esp");
        return false;
    }

    auto vars = grubenv_read(grubenv_path);
    // Only act if boot_counter exists (we're in a trial boot)
    if (vars.find("boot_counter") == vars.end() && vars["boot_success"] == "1") {
        // Normal boot, nothing to do
        if (we_mounted) umount("/mnt/esp");
        return true;
    }

    fprintf(stderr, "[update] Marking boot success for slot %s\n",
            vars["active_slot"].c_str());

    vars["boot_success"] = "1";
    vars.erase("boot_counter");

    bool ok = grubenv_write(grubenv_path, vars);
    if (we_mounted) umount("/mnt/esp");
    return ok;
#endif
}
```

**Step 2: Wire into child_main.cpp**

After `svr.listen()` starts (actually, just before — right after all routes are registered and before `svr.listen()` which blocks):

```cpp
// In child_main.cpp, after all route registration, before svr.listen():
#include "updater.h"

// Mark boot as successful (no-op if not a post-update trial boot)
// This runs after all critical systems are initialized:
// - Tools registered
// - HTTP routes set up
// - Model loaded (if available)
// - mDNS running
std::thread boot_check_thread([&]() {
    // Wait for HTTP server to actually be listening
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (mark_boot_success()) {
        fprintf(stderr, "[update] Boot health check passed\n");
    }
});
boot_check_thread.detach();
```

**Step 3: Add mount.h include guard to updater.cpp**

```cpp
#ifndef _WIN32
#include <sys/mount.h>
#include <sys/stat.h>
#endif
```

**Step 4: Run tests, verify pass, commit**

```bash
git commit -m "feat: boot success health check — marks grubenv after healthy startup"
```

---

## Task 5: Update Tools (4 tools)

**Files:**
- Create: `src/llamaste/tools_update.cpp`
- Modify: `src/llamaste/tools.h` — add `register_update_tools()` declaration
- Modify: `src/llamaste/child_main.cpp` — call `register_update_tools()`
- Modify: `src/llamaste/CMakeLists.txt` — add `tools_update.cpp`

**Step 1: Create `tools_update.cpp`**

4 tools: `update.check`, `update.install`, `update.status`, `update.rollback`

```cpp
// src/llamaste/tools_update.cpp
#include "tools.h"
#include "updater.h"
#include "json.hpp"
#include <string>

using json = nlohmann::json;

// Shared update state
static std::string g_current_slot = "A";
static std::string g_update_state = "idle";  // idle, checking, downloading, installing, done, error
static std::string g_update_error;
static int g_update_progress = 0;

void register_update_tools(ToolRegistry& reg) {
    g_current_slot = detect_current_slot();

    // --- update.status ---
    reg.register_tool({
        .name = "update.status",
        .description = "Get current system version, active boot slot, and update status",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = [](const std::string&) -> std::string {
            json result;
            result["version"] = LLAMASTE_VERSION;
            result["active_slot"] = g_current_slot;
            result["inactive_slot"] = inactive_slot(g_current_slot);
            result["update_state"] = g_update_state;
            result["update_progress"] = g_update_progress;
            if (!g_update_error.empty()) {
                result["error"] = g_update_error;
            }
            return result.dump();
        }
    });

    // --- update.check ---
    reg.register_tool({
        .name = "update.check",
        .description = "Check for available system updates online (GitHub Releases)",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = [](const std::string&) -> std::string {
            json result;
            result["current_version"] = LLAMASTE_VERSION;
            // TODO: Phase 4b — actually check GitHub API
            result["available"] = false;
            result["message"] = "Online update checking not yet implemented. Use update.install with a local .update file path.";
            return result.dump();
        }
    });

    // --- update.install ---
    reg.register_tool({
        .name = "update.install",
        .description = "Install a system update from a local .update file path",
        .parameters = R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the .update bundle file"}},"required":["path"]})json",
        .handler = [](const std::string& args_json) -> std::string {
            json result;
            try {
                auto args = json::parse(args_json);
                std::string path = args.value("path", "");
                if (path.empty()) {
                    result["error"] = "path is required";
                    return result.dump();
                }
                // TODO: implement full install flow
                // For now, validate the path exists
                if (access(path.c_str(), R_OK) != 0) {
                    result["error"] = "File not found: " + path;
                    return result.dump();
                }
                result["error"] = "Update installation not yet implemented";
            } catch (const std::exception& e) {
                result["error"] = std::string("JSON parse error: ") + e.what();
            }
            return result.dump();
        }
    });

    // --- update.rollback ---
    reg.register_tool({
        .name = "update.rollback",
        .description = "Switch to the other boot slot (rollback to previous version). Requires reboot.",
        .parameters = R"json({"type":"object","properties":{}})json",
        .handler = [](const std::string&) -> std::string {
            json result;
#ifndef _WIN32
            std::string new_slot = inactive_slot(g_current_slot);
            std::string grubenv_path = find_grubenv_path();

            if (grubenv_path.empty()) {
                result["error"] = "grubenv not found — cannot switch slot";
                return result.dump();
            }

            auto vars = grubenv_read(grubenv_path);
            vars["active_slot"] = new_slot;
            vars["boot_success"] = "0";
            vars["boot_counter"] = "3";

            if (grubenv_write(grubenv_path, vars)) {
                result["success"] = true;
                result["message"] = "Switched to slot " + new_slot + ". Reboot to activate.";
                result["new_slot"] = new_slot;
            } else {
                result["error"] = "Failed to write grubenv";
            }
#else
            result["error"] = "Rollback not available on Windows";
#endif
            return result.dump();
        }
    });
}
```

**Step 2: Update tools.h**

Add declaration:
```cpp
void register_update_tools(ToolRegistry& reg);
```

**Step 3: Wire into child_main.cpp**

After existing tool registration (line ~944):
```cpp
register_update_tools(g_tools);
```

**Step 4: Update CMakeLists.txt**

Add `tools_update.cpp` and `updater.cpp` to `LLAMASTE_SOURCES`.

**Step 5: Update host-test.sh**

Add `tools_update.cpp` and `updater.cpp` to the compile lines for Suite 5 (HTTP) and Suite 8 (Inference) which compile `child_main.cpp`.

**Step 6: Run all tests, verify pass, commit**

```bash
git commit -m "feat: 4 update tools — status, check, install, rollback"
```

---

## Task 6: Web UI Update Card

**Files:**
- Modify: `src/llamaste/web/index.html` — add update card to system panel
- Modify: `src/llamaste/web/system.js` — add update card JS logic

**Step 1: Add update card HTML to index.html**

Insert the update card in the `#panel-system` section, between the About card and the MCP card:

```html
<!-- System Update -->
<div class="system-card" id="update-card">
    <h3>System Update</h3>
    <div class="dash-row">
        <span class="label">Version</span>
        <span id="update-version" class="value">—</span>
    </div>
    <div class="dash-row">
        <span class="label">Active Slot</span>
        <span id="update-slot" class="value">—</span>
    </div>
    <div class="dash-row" id="update-available-row" style="display:none">
        <span class="label">Available</span>
        <span id="update-available" class="value" style="color:#00e5a0"></span>
    </div>
    <div id="update-progress-row" style="display:none">
        <div class="progress-bar-outer" style="height:6px;background:#333;border-radius:3px;margin:8px 0">
            <div id="update-progress-bar" style="height:100%;width:0%;background:#00e5a0;border-radius:3px;transition:width 0.3s"></div>
        </div>
        <p id="update-progress-text" class="text-muted" style="font-size:0.8em"></p>
    </div>
    <div class="system-actions" style="margin-top:8px">
        <button id="update-check-btn" class="btn-sm">Check for Updates</button>
        <button id="update-rollback-btn" class="btn-sm" style="display:none">Rollback</button>
    </div>
</div>
```

**Step 2: Add JavaScript to system.js**

Add `updateUpdateCard()` function and wire buttons:

```javascript
// --- Update card ---
function updateUpdateCard() {
    fetch('/llamaste/update/status', { credentials: 'include' })
        .then(function (r) { return r.json(); })
        .then(function (data) {
            var verEl = document.getElementById('update-version');
            var slotEl = document.getElementById('update-slot');
            var availRow = document.getElementById('update-available-row');
            var availEl = document.getElementById('update-available');
            var rollbackBtn = document.getElementById('update-rollback-btn');

            if (!verEl) return;

            verEl.textContent = data.version || '—';
            slotEl.textContent = data.active_slot || '—';

            // Show rollback button if inactive slot has a version
            if (data.inactive_version) {
                rollbackBtn.style.display = '';
                rollbackBtn.textContent = 'Rollback to ' + data.inactive_version;
            }

            // Show update progress if installing
            var progressRow = document.getElementById('update-progress-row');
            var progressBar = document.getElementById('update-progress-bar');
            var progressText = document.getElementById('update-progress-text');
            if (data.update_state && data.update_state !== 'idle') {
                progressRow.style.display = '';
                progressBar.style.width = (data.update_progress || 0) + '%';
                progressText.textContent = data.update_state + ' (' + (data.update_progress || 0) + '%)';
            } else {
                progressRow.style.display = 'none';
            }
        })
        .catch(function () {});
}
```

Wire the check and rollback buttons, and call `updateUpdateCard()` from `systemRefresh()`.

**Step 3: Commit**

```bash
git commit -m "feat: web UI update card — version, slot, rollback button"
```

---

## Task 7: Update HTTP Endpoints

**Files:**
- Modify: `src/llamaste/child_main.cpp` — add `/llamaste/update/*` routes

**Step 1: Add update routes in child_main.cpp**

After the cluster status endpoint registration:

```cpp
// --- Update endpoints ---
svr.Get("/llamaste/update/status", require_auth(
    [](const httplib::Request&, httplib::Response& res) {
        json status;
        status["version"] = LLAMASTE_VERSION;
        status["active_slot"] = detect_current_slot();
        status["inactive_slot"] = inactive_slot(detect_current_slot());
        // Check if inactive slot has metadata
        std::string inactive = inactive_slot(detect_current_slot());
        std::string meta_path = "/data/llamaste/slots/" + inactive + ".json";
        std::ifstream mf(meta_path);
        if (mf.is_open()) {
            try {
                json meta = json::parse(mf);
                status["inactive_version"] = meta.value("version", "");
            } catch (...) {}
        }
        res.set_content(status.dump(), "application/json");
    }
));

svr.Post("/llamaste/update/check", require_auth(
    [](const httplib::Request&, httplib::Response& res) {
        json result;
        result["current_version"] = LLAMASTE_VERSION;
        result["available"] = false;
        result["message"] = "Online update checking coming soon";
        res.set_content(result.dump(), "application/json");
    }
));

svr.Post("/llamaste/update/rollback", require_auth(
    [](const httplib::Request&, httplib::Response& res) {
        std::string grubenv_path = find_grubenv_path();
        if (grubenv_path.empty()) {
            res.status = 500;
            res.set_content(R"json({"error":"grubenv not found"})json", "application/json");
            return;
        }
        std::string current = detect_current_slot();
        std::string new_slot = inactive_slot(current);
        auto vars = grubenv_read(grubenv_path);
        vars["active_slot"] = new_slot;
        vars["boot_success"] = "0";
        vars["boot_counter"] = "3";
        if (grubenv_write(grubenv_path, vars)) {
            json ok;
            ok["success"] = true;
            ok["new_slot"] = new_slot;
            ok["message"] = "Switched to slot " + new_slot + ". Reboot to activate.";
            res.set_content(ok.dump(), "application/json");
        } else {
            res.status = 500;
            res.set_content(R"json({"error":"Failed to write grubenv"})json", "application/json");
        }
    }
));
```

**Step 2: Add version/slot info to `/llamaste/system` response**

In `gather_system_info()`, add:
```cpp
info["version"] = LLAMASTE_VERSION;
info["active_slot"] = detect_current_slot();
```

**Step 3: Run all tests, verify pass, commit**

```bash
git commit -m "feat: update HTTP endpoints — status, check, rollback"
```

---

## Task 8: Build, Deploy, Verify on VDI

**Files:**
- Modify: `br2-external/board/llamaste/post_image.sh` — create grubenv on ESP
- Build full image with new code
- Deploy to VDI and verify

**Step 1: Read and update post_image.sh**

Read the file first to find where the ESP partition is set up. Add grubenv creation there.

**Step 2: Build llamaste binary**

```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && export FORCE_UNSAFE_CONFIGURE=1 && cd /root/llamaste-build/output && make llamaste-dirclean && make llamaste && make"
```

**Step 3: Deploy to VDI**

Use the virtualbox-vdi-partition-update skill to update partition 3 (SYS-A) with new squashfs.

**Step 4: Verify**

Start VM, check:
- `GET /health` — should include version
- `GET /llamaste/system` — should show `version` and `active_slot`
- `GET /llamaste/update/status` — should return JSON with version, active_slot
- Web UI System panel — update card visible with version and slot info
- Tool count should be 51 (47 + 4 update tools)
- Serial log should show `[update] Boot health check passed` (if grubenv has boot_counter)

**Step 5: Commit any fixes**

```bash
git commit -m "fix: build and deploy fixes for A/B update system"
```

---

## Task 9: Update Session Status + CLAUDE.md

**Files:**
- Modify: `SESSION-STATUS.md` — add Phase 4 progress
- Modify: `CLAUDE.md` — update status to Phase 4 in progress

**Step 1: Update SESSION-STATUS.md**

Add Phase 4 section with task status.

**Step 2: Update CLAUDE.md**

Update phase status and known bugs.

**Step 3: Commit**

```bash
git commit -m "docs: update session status for Phase 4 A/B updates"
```

---

## Summary

| Task | Description | ~LOC | Commit |
|------|-------------|------|--------|
| 1 | Version header, slot detection, grubenv R/W | 250 | feat: updater foundation |
| 2 | Ed25519 signature verification (TweetNaCl) | 750 | feat: Ed25519 verify |
| 3 | GRUB config for A/B boot | 50 | feat: GRUB A/B switching |
| 4 | Boot success health check | 100 | feat: boot health check |
| 5 | 4 update tools | 150 | feat: update tools |
| 6 | Web UI update card | 100 | feat: update card UI |
| 7 | Update HTTP endpoints | 80 | feat: update endpoints |
| 8 | Build, deploy, verify | 20 | fix: build/deploy |
| 9 | Session status + docs | 30 | docs: session status |
| **Total** | | **~1,530** | **9 commits** |

Tool count after: 51 (47 current + 4 update tools)
Test count after: ~170 (155 current + ~15 updater tests)
Test suites: 12 (11 current + 1 updater)
