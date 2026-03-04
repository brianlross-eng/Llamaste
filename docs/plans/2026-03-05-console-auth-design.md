# Server Console + Device Password Design

**Date**: 2026-03-05
**Status**: Approved (user confirmed 2026-03-04)
**Version**: Llamaste 1.0.000a

---

## 1. Server Console Status Display

### Purpose
In server (headless) mode, the physical display/serial console shows a VT100-formatted
status screen so administrators can confirm the system is running without needing a
browser.

### Architecture
- **Location**: Supervisor process (PID 1) in `supervisor.cpp`
- **Thread**: New `console_display_thread()` launched from `supervisor_run()`
- **Data source**: Direct reads from `/proc/stat`, `/proc/meminfo`, `statvfs()`,
  network interfaces — no dependency on child HTTP server being up
- **Output**: `/dev/console` via `open()`/`write()`/`close()` cycle
- **Refresh**: Every 5 seconds
- **Boot mode**: Only runs when `boot_mode == "server"` (skip desktop/live)

### Display Format
```
╔══════════════════════════════════════════════╗
║          LLAMASTE 1.0.000a  SERVER           ║
╠══════════════════════════════════════════════╣
║  Status:   RUNNING                           ║
║  Uptime:   2d 5h 23m                         ║
║  CPU:      ████████░░░░░░░░  47%             ║
║  RAM:      ██████████░░░░░░  3.2 / 7.8 GB   ║
║  Disk:     ██░░░░░░░░░░░░░░  1.2 / 14.1 GB  ║
║  Temp:     42°C                              ║
║  IP:       192.168.1.100                      ║
║  Hostname: llamaste.local                     ║
║  Model:    qwen2.5-0.5b-instruct-q4_k_m     ║
║  Web UI:   http://192.168.1.100              ║
╠══════════════════════════════════════════════╣
║  Active connections: 2  │  Requests: 1,247   ║
║  Child PID: 42  │  Restarts: 0               ║
╚══════════════════════════════════════════════╝
```

### Data Gathering (in supervisor, no HTTP dependency)
| Metric | Source | Method |
|--------|--------|--------|
| CPU % | `/proc/stat` | Two reads 100ms apart, delta calculation |
| RAM | `/proc/meminfo` | `MemTotal` - `MemAvailable` |
| Disk | `statvfs("/data")` | Used / total blocks |
| Temperature | `/sys/class/thermal/thermal_zone0/temp` | Read and divide by 1000 |
| IP | `/proc/net/fib_trie` or `ioctl(SIOCGIFADDR)` | First non-loopback interface |
| Uptime | `clock_gettime(CLOCK_MONOTONIC)` | Seconds since boot |
| Child PID | Already tracked in supervisor | `g_child_pid` variable |
| Restarts | Already tracked in supervisor | `g_crash_count` counter |
| Hostname | `gethostname()` | Static |
| Model | `config.model_path` | Passed from main |

### Request counter
The supervisor needs to know active connections and total requests. Options:
- **Shared memory counter**: Child increments atomics in mmap'd region; supervisor reads
- **Simple approach**: Supervisor doesn't show connection count (it can't see inside child)
- **Chosen**: Skip connection/request counts for Phase 1. Show static info only. Add
  connection stats when we implement IPC in Phase 2.

### VT100 Control
```cpp
// Clear screen and position cursor at top-left
write(fd, "\033[2J\033[H", 7);
// Bold text
write(fd, "\033[1m", 4);
// Normal text
write(fd, "\033[0m", 4);
// Green for OK status
write(fd, "\033[32m", 5);
// Red for error status
write(fd, "\033[31m", 5);
```

---

## 2. Device Password + Session Auth

### Purpose
Protect the web UI and API endpoints with a single device password. No user accounts —
one password grants full access. Set on first boot via a setup wizard.

### Architecture
- **New files**: `auth.h`, `auth.cpp` (~300 LOC)
- **Modified files**: `child_main.cpp` (auth checks), web UI (login page)
- **Password hashing**: bcrypt (embedded implementation, ~200 LOC, no dependencies)
- **Session storage**: In-memory `std::unordered_map` (lost on restart = re-login)
- **Config file**: `/data/config/device.json`

### Why bcrypt (not Argon2id)
- bcrypt is ~200 lines of C — can be embedded directly in the binary
- Argon2id requires either the PHC reference library or Botan as a Buildroot package
- Both provide adequate security for a LAN appliance device password
- bcrypt work factor 12 = ~250ms on modern hardware

### device.json Format
```json
{
  "password_hash": "$2b$12$...",
  "device_name": "llamaste",
  "setup_complete": true,
  "session_timeout_seconds": 604800
}
```

### Auth Flow

#### First Boot (no password set)
```
Browser → GET / → server checks device.json
  → setup_complete == false → serve setup.html
  → user enters new password
  → POST /llamaste/auth/setup { "password": "..." }
  → server hashes with bcrypt, stores in device.json
  → server creates session cookie, redirects to /
```

#### Normal Login (password set)
```
Browser → GET / → server checks session cookie
  → no cookie or expired → serve login.html
  → user enters password
  → POST /llamaste/auth/login { "password": "..." }
  → server verifies with bcrypt_check()
  → success → Set-Cookie: llamaste_sid=<128-bit-hex>; HttpOnly; SameSite=Lax; Path=/; Max-Age=604800
  → redirect to /
  → failure → 401, show error on login page
```

