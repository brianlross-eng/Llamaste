// Llamaste device authentication implementation.
// Single device password with session cookies and API key support.

#include "auth.h"
#include "bcrypt.h"

// Use the vendored nlohmann JSON
#include "json.hpp"
using json = nlohmann::json;

// httplib types for cookie/header extraction
#include "httplib.h"

#include <fstream>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_p(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>   // getrandom() syscall
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 1
#endif
#endif
#define mkdir_p(path, mode) mkdir(path, mode)
#endif

// ---------- Helpers ----------

std::string AuthManager::generate_token(int bytes) {
    uint8_t buf[32];
    if (bytes > 32) bytes = 32;

#ifdef _WIN32
    // On Windows, use a simple fallback for testing
    srand((unsigned)time(nullptr));
    for (int i = 0; i < bytes; i++) buf[i] = rand() & 0xff;
#else
    // Prefer getrandom() syscall — blocks until entropy pool is ready.
    // Falls back to /dev/urandom with proper read() checking.
    bool filled = false;

#ifdef __linux__
    // getrandom() with no flags blocks until the kernel CSPRNG is seeded.
    // This is the preferred path — no file descriptor needed.
    ssize_t gr = getrandom(buf, (size_t)bytes, 0);
    if (gr == (ssize_t)bytes) {
        filled = true;
    }
#endif

    if (!filled) {
        // Fallback: /dev/urandom with retry and read() checking.
        // Block until /dev/urandom is available — the system cannot
        // securely generate tokens without a working entropy source.
        int fd = -1;
        while (fd < 0) {
            fd = open("/dev/urandom", O_RDONLY);
            if (fd < 0) {
                // If urandom isn't available yet, sleep and retry.
                // We MUST NOT fall through to predictable rand().
                usleep(100000);  // 100ms
            }
        }

        // Read with partial-read loop — read() may return fewer bytes
        // than requested, especially on early boot.
        size_t total = 0;
        while (total < (size_t)bytes) {
            ssize_t n = read(fd, buf + total, (size_t)bytes - total);
            if (n < 0) {
                if (errno == EINTR) continue;  // signal, retry
                // Fatal: /dev/urandom read error — this shouldn't happen
                // on a working system, but if it does, abort generation
                // rather than filling with zeroes or predictable data.
                close(fd);
                return "";
            }
            total += (size_t)n;
        }
        close(fd);
    }
#endif

    char hex[65];
    for (int i = 0; i < bytes; i++) {
        snprintf(hex + i * 2, 3, "%02x", buf[i]);
    }
    return std::string(hex, bytes * 2);
}

std::string AuthManager::generate_api_key() {
    // Format: "llm-XXXXXXXXXXXXXXXXXXXX" (20 hex chars)
    return "llm-" + generate_token(10);
}

// ---------- Config persistence ----------

void AuthManager::load_config(const std::string& config_dir) {
    config_dir_ = config_dir;
    config_path_ = config_dir + "/device.json";

    // Ensure config directory exists
    mkdir_p(config_dir.c_str(), 0700);

    // Try to read existing config
    std::ifstream f(config_path_);
    if (f.is_open()) {
        try {
            json j;
            f >> j;
            password_hash_ = j.value("password_hash", "");
            api_key_ = j.value("api_key", "");
            device_name_ = j.value("device_name", "llamaste");
            session_timeout_seconds_ = j.value("session_timeout_seconds", 604800);
            setup_complete_ = j.value("setup_complete", false);
        } catch (...) {
            // Corrupted config — start fresh
            fprintf(stderr, "[auth] Warning: corrupted device.json, starting fresh\n");
            password_hash_.clear();
            setup_complete_ = false;
        }
    } else {
        // First boot — no config file yet
        fprintf(stderr, "[auth] No device.json found, first-boot mode\n");
    }
}

void AuthManager::save_config() {
    std::lock_guard<std::mutex> lock(mutex_);

    json j;
    j["password_hash"] = password_hash_;
    j["api_key"] = api_key_;
    j["device_name"] = device_name_;
    j["session_timeout_seconds"] = session_timeout_seconds_;
    j["setup_complete"] = setup_complete_;

    // Write atomically: write to temp file, then rename
    std::string tmp_path = config_path_ + ".tmp";
    std::ofstream f(tmp_path);
    if (f.is_open()) {
        f << j.dump(2) << std::endl;
        f.close();
        rename(tmp_path.c_str(), config_path_.c_str());
    } else {
        fprintf(stderr, "[auth] Error: could not write %s\n", config_path_.c_str());
    }
}

// ---------- Password management ----------

bool AuthManager::is_setup_complete() const {
    return setup_complete_;
}

bool AuthManager::set_device_password(const std::string& password) {
    if (password.empty() || password.size() > 72) return false;

    // bcrypt with work factor 12 (~250ms)
    std::string hash = bcrypt_hash(password, 12);
    if (hash.empty()) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        password_hash_ = hash;
        setup_complete_ = true;

        // Generate API key on first setup
        if (api_key_.empty()) {
            api_key_ = generate_api_key();
        }
    }

    save_config();
    fprintf(stderr, "[auth] Device password set successfully\n");
    return true;
}

bool AuthManager::verify_device_password(const std::string& password) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (password_hash_.empty()) return false;
    return bcrypt_check(password, password_hash_);
}

// ---------- Session management ----------

std::string AuthManager::create_session() {
    std::string token = generate_token(16);  // 32 hex chars
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    sessions_[token] = Session{now, now};
    return token;
}

