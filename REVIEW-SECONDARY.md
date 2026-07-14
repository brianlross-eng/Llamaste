# Llamaste — Secondary Code Review

**Date**: 2026-07-14  
**Scope**: Verification of 15 post-review fixes + hunt for missed issues  
**Reviewed by**: Automated secondary agent review  
**Prior reviews**: REVIEW.md (24 issues), REVIEW-HARDWARE.md (7 issues) — READ FIRST

---

## PART 1: VERIFICATION OF 15 FIXES

All 15 commits were spot-checked via `git diff`. Summary of verification:

| Commit | Fix | Applied? | Notes |
|--------|-----|----------|-------|
| `c0ef01f` | Reboot from child sends SIGUSR1/SIGTERM | ✅ Correct | `#include <sys/reboot.h>` now unused but harmless |
| `c1190c4` | Replace rand() with getrandom() in tokens | ✅ Correct | Partial-read loop handles short reads; EINTR retry; Windows path untouched (acceptable — Linux appliance) |
| `b8f5894` | Make stderr tee thread joinable | ✅ Correct | Cleanup with 2s timeout → detach fallback. Minor: timeout-based detach leaks thread stack on pathologically slow tees |
| `1b1a8a6` | SIZE_MAX check before malloc | ✅ Correct | Uses `alloc_size` consistently across malloc/fread/sha256_hex |
| `e504cba` | Check snprintf in http_post_localhost | ✅ Correct | Headers separated from body; truncation checked; body written via separate write() |
| `b9db981` | Harden API key comparison | ✅ Correct | Fixed 32-byte loop with zero-padding. `volatile` trick on local is fragile but improvement over prior |
| `82ff6b4` | Mask PSK in wpa_supplicant error logging | ✅ Correct | Handles quoted and unquoted PSK values |
| `ac26873` | Reduce compositor RAM reserve 500→128MB | ✅ Correct | Comment explains rationale |
| `81f85fb` | Reorder network init: ethernet before WiFi | ✅ Correct | Uses `ethernet_got_ip` flag; skips WiFi entirely when ethernet succeeds |
| `5debbdb` | Ethernet DHCP pre-flight in supervisor | ✅ Correct | Server mode gets wired network before child spawns |
| `85c1687` | Discover root block device from /proc/mounts | ✅ Correct | Handles NVMe (`pN` suffix), mmcblk, SATA/VirtIO; falls back to hardcoded candidates |
| `a8818c0` | Close SIGCHLD race in signal handler | ✅ Correct | Handler calls waitpid(WNOHANG); main loop checks g_child_exited before blocking waitpid |
| `b3f28c3` | Remove const_cast UB on execv in init.cpp | ⚠️ Applied — **regression** | `run_amixer` lambda uses `strdup()` but never frees strings. See REGRESSION-1 below |
| `5bc71e9` | Load Intel Xe GPU module | ✅ Correct | Single line addition |
| `82d505d` | Add GPU VRAM detection | ✅ Correct | AMDGPU (`mem_info_vram_total`) and Intel (`total_vram`) paths; sets discrete/unified flags |

---

## PART 2: REGRESSIONS INTRODUCED BY FIXES

### REGRESSION-1: Memory leak in `init.cpp` `run_amixer` lambda (`b3f28c3`)

`init.cpp`, the `run_amixer` lambda introduced by fix `b3f28c3`:

```cpp
auto run_amixer = [](const char* amixer_path, const char* const* cargs) {
    int n = 0;
    while (cargs[n]) n++;
    std::vector<char*> argv(n + 1);
    for (int i = 0; i < n; i++) {
        argv[i] = strdup(cargs[i]);  // ALLOCATED — never freed
    }
    argv[n] = nullptr;
    pid_t pid = fork();
    if (pid == 0) {
        execv(amixer_path, argv.data());  // OK: exec replaces process, OS frees
        _exit(127);
    }
    if (pid > 0) {
        // ... wait for child ...
    }
    // LEAK: strdup'd strings in argv are never freed when lambda returns
};
```

Each call to `run_amixer` leaks 3–6 `strdup()`'d strings (the argv entries). On the init process this is one-shot and the OS reclaims everything on exit — but for any process with a longer lifetime (if this pattern is copied), this would accumulate. **Severity: LOW** (init is single-shot), but worth cleaning up for code hygiene.

