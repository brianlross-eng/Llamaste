#pragma once
// Llamaste device authentication: single device password + session cookies.
// No user accounts — one password grants full access.

#include <string>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <atomic>

// Forward declare httplib types to avoid pulling in the full header
namespace httplib {
    class Request;
    class Response;
}

class AuthManager {
public:
    // Load config from /data/config/device.json (or specified dir).
    // Creates directory and default config if missing.
    void load_config(const std::string& config_dir);

    // Save current config to device.json.
    void save_config();

    // --- Password management ---

    // Returns true if first-boot setup has been completed.
    bool is_setup_complete() const;

    // Hash and store device password. Returns true on success.
    bool set_device_password(const std::string& password);

    // Verify a password against the stored hash.
    bool verify_device_password(const std::string& password) const;

    // --- Session management ---

    // Create a new session, returns 32-hex-char token.
    std::string create_session();

    // Check if a session token is valid (exists and not expired).
    bool is_valid_session(const std::string& token) const;

    // Touch session (update last_seen timestamp).
    void touch_session(const std::string& token);

    // Remove expired sessions. Call periodically (e.g. every 60s).
    void expire_sessions();

    // Invalidate a specific session.
    void invalidate_session(const std::string& token);

    // --- API key ---

    // Get the auto-generated API key (created on first setup).
    std::string get_api_key() const;

    // Verify an API key (Bearer token).
    bool verify_api_key(const std::string& key) const;

    // --- Auth check (for route handlers) ---

    // Check if a request is authenticated via session cookie or Bearer token.
    // Returns true if:
    //   - Setup is not complete (no password set, allow access), OR
    //   - Valid session cookie present, OR
    //   - Valid Bearer API key present
    bool is_authenticated(const httplib::Request& req) const;

    // Extract session token from request cookie header.
    // Returns empty string if not found.
    static std::string extract_session_cookie(const httplib::Request& req);

    // Extract Bearer token from Authorization header.
    // Returns empty string if not found.
    static std::string extract_bearer_token(const httplib::Request& req);

    // Set session cookie on response.
    void set_session_cookie(httplib::Response& res, const std::string& token) const;

    // Clear session cookie on response.
    static void clear_session_cookie(httplib::Response& res);

    // --- Configuration ---

    int session_timeout_seconds() const { return session_timeout_seconds_; }
    void set_session_timeout(int seconds);

    // --- Brute force protection ---

    // Record a failed login attempt from an IP. Returns true if the IP
    // is now rate-limited (too many failures).
    bool record_failed_login(const std::string& ip);

    // Check if an IP is rate-limited.
    bool is_rate_limited(const std::string& ip) const;

private:
    std::string config_dir_;
    std::string config_path_;

    // Config state
    std::string password_hash_;
    std::string api_key_;
    std::string device_name_ = "llamaste";
    int session_timeout_seconds_ = 604800;  // 7 days
    bool setup_complete_ = false;

    // Sessions (in-memory, lost on restart)
    struct Session {
        std::chrono::steady_clock::time_point created;
        std::chrono::steady_clock::time_point last_seen;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;

    // Brute force tracking (in-memory)
    struct LoginAttempt {
        int failures = 0;
        std::chrono::steady_clock::time_point last_failure;
    };
    std::unordered_map<std::string, LoginAttempt> login_attempts_;
    static constexpr int MAX_FAILURES = 5;
    static constexpr int COOLDOWN_SECONDS = 30;

    // Generate a cryptographically random hex token
    static std::string generate_token(int bytes = 16);

    // Generate a random API key (readable format)
    static std::string generate_api_key();
};
