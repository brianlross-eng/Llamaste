#pragma once
// updater.h -- A/B update system: slot detection, grubenv R/W, manifest parsing
//
// The updater manages dual-partition (A/B) boot slots for atomic OS updates.
// Slot detection reads /proc/cmdline for llamaste.slot=A|B.
// GRUB environment block (grubenv) is a 1024-byte file used to coordinate
// slot selection and boot success tracking between the updater and GRUB.

#include <string>
#include <map>
#include <vector>
#include <cstdint>
#include "version.h"

// --- Slot detection ---

// Extract active slot from kernel cmdline string (looks for "llamaste.slot=X")
// Returns "A" or "B"; defaults to "A" if not found.
std::string parse_active_slot(const std::string& cmdline);

// Return the inactive slot: A->B, B->A
std::string inactive_slot(const std::string& active);

// Return the GPT partition number of the inactive system slot.
// A is active -> inactive is B (partition 4), B is active -> inactive is A (partition 3)
int inactive_partition_num(const std::string& active);

// Detect the currently booted slot by reading /proc/cmdline (Linux).
// On Windows, always returns "A" (for host testing).
std::string detect_current_slot();

// --- GRUB environment block ---

// Parse a 1024-byte grubenv file content into key-value pairs.
// Skips comment lines (starting with '#').
std::map<std::string, std::string> grubenv_parse(const std::string& content);

// Serialize key-value pairs into a 1024-byte grubenv block.
// Format: "# GRUB Environment Block\n" header, then "key=value\n" pairs,
// padded with '#' characters to exactly 1024 bytes.
std::string grubenv_serialize(const std::map<std::string, std::string>& vars);

// Read a grubenv file and parse it into a map.
// Returns empty map if file cannot be read.
std::map<std::string, std::string> grubenv_read(const std::string& path);

// Atomic write of grubenv: writes to path.tmp, fsyncs, then renames.
// Returns true on success.
bool grubenv_write(const std::string& path, const std::map<std::string, std::string>& vars);

// --- Version comparison ---

// Compare two semver strings (major.minor.patch).
// Returns <0 if a < b, 0 if equal, >0 if a > b.
int version_compare(const std::string& a, const std::string& b);

// --- Update manifest ---

struct UpdateComponent {
    std::string file;
    std::string sha256;
    uint64_t size_compressed = 0;
    uint64_t size_uncompressed = 0;
};

struct UpdateManifest {
    bool valid = false;
    int format_version = 0;
    std::string version;
    std::string build_date;
    std::string arch;
    std::string min_version;        // minimum installed version required
    UpdateComponent system;          // system image component
    std::vector<std::string> changelog;
};

// Parse an update manifest JSON string into an UpdateManifest struct.
// Sets valid=true if version is non-empty and format_version > 0.
UpdateManifest parse_manifest(const std::string& json_str);

// --- Ed25519 signature verification ---

// Verify Ed25519 detached signature. Returns true if valid.
// sig: 64-byte signature, msg: message bytes, pubkey: 32-byte public key
bool verify_update_signature(
    const unsigned char* sig, size_t sig_len,
    const unsigned char* msg, size_t msg_len,
    const unsigned char* pubkey
);

// --- ESP / grubenv helpers ---

// Search well-known paths for grubenv on the ESP.
// Returns the first path that exists, or empty string if none found.
std::string find_grubenv_path();

// Mark the current boot as successful: sets boot_success=1 and
// clears boot_counter in grubenv. Mounts ESP if needed.
// No-op on Windows.
void mark_boot_success();