### REGRESSION-2: `const_cast` UB fix incomplete — missed instances in `child_main.cpp`

Fix `b3f28c3` correctly addressed all `const_cast` on execv arguments in `init.cpp`. However, it missed three instances in `child_main.cpp`:

| Line | Context |
|------|---------|
| 907 | `execv("/opt/llamaste/llama-server", const_cast<char**>(args.data()))` — llama-server spawn |
| 2739 | `execv(reload_argv[0], const_cast<char**>(reload_argv))` — iw reg reload |
| 2745 | `execv(set_argv[0], const_cast<char**>(set_argv))` — iw reg set |

These compile without warning but are technically undefined behavior in C++ (casting away const from string literals passed to exec). On any practical Linux system, execv does not modify its argv, but a standards-conforming compiler could optimize around the cast. The fix should be extended to cover these call sites. **Severity: MEDIUM**.

---

## PART 3: NEW FINDINGS — MISSED BY PRIOR REVIEWS

### N1 (CRITICAL): MCP API key generated with non-cryptographic PRNG

**File**: `mcp_server.cpp:571-590`  
**Function**: `McpServer::gen_random_hex()`

```cpp
std::string McpServer::gen_random_hex(int bytes) {
    static std::mt19937_64 rng(
        std::chrono::steady_clock::now().time_since_epoch().count()
        ^ ((uint64_t)getpid() << 32)
    );
    // ... generates hex output from uniform_int_distribution
}
```

The MCP Bearer token (used for programmatic API access) is generated with **`std::mt19937_64`** — a Mersenne Twister PRNG that is **NOT cryptographically secure**. It is seeded with:
- `steady_clock::now().time_since_epoch().count()` — the process start time (low-precision, guessable within seconds)
- `getpid() << 32` — the process PID (guessable within a small range on embedded Linux)

An attacker who knows (or can estimate) the boot time and PID of the child process can **regenerate the MCP API key offline**. With 64 bits of seed entropy from guessable sources, a brute-force search of plausible (timestamp, PID) pairs is feasible.

The prior fix `c1190c4` correctly addressed `AuthManager::generate_token()` by switching to `getrandom()` — but **`gen_random_hex()` is a completely independent function with its own insecure PRNG**. The MCP key is generated via `gen_random_hex()`, not via `AuthManager::generate_token()`.

**Impact**: The MCP Bearer token — used to authenticate all programmatic API access — is predictable. An attacker on the same network can brute-force the token and gain full API access.

**Fix**: Replace `std::mt19937_64` with `getrandom()` or `/dev/urandom`, matching the approach used in `auth.cpp` after fix `c1190c4`.

---

### N2 (HIGH): No HTTP request body size limit — memory exhaustion DoS

**File**: `child_main.cpp` (HTTP server initialization, ~line 3229)  

`set_payload_max_length()` is **never called** on the httplib `Server` instance. All POST endpoints (`/v1/chat/completions`, `/llamaste/chat`, `/llamaste/audio/transcribe`, `/llamaste/model/download`, etc.) accept **unbounded request bodies** that are fully read into a `std::string` before parsing.

An attacker can POST a multi-gigabyte body to exhaust process memory and crash the HTTP server. On a device with limited RAM (e.g., 4GB SBC), a 2GB POST would trigger OOM.

**Impact**: Remote unauthenticated denial of service (for endpoints like `/llamaste/auth/login` and `/health`) or authenticated DoS (for auth-required endpoints). Process crash kills all active connections and the compositor thread (in desktop mode).

**Fix**: Add `svr.set_payload_max_length(10 * 1024 * 1024)` (10MB) — sufficient for audio uploads and conversation history, while preventing memory exhaustion.

---

### N3 (HIGH): Desktop mode disables ALL authentication globally

**File**: `child_main.cpp:3244-3262`

```cpp
auto require_auth = [](std::function<...> handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        // Desktop mode: all requests come from cog on localhost — no auth needed.
        // Remove this bypass once keyboard input in Wayland is confirmed working.
        if (g_boot_mode == "desktop") {
            handler(req, res);     // <--- AUTH COMPLETELY BYPASSED
            return;
        }
        // ... normal auth check ...
    };
};
```

