# Console + Auth Implementation Plan

**Date**: 2026-03-05
**Design doc**: `docs/plans/2026-03-05-console-auth-design.md`

---

## Tasks

### Task 1: bcrypt Implementation
**Files**: `src/llamaste/bcrypt.h`, `src/llamaste/bcrypt.cpp`
**Spec**:
- Embed minimal bcrypt from OpenBSD reference
- `std::string bcrypt_hash(const std::string& password, int work_factor = 12)`
- `bool bcrypt_check(const std::string& password, const std::string& hash)`
- Uses `/dev/urandom` for salt generation
- Work factor 12 (default), supports 4-31
- Output format: `$2b$12$<22-char-salt><31-char-hash>`
**Tests**: Hash + verify round-trip, wrong password fails, different salts produce different hashes

### Task 2: AuthManager
**Files**: `src/llamaste/auth.h`, `src/llamaste/auth.cpp`
**Spec**:
- Load/save `/data/config/device.json`
- `set_device_password()` → bcrypt hash + save
- `verify_device_password()` → bcrypt check
- `create_session()` → 128-bit random hex token, stored in memory map
- `is_valid_session()` → lookup + expiry check
- `expire_sessions()` → remove expired (called every 60s by background thread)
- `is_authenticated(req)` → check cookie OR Bearer header
- `generate_api_key()` → auto-generated on first setup, stored in device.json
- Thread-safe (mutex on all map operations)
**Tests**: Create/verify password, session create/validate/expire, API key verify

### Task 3: Server Console Display
**Files**: `src/llamaste/supervisor.h`, `src/llamaste/supervisor.cpp`
**Spec**:
- New `console_display_thread(const SupervisorConfig&)` function
- Reads: `/proc/stat` (CPU), `/proc/meminfo` (RAM), `statvfs("/data")` (disk),
  `/sys/class/thermal/thermal_zone0/temp`, `ioctl(SIOCGIFADDR)` (IP), `gethostname()`
- Writes formatted VT100 output to `/dev/console` every 5 seconds
- Shows: status, uptime, CPU/RAM/disk bars, temp, IP, hostname, model, web URL
- Thread launched in `supervisor_run()` only when `boot_mode == "server"`
- Graceful shutdown on `g_shutdown_requested`
**Tests**: Format functions can be unit tested on host (bar rendering, uptime formatting)

### Task 4: Auth Routes + Middleware
**Files**: `src/llamaste/child_main.cpp`
**Spec**:
- Create global `AuthManager g_auth`
- Load config in `run_http_server()` before route setup
- `require_auth` lambda wrapping route handlers
- New routes:
  - `POST /llamaste/auth/setup` — first-boot password setup
  - `POST /llamaste/auth/login` — verify password, set cookie
  - `POST /llamaste/auth/logout` — clear session
  - `GET /llamaste/auth/status` — return auth state (setup_complete, is_authenticated)
- Protected routes: `/llamaste/chat`, `/llamaste/system`, `/llamaste/tools`,
  `/llamaste/files`, `/llamaste/conversations`, `/llamaste/schedules`,
  `/llamaste/notifications`, `/v1/chat/completions`
- Unprotected routes: `/health`, `/llamaste/auth/*`, static assets (login/setup pages)
- Root route `/` serves login.html if not authenticated, index.html if authenticated
- Start session expiry background thread
**Tests**: Auth middleware blocks unauthenticated requests, allows authenticated ones

### Task 5: Login + Setup Web UI
**Files**: `src/llamaste/web/login.html`, `src/llamaste/web/setup.html`
**Spec**:
- `login.html`: Dark theme, password field, submit button, error display, POST to /llamaste/auth/login
- `setup.html`: Dark theme, password + confirm fields, device name (optional), POST to /llamaste/auth/setup
- Both redirect to `/` on success via JavaScript
- Match existing dark theme (style.css variables)
- Minimal — no external dependencies, inline styles or shared style.css
**Build**: Add to embed_web.cmake WEB_FILES + CMakeLists.txt DEPENDS

### Task 6: Auth Tools
**Files**: `src/llamaste/tools_auth.cpp`
**Spec**:
- `auth.change_password` — change device password (requires current password)
- `auth.get_api_key` — return the auto-generated API key
- `auth.set_session_timeout` — change session timeout duration
- Register via `register_auth_tools(ToolRegistry&, AuthManager&)`
- All require confirmation flag
**Tests**: Tool handler unit tests

### Task 7: CMake + Build Integration
**Files**: `CMakeLists.txt`, `src/llamaste/CMakeLists.txt`, `embed_web.cmake`
**Spec**:
- Add bcrypt.cpp, auth.cpp, tools_auth.cpp to source list
- Add login.html, setup.html to web embed list
- Add routes in child_main.cpp for new pages
- No new Buildroot packages needed (bcrypt is embedded)

### Task 8: Host Tests
**Files**: `tests/test_auth.cpp`
**Spec**:
- bcrypt hash/verify round-trip
- bcrypt wrong password rejection
- AuthManager password set/verify
- AuthManager session create/validate/expire
- AuthManager API key verify
- Console display format functions (bar rendering, uptime formatting)
- Add to host-test.sh

---

## Implementation Order
1 → 2 → 3 (parallel with 4) → 4 → 5 → 6 → 7 → 8