#### Authenticated Request
```
Browser → GET /llamaste/chat (Cookie: llamaste_sid=abc123)
  → server looks up session in memory
  → found and not expired → proceed with request
  → not found or expired → 401 redirect to login
```

#### API Key (Bearer Token)
```
curl -H "Authorization: Bearer <api-key>" http://llamaste.local/v1/chat/completions
  → server checks api key against stored hash
  → valid → proceed
  → invalid → 401
```

### AuthManager Class (Simplified for Phase 1)
```cpp
class AuthManager {
public:
    // Load/save config from /data/config/device.json
    void load_config(const std::string& config_dir);
    void save_config();

    // Password management
    bool is_setup_complete() const;
    bool set_device_password(const std::string& password);
    bool verify_device_password(const std::string& password) const;

    // Session management
    std::string create_session();  // returns session token
    bool is_valid_session(const std::string& token) const;
    void expire_sessions();  // called periodically

    // API key (Phase 1: single auto-generated key)
    std::string get_api_key() const;
    bool verify_api_key(const std::string& key) const;

    // Auth check helper for routes
    bool is_authenticated(const httplib::Request& req) const;

private:
    std::string config_dir_;
    std::string password_hash_;
    std::string api_key_;
    int session_timeout_seconds_ = 604800;  // 7 days
    bool setup_complete_ = false;

    struct Session {
        std::chrono::steady_clock::time_point created;
        std::chrono::steady_clock::time_point last_seen;
    };
    mutable std::mutex mutex_;
    std::unordered_map<std::string, Session> sessions_;
};
```

### Route Auth Pattern
```cpp
// Helper lambda wrapping auth check
auto require_auth = [&](auto handler) {
    return [&, handler](const httplib::Request& req, httplib::Response& res) {
        if (!g_auth.is_authenticated(req)) {
            res.status = 401;
            res.set_content(R"({"error":"Unauthorized"})", "application/json");
            return;
        }
        handler(req, res);
    };
};

// Usage:
svr.Get("/llamaste/system", require_auth(handle_system));
svr.Post("/llamaste/chat", require_auth(handle_chat));

// Exempt routes (no auth needed):
svr.Get("/health", handle_health);
svr.Get("/", handle_index);  // serves login page if not authed
svr.Post("/llamaste/auth/login", handle_login);
svr.Post("/llamaste/auth/setup", handle_setup);
```

### Web UI Changes

#### login.html (new file)
- Simple password field + submit button
- Dark theme matching existing UI
- Shows error message on failed login
- Redirects to / on success

#### setup.html (new file)
- First-boot wizard
- Password field + confirm field
- Optional: device name
- Stores password and redirects to main UI

#### index.html (modified)
- No changes needed — server decides whether to serve index.html or login.html

### Session Token Generation
```cpp
std::string generate_session_token() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t hi = dist(gen), lo = dist(gen);
    char buf[33];
    snprintf(buf, sizeof(buf), "%016llx%016llx", hi, lo);
    return std::string(buf, 32);
}
```

### bcrypt Integration
Embed a minimal bcrypt implementation:
- `bcrypt_hash(password, work_factor)` → returns "$2b$12$..." hash string
- `bcrypt_check(password, hash)` → returns true/false
- Based on the OpenBSD bcrypt reference (~200 lines of C)
- Compiled directly into the llamaste binary

### Security Considerations
- **HttpOnly cookies**: JavaScript cannot read session tokens (XSS protection)
- **SameSite=Lax**: Browser won't send cookie on cross-site POST (CSRF protection)
- **No Secure flag**: Llamaste runs HTTP on LAN (no TLS in Phase 1)
- **Session timeout**: 7 days default, configurable
- **Password change**: Via `auth.change_password` tool or settings UI
- **Brute force**: Simple rate limit — 5 failed attempts = 30 second cooldown per IP

### What's Deferred to Phase 2+
- Multi-user accounts
- Role-based access control
- API key management UI
- TLS/HTTPS
- OAuth/SSO
- CSRF tokens (SameSite=Lax provides adequate protection for LAN)
- Full rate limiting

---

## 3. New Files Summary

| File | Purpose | Est. LOC |
|------|---------|----------|
| `auth.h` | AuthManager class declaration | ~60 |
| `auth.cpp` | AuthManager implementation | ~250 |
| `bcrypt.h` | bcrypt hash/check declarations | ~15 |
| `bcrypt.cpp` | bcrypt implementation (embedded) | ~220 |
| `tools_auth.cpp` | auth.* tools (set_password, etc.) | ~100 |
| `login.html` | Login page | ~80 |
| `setup.html` | First-boot setup wizard | ~100 |
| Console display code (in supervisor.cpp) | ~150 |
| **Total new code** | | **~975** |

---

## 4. Implementation Order

1. **bcrypt.h/cpp** — standalone, no dependencies, testable immediately
2. **auth.h/cpp** — AuthManager with bcrypt, session management, config persistence
3. **Console display** — supervisor.cpp changes (independent of auth)
4. **child_main.cpp** — auth middleware, login/setup routes
5. **login.html + setup.html** — web UI for auth flow
6. **tools_auth.cpp** — LLM-accessible auth tools
7. **embed_web.cmake + CMakeLists.txt** — add new web assets
8. **Host tests** — auth manager tests, bcrypt tests