When `g_boot_mode == "desktop"`, **every** authenticated route is accessible without credentials. This includes:
- `/llamaste/chat` — full agent chat with tool execution
- `/llamaste/shutdown`, `/llamaste/reboot` — system power control
- `/v1/chat/completions` — OpenAI-compatible API
- `/llamaste/model/*` — model management
- `/llamaste/tool` — arbitrary tool dispatch

The comment states this is a TODO pending keyboard input support, but the bypass has no network-level restriction — it trusts `req.remote_addr`, but in desktop mode the HTTP server still binds to `0.0.0.0`, making it accessible from the LAN.

**Impact**: Any device on the same network can access the full API without authentication when booted in desktop mode. This includes shutdown/reboot, arbitrary tool execution, and access to stored conversations.

**Fix**: At minimum, restrict the bypass to requests from `127.0.0.1` / `::1`. Better: remove the bypass and fix keyboard input in Wayland so auth works properly.

---

### N4 (HIGH): `fork()` called from multi-threaded HTTP server — potential deadlock

**File**: `child_main.cpp` — 13 `fork()` calls at lines 847, 919, 1041, 1275, 2696, 2738, 2744, 3014, 3027, 3082, 3200  
**Also**: `supervisor.cpp:1016` (in `supervisor_scan_wifi`)

The httplib server runs with a thread pool (default: 8 threads). When any route handler calls a function that internally calls `fork()` (e.g., `spawn_llama_server`, `spawn_wpa_supplicant`, `spawn_dhcpcd`), the child process inherits the parent's memory space but **only the calling thread**. All other threads' mutexes, condition variables, and file locks remain in whatever state they were at the moment of `fork()`.

The child code between `fork()` and `exec()` performs operations that are **not async-signal-safe**:

| Call site | Unsafe pre-exec operations |
|-----------|---------------------------|
| `spawn_llama_server` (line 847) | `open()`, `dup2()`, `close()`, `std::vector::push_back()` (heap alloc), `std::string::c_str()`, `fprintf()` |
| `spawn_wpa_supplicant` (line 1041) | `setsid()`, `open()`, `dup2()`, `close()`, `dprintf()` |
| `spawn_dhcpcd` (line 2696) | `std::string` operations, `execl()` parameter construction |

If any other thread held the heap allocator mutex at the moment of `fork()`, the child's `push_back()` or `std::to_string()` will **deadlock permanently**. The child hangs before reaching `exec()`, and the parent's route handler thread is blocked waiting for `waitpid()` or socket operations.

**Impact**: Sporadic, non-deterministic hangs when spawning subprocesses under load. On a single-core or lightly-threaded system, this may never trigger; under heavy HTTP traffic, the probability increases. A hung child may also leave zombie processes if `waitpid()` is never reached.

**Fix**: Either:
1. Use `posix_spawn()` instead of `fork()`+`exec()` (best option — avoids the problem entirely)
2. Call `fork()` from a dedicated single-threaded process, not from httplib handler threads
3. Pre-fork at startup before httplib starts its thread pool

---

### N5 (MEDIUM): Child's stderr tee thread has same bugs as supervisor's (not fixed)

**File**: `child_main.cpp:1972-1983`

```cpp
std::thread([read_end, log_fd, orig_console]() {
    char buf[512];
    while (true) {
        ssize_t n = read(read_end, buf, sizeof(buf));
        if (n <= 0) break;
        write(log_fd, buf, n);                    // no error check
        if (orig_console >= 0) write(orig_console, buf, n);  // no error check
    }
    close(read_end);
    close(log_fd);
    if (orig_console >= 0) close(orig_console);
}).detach();
```

The supervisor's identical tee thread was fixed in commit `b8f5894` — it's now joinable, checks `write()` errors, and is cleaned up on shutdown. The child process's tee thread (which writes to `/tmp/child.log`) was **not fixed**. It has the same issues:
1. **Detached** — if the thread throws, `std::terminate()` kills the HTTP server
2. **No write error checking** — ENOSPC on `/tmp` silently loses debug logs
3. **No cleanup on shutdown** — the pipe write-end is never explicitly closed; the thread may linger after the HTTP server exits

