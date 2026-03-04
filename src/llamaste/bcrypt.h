#pragma once
// Minimal bcrypt implementation for Llamaste device password hashing.
// Based on the OpenBSD bcrypt reference (Provos & Mazieres, 1999).
// No external dependencies — uses Blowfish cipher internally.

#include <string>

// Hash a password with bcrypt. Returns "$2b$WW$<53 chars>" on success,
// or an empty string on failure. work_factor range: 4-31 (default 12,
// ~250ms on modern hardware).
std::string bcrypt_hash(const std::string& password, int work_factor = 12);

// Verify a password against a bcrypt hash. Returns true if the password
// matches, false otherwise. Constant-time comparison to prevent timing attacks.
bool bcrypt_check(const std::string& password, const std::string& hash);