bool AuthManager::is_valid_session(const std::string& token) const {
    if (token.empty()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return false;

    // Check expiry
    auto elapsed = std::chrono::steady_clock::now() - it->second.last_seen;
    auto timeout = std::chrono::seconds(session_timeout_seconds_);
    return elapsed < timeout;
}

void AuthManager::touch_session(const std::string& token) {
    if (token.empty()) return;

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = sessions_.find(token);
    if (it != sessions_.end()) {
        it->second.last_seen = std::chrono::steady_clock::now();
    }
}

void AuthManager::expire_sessions() {
    auto now = std::chrono::steady_clock::now();
    auto timeout = std::chrono::seconds(session_timeout_seconds_);

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if ((now - it->second.last_seen) >= timeout) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }

    // Also clean up old login attempts
    for (auto it = login_attempts_.begin(); it != login_attempts_.end(); ) {
        auto elapsed = now - it->second.last_failure;
        if (elapsed > std::chrono::seconds(COOLDOWN_SECONDS * 10)) {
            it = login_attempts_.erase(it);
        } else {
            ++it;
        }
    }
}

void AuthManager::invalidate_session(const std::string& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(token);
}

// ---------- API key ----------

std::string AuthManager::get_api_key() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return api_key_;
}

bool AuthManager::verify_api_key(const std::string& key) const {
    if (key.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (api_key_.empty()) return false;

    // Constant-time comparison: always compare exactly 32 bytes regardless
    // of actual key length. API keys are "llm-" + 20 hex chars = 24 bytes,
    // well within the fixed 32-byte window. Short keys are padded with
    // zero bytes to avoid leaking key length via early return or variable
    // loop bounds. Uses volatile to block dead-store elimination.
    static constexpr size_t FIXED_LEN = 32;
    unsigned char diff = 0;
    for (size_t i = 0; i < FIXED_LEN; i++) {
        unsigned char a = (i < key.size())       ? static_cast<unsigned char>(key[i])       : 0;
        unsigned char b = (i < api_key_.size())  ? static_cast<unsigned char>(api_key_[i])  : 0;
        diff |= a ^ b;
    }
    volatile unsigned char vdiff = diff;
    return vdiff == 0;
}

// ---------- Auth check (for routes) ----------

std::string AuthManager::extract_session_cookie(const httplib::Request& req) {
    // Look for "llamaste_sid=<token>" in Cookie header
    auto it = req.headers.find("Cookie");
    if (it == req.headers.end()) return "";

    const std::string& cookies = it->second;
    const char* prefix = "llamaste_sid=";
    size_t pos = cookies.find(prefix);
    if (pos == std::string::npos) return "";

    pos += strlen(prefix);
    size_t end = cookies.find(';', pos);
    if (end == std::string::npos) end = cookies.size();
    return cookies.substr(pos, end - pos);
}

std::string AuthManager::extract_bearer_token(const httplib::Request& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return "";

    const std::string& auth = it->second;
    if (auth.size() > 7 && auth.substr(0, 7) == "Bearer ") {
        return auth.substr(7);
    }
    return "";
}

bool AuthManager::is_authenticated(const httplib::Request& req) {
    // If setup not complete, allow everything (first-boot mode)
    if (!setup_complete_) return true;

    // Check session cookie
    std::string sid = extract_session_cookie(req);
    if (is_valid_session(sid)) {
        // Touch session to extend timeout
        touch_session(sid);
        return true;
    }

    // Check Bearer API key
    std::string bearer = extract_bearer_token(req);
    if (verify_api_key(bearer)) {
        return true;
    }

    return false;
}

void AuthManager::set_session_cookie(httplib::Response& res, const std::string& token) const {
    char cookie[256];
    snprintf(cookie, sizeof(cookie),
             "llamaste_sid=%s; HttpOnly; SameSite=Lax; Path=/; Max-Age=%d",
             token.c_str(), session_timeout_seconds_);
    res.set_header("Set-Cookie", cookie);
}

void AuthManager::clear_session_cookie(httplib::Response& res) {
    res.set_header("Set-Cookie",
        "llamaste_sid=; HttpOnly; SameSite=Lax; Path=/; Max-Age=0");
}

// ---------- Brute force protection ----------

bool AuthManager::record_failed_login(const std::string& ip) {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(mutex_);
    auto& attempt = login_attempts_[ip];

    // Reset if cooldown has passed
    auto elapsed = now - attempt.last_failure;
    if (elapsed > std::chrono::seconds(COOLDOWN_SECONDS)) {
        attempt.failures = 0;
    }

    attempt.failures++;
    attempt.last_failure = now;
    return attempt.failures >= MAX_FAILURES;
}

bool AuthManager::is_rate_limited(const std::string& ip) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = login_attempts_.find(ip);
    if (it == login_attempts_.end()) return false;

    if (it->second.failures < MAX_FAILURES) return false;

    auto elapsed = std::chrono::steady_clock::now() - it->second.last_failure;
    return elapsed < std::chrono::seconds(COOLDOWN_SECONDS);
}

// ---------- Configuration ----------

void AuthManager::set_session_timeout(int seconds) {
    if (seconds < 60) seconds = 60;           // minimum 1 minute
    if (seconds > 31536000) seconds = 31536000; // maximum 1 year

    {
        std::lock_guard<std::mutex> lock(mutex_);
        session_timeout_seconds_ = seconds;
    }
    save_config();
}