**Fix**: Apply the same fix pattern used in `b8f5894` — make joinable, check write() errors, close pipe write-end on shutdown.

---

### N6 (MEDIUM): Multiple `.detach()` calls — crash-on-exception risk

`child_main.cpp` contains **19 `.detach()` calls** on `std::thread` objects. The prior review (REVIEW.md #21) noted the compositor thread detach specifically, but the full scope was not cataloged. Any unhandled exception in a detached thread calls `std::terminate()`, killing the entire process including the HTTP server and all active connections.

Detached threads include:
- Child stderr tee (line 1983)
- llama-server health monitor (line 2080)
- Cluster heartbeat (line 2408)
- Session expiry (line 2911)
- Compositor launch (line 3224)
- Cluster model download (line 1947)
- Cluster election (line 4172)
- Shutdown watcher (line 4764)
- Boot success thread (line 4781)

**Recommendation**: Audit each detached thread — move to joinable management, or at minimum wrap the thread body in a `try { ... } catch (...) { }` block.

---

### N7 (MEDIUM): CURL download uses HTTPS with unverified fallback

**File**: `child_main.cpp:1914-1916`

```cpp
if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
    curl_easy_setopt(curl, CURLOPT_CAINFO,
                     "/etc/ssl/certs/ca-certificates.crt");
```

The model download thread uses libcurl with HTTPS URLs (`https://huggingface.co/...`). If the CA bundle at `/etc/ssl/certs/ca-certificates.crt` is missing (not installed in the Buildroot image, or deleted), `CURLOPT_CAINFO` is never set, and **curl silently falls back to unverified TLS connections**. No `CURLOPT_SSL_VERIFYPEER` or `CURLOPT_SSL_VERIFYHOST` is explicitly set — curl defaults to verifying, but if the CA bundle can't be found, verification fails silently and the connection may succeed without any certificate validation depending on the curl build.

The prior review (REVIEW.md #5) noted that HTTPS is explicitly disabled for the HTTP server, which makes this curl-based HTTPS usage inconsistent — model downloads go over TLS while all API traffic is cleartext.

**Fix**: Explicitly set `CURLOPT_SSL_VERIFYPEER = 1L` and `CURLOPT_SSL_VERIFYHOST = 2L`. Add a fallback CA bundle path. If no CA bundle is available, abort or warn prominently.

---

### N8 (MEDIUM): `strtol` without overflow checking in scheduler

**File**: `scheduler.cpp:309, 662`

```cpp
long v = strtol(field.c_str(), &end, 10);
if (end == field.c_str()) return true;  // parse failure -> treat as *
value = static_cast<int>(v);
```

`strtol` sets `errno = ERANGE` on overflow but the code **never checks `errno`**. If a cron field is `"99999999999999999999"`, `strtol` returns `LONG_MAX` (> 59 for minute field) and the range check `want_min > 59` catches it — but for fields without a tight range, the overflowed value silently becomes valid. Worse, if `LONG_MAX` happens to fall within the valid range, an attacker-supplied cron expression could produce unexpected scheduling behavior.

Same pattern at line 662 for thermal zone temperature parsing.

**Fix**: Check `errno` after `strtol`/`strtoul` calls. Set `errno = 0` before the call and check `errno == ERANGE` after.

---

### N9 (LOW): `Access-Control-Allow-Origin: *` on all responses

**File**: `child_main.cpp:3232-3236`

```cpp
svr.set_default_headers({
    {"Access-Control-Allow-Origin", "*"},
    {"Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS"},
    {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
});
```

Combined with the lack of HTTPS (REVIEW.md #5) and `HttpOnly` cookies without `Secure` flag, this means **any website** can make authenticated requests to the device if the user's browser has a valid `llamaste_sid` cookie. A malicious ad on any webpage could:
1. Send a POST to `http://<device-ip>/llamaste/tool` with arbitrary tool arguments
2. Read sensitive data from `/llamaste/system`
3. Trigger shutdown/reboot

This is a classic CSRF vulnerability enabled by permissive CORS + cleartext cookies. The `SameSite=Lax` cookie setting mitigates some attack vectors but does not protect against top-level navigations or form submissions.

**Fix**: Remove the wildcard CORS header. Set `Access-Control-Allow-Origin` to the specific origin(s) that need access, or omit it entirely for same-origin-only access. Add CSRF tokens for state-changing operations.

---

### N10 (LOW): `dhcpcd-hook.c` — no timeout on `waitpid` for `ip route del`

**File**: `dhcpcd-hook.c:72-76`

```cpp
pid_t pid = fork();
if (pid == 0) {
    execl("/sbin/ip", "ip", "route", "del", "default", (char*)NULL);
    _exit(0);
}
if (pid > 0) waitpid(pid, NULL, 0);  // blocks indefinitely
```

If `/sbin/ip` hangs (e.g., NFS mount is stale, kernel route table lock contention), the dhcpcd hook process blocks forever with no timeout. dhcpcd may time out waiting for its hook and restart the DHCP process unnecessarily.

**Fix**: Add a 5-second timeout with `alarm()` or a `select()`/`waitpid` polling loop.

---

## PART 4: SUMMARY OF NEW FINDINGS

| # | Severity | File(s) | Issue |
|---|----------|---------|-------|
| N1 | **CRITICAL** | `mcp_server.cpp:571` | MCP API key uses non-cryptographic `std::mt19937_64` — predictable token |
| N2 | **HIGH** | `child_main.cpp:3229` | No HTTP body size limit — memory exhaustion DoS |
| N3 | **HIGH** | `child_main.cpp:3248` | Desktop mode bypasses ALL auth — full API access without credentials |
| N4 | **HIGH** | `child_main.cpp` (13 sites) | `fork()` from multi-threaded HTTP server — potential deadlock from inherited mutexes |
| N5 | **MEDIUM** | `child_main.cpp:1972` | Child's stderr tee thread not fixed (same bugs as supervisor's was) |
| N6 | **MEDIUM** | `child_main.cpp` (19 sites) | 19 `.detach()` calls — any exception kills entire process |
| N7 | **MEDIUM** | `child_main.cpp:1914` | CURL HTTPS falls back to unverified TLS if CA bundle missing |
| N8 | **MEDIUM** | `scheduler.cpp:309,662` | `strtol` without `errno` check — silent overflow |
| N9 | **LOW** | `child_main.cpp:3232` | `Access-Control-Allow-Origin: *` + cleartext cookies = CSRF |
| N10 | **LOW** | `dhcpcd-hook.c:75` | No timeout on `waitpid` for `ip route` — can block dhcpcd hook |

### Regressions

| # | Severity | File | Issue |
|---|----------|------|-------|
| R1 | LOW | `init.cpp:run_amixer` | `strdup()` leak in parent process from fix `b3f28c3` |
| R2 | MEDIUM | `child_main.cpp:907,2739,2745` | `const_cast` UB on execv not fixed (fix `b3f28c3` only covered `init.cpp`) |

---

## PART 5: THINGS THE PRIOR REVIEWS GOT RIGHT

For completeness, the following areas were competently reviewed and no new issues were found beyond what was already documented:

1. **Process lifecycle** (REVIEW.md #1, #11, #16): The SIGCHLD race fix (`a8818c0`) and reboot fix (`c0ef01f`) are correct. No remaining races found.

2. **GPU compatibility** (REVIEW-HARDWARE.md #S1-S7): The VRAM detection fix (`82d505d`) and Intel Xe module loading (`5bc71e9`) correctly address the documented gaps. The remaining GPU backend compilation issue (no CUDA/Vulkan/ROCm) is a feature gap, not a code defect.

3. **Memory safety** (REVIEW.md #4, #14): The `SIZE_MAX` guard (`1b1a8a6`) is correctly implemented. No additional malloc truncation paths found.

4. **WiFi PSK handling** (REVIEW.md #8): The `mask_psk()` fix (`82ff6b4`) is correct. No other PSK leak paths found beyond those already documented.

5. **Auth infrastructure** (REVIEW.md #2, #7, #18): The `getrandom()` fix (`c1190c4`) and timing-safe comparison (`b9db981`) are correctly implemented. The remaining issue is the desktop mode bypass (N3 above).

6. **Ed25519 signatures, watchdog, A/B updates, GPT resizing**: All confirmed working as described in REVIEW.md "What's Working Well."
