# Llamaste C++ Codebase — Security & Correctness Review

**Date**: 2026-07-13  
**Scope**: Full codebase at `/tmp/llamaste-review/src/llamaste/` (~24K LOC)  
**Focus**: Process lifecycle, memory safety, concurrency, auth/security, Strix Halo GPU  
**Reviewer**: Automated agent review

---

## CRITICAL

### 1. `tools_system.cpp:258-275` — `reboot()` called from child process, not PID 1

```cpp
// tools_system.cpp:266
int ret = reboot(RB_POWER_OFF);  // system.shutdown
```

The `system.shutdown` and `system.reboot` tools call `reboot()` directly. The comment at line 264 states _"As PID 1, we can call reboot()"_ — but these tools execute in the **child process** (`child_main`, forked by supervisor), not in the PID 1 supervisor. The `reboot()` syscall requires `CAP_SYS_BOOT` and typically PID 1. In the child process, this will return `-EPERM` and log a misleading error while the system keeps running **without any actual shutdown**. The tool handler reports success ("shutdown initiated") despite the call failing.

**Impact**: LLM-initiated or API-triggered shutdown/reboot silently fails. The tool lies about success.

**Fix**: These tools must signal the supervisor (e.g., via `SIGTERM`/`SIGUSR1` or a Unix socket message) rather than calling `reboot()` directly. The supervisor (actual PID 1) already handles shutdown via `supervisor_sigterm`/`supervisor_sigusr1`.

---

### 2. `auth.cpp:32-57` — Cryptographic token generation has weak fallback

```cpp
// auth.cpp:41-45
int fd = open("/dev/urandom", O_RDONLY);
if (fd < 0) {
    // Fallback: use time-based seed (weak, but better than nothing)
    srand((unsigned)time(nullptr));
    for (int i = 0; i < bytes; i++) buf[i] = rand() & 0xff;
}
```

When `/dev/urandom` is unavailable (e.g., early boot before devtmpfs mounts), session tokens and API keys fall back to `rand()` seeded with `time()`. This produces **entirely predictable** tokens. An attacker who knows the approximate boot time can trivially enumerate all possible session tokens. The `read()` return value is also not checked — a short read produces a partially uninitialized buffer mixed with random data.

**Impact**: Session hijacking via predictable tokens. API key compromise if generated during early boot before `/dev/urandom` is available (e.g., first-boot password setup in `supervisor_preflight_wifi`).

**Fix**: Block `generate_token()` until `/dev/urandom` is available. Check `read()` return value. Consider `getrandom()` syscall as primary with `/dev/urandom` fallback.

---

### 3. `supervisor.cpp:121-161` — Detached stderr tee thread leaks FDs, no error recovery

```cpp
// supervisor.cpp:137-160
std::thread([read_fd, original_console, log_file]() {
    char buf[512];
    std::string partial;
    while (true) {
        ssize_t n = read(read_fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        if (original_console >= 0)
            write(original_console, buf, n);   // no error check
        if (log_file >= 0)
            write(log_file, buf, n);            // no error check
        partial += buf;
        // ...
    }
}).detach();
```

The thread is detached (never joined). If `write()` to the console or log file fails (e.g., ENOSPC on `/tmp`), errors are silently swallowed. If the pipe breaks, the thread exits without cleanup notification. On fast restarts, orphaned tee threads and file descriptors accumulate.

**Impact**: Silent data loss in debug logging. FD leak on crash/restart cycles. Debug log (`/tmp/llamaste-debug.log`) can silently stop recording.

**Fix**: Check `write()` return values. Replace detached thread with a joinable thread managed alongside the kicker thread.

---

### 4. `updater.cpp:221-244` — Integer overflow in `malloc`, unsafe truncation of file size

```cpp
// updater.cpp:225-238
fseek(f, 0, SEEK_END);
long file_size = ftell(f);
if (file_size < 0 || (uint64_t)file_size <= offset) { ... }
uint64_t data_size = (uint64_t)file_size - offset;
unsigned char* buf = (unsigned char*)malloc((size_t)data_size);  // BUG: truncation
```

`ftell()` returns `long`, which on 32-bit platforms is 32 bits. Even on 64-bit Linux, the conversion chain `long → uint64_t → size_t` has issues: `malloc` takes `size_t` which could be smaller than `uint64_t` on some platforms. If `data_size > SIZE_MAX`, `malloc` silently allocates much less memory than needed, leading to a heap buffer overflow on `fread`. Additionally, the `.update` file format supports payloads up to 4GiB (`uint32_t` manifest size), but a maliciously crafted file could exploit this truncation.

**Impact**: Heap buffer overflow via crafted `.update` file. Potential arbitrary code execution during OTA update processing.

**Fix**: Check `data_size <= SIZE_MAX` before calling `malloc`. Use `fseek`/`ftell` with large file support (#define `_FILE_OFFSET_BITS 64`) or `stat()` for file size.

---

## HIGH

### 5. `child_main.cpp:37-39` — No HTTPS; all traffic including passwords in cleartext

```cpp
// child_main.cpp:36-39
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "httplib.h"
```

SSL support is **explicitly disabled**. All HTTP routes — including `/llamaste/auth/login` (password submission), `/llamaste/auth/setup` (first-boot password set), and Bearer token API endpoints — transmit credentials in cleartext. The `set_session_cookie` (auth.cpp:287-293) sets `HttpOnly` and `SameSite=Lax` but **no `Secure` flag** because the server doesn't support TLS.

**Impact**: Password sniffing on any network between client and device. Session cookie theft via passive network monitoring.

**Fix**: Enable `CPPHTTPLIB_OPENSSL_SUPPORT` and generate a self-signed certificate at first boot. Set `Secure` flag on session cookies. Alternatively, document that this is a local-network-only device and the web UI is only accessible over trusted LANs.

---

### 6. `supervisor.cpp:386-427` — `http_post_localhost` embeds unsanitized data in HTTP request

```cpp
// supervisor.cpp:401-409
int reqlen = snprintf(req, sizeof(req),
    "POST %s HTTP/1.0\r\n"
    "Host: localhost\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: %zu\r\n"
    "\r\n"
    "%s",
    path, body.size(), body.c_str());
```

The password JSON body (containing the new password in plaintext) is embedded directly into an HTTP/1.0 request via `snprintf`. No encoding or special-character escaping. While this specific path is `/llamaste/auth/setup`, other uses of this helper could be exploited. More critically, `snprintf` is **not checked for truncation** — if the body exceeds ~1800 bytes, the request is silently truncated, producing a malformed HTTP request.

**Impact**: Truncated password setup requests silently fail. Potential HTTP request smuggling if body contains `\r\n\r\n`.

**Fix**: Check `snprintf` return value. Use `write()` directly for the body after headers, rather than embedding via format string.

---

### 7. `auth.cpp:229-234` — API key comparison is "constant-time" but not timing-safe

```cpp
// auth.cpp:229-234
if (key.size() != api_key_.size()) return false;  // LEAKS length!
volatile int diff = 0;
for (size_t i = 0; i < key.size(); i++) {
    diff |= key[i] ^ api_key_[i];
}
return diff == 0;
```

The early return on length mismatch leaks the API key length. While the comparison loop uses `volatile` to discourage compiler optimization, modern C++ compilers may still optimize away the `volatile` on local variables (C++ standard: `volatile` only prevents elision of reads/writes but does not guarantee ordering or prevent dead-store elimination of `diff`).

**Impact**: Timing side-channel leaks API key length; theoretical key recovery via byte-at-a-time timing attack.

**Fix**: Use `CRYPTO_memcmp` (OpenSSL) or `sodium_memcmp` (libsodium), or at minimum, always loop 32 bytes and accumulate diff without early exit.

---

### 8. `wifi.cpp:469-476` — PSK written to stderr in `connect()` path

```cpp
// wifi.cpp:469-475 (the cmd string contains SSID and PSK)
if (str_trim(wpa("SET_NETWORK " + id + " ssid \"" + ssid + "\"")) != "OK")
    return "SET_NETWORK ssid failed";
if (psk.empty()) {
    // ...
} else {
    if (str_trim(wpa("SET_NETWORK " + id + " psk \"" + psk + "\"")) != "OK")
```

While the PSK is not explicitly logged, the `wpa()` function is called with commands containing the PSK. If `send()` fails (line 257-260), `fprintf(stderr, ...)` logs the entire command **including the PSK**:

```cpp
// wifi.cpp:257-259
if (send(ctrl_fd_, cmd.c_str(), cmd.size(), 0) < 0) {
    fprintf(stderr, "[wifi] send(%s) failed: %s\n",
            cmd.c_str(), strerror(errno));  // LEAKS PSK
```

And general log storage at `/tmp/llamaste-debug.log` makes this persistent.

**Impact**: WiFi PSK leaked to debug logs, readable via the `/debug/dmesg` HTTP endpoint.

**Fix**: Mask PSK in error log paths. Consider a separate `wpa_safe()` that takes sensitive params separately.

---

### 9. `child_main.cpp` — 4835-line god-object: unmaintainable, untestable

The file `child_main.cpp` is 4835 lines. It contains:
- HTTP server initialization and route registration
- llama-server lifecycle (fork/exec/monitor)
- Inference pipeline (chatml formatting, grammar retry, semantic cache)
- Desktop compositor launch (cage/labwc/weston fallback chain)
- WiFi daemon management (wpa_supplicant, dhcpcd, route/DNS)
- Session expiry thread
- Bluetooth daemon management
- `/debug/drm` diagnostic endpoint
- All system metrics collection
- Stub inference fallback
- File serving and embedded resource handling
- Conversation persistence

This violates single-responsibility principle and makes the codebase extremely fragile. Changing the compositor launch code risks breaking inference. Changing WiFi setup risks breaking session management. There are **no unit tests** for any of the compositor launch logic because it's all inside the 4835-line monolith.

**Recommendation**: Split into `http_server.cpp`, `inference_backend.cpp`, `compositor_launch.cpp`, `wifi_daemon.cpp`, `child_init.cpp`. Target <500 lines per file.

---

## MEDIUM

### 10. `bcrypt.cpp` — Custom cryptographic implementation with no external vetting

The entire file (601 lines) implements bcrypt from scratch: the Blowfish block cipher, Eksblowfish key schedule, custom base64 encoding/decoding, and the bcrypt hash format. This is security-critical code with no external audit. Specific concerns:

- `bf_base64_decode` (line 444-462): Does not validate input length. A truncated salt string produces an incorrectly-sized buffer.
- `bf_expand_key` (line 365-395): The salt-stream index wraps via modulo — this matches the OpenBSD reference but is a subtle behavior that's easy to get wrong.
- No test vectors for the `$2b$` format are shipped with the code; verification against known bcrypt hashes is not automated.

**Recommendation**: Replace with libsodium's `crypto_pwhash_str` or OpenSSL's `EVP_PBE_scrypt`/bcrypt. The vendored tweetnacl already provides `crypto_sign_ed25519` — adding `crypto_pwhash` from the same library eliminates all custom hash code.

---

### 11. `supervisor.cpp:163-178` — Signal handler / waitpid race condition

```cpp
static void supervisor_sigchld(int) {
    g_child_exited = 1;   // sig_atomic_t
}
```

The signal handler sets `g_child_exited` asynchronously. The main loop (line 1566-1631) reads this flag. Between the signal firing and `waitpid()` being called, the flag is set but the child's PID is not reaped. If:
1. SIGCHLD fires → `g_child_exited = 1`
2. Main loop sees `g_child_exited != 0` and exits the wait loop
3. Before `waitpid()` is called again, some other signal arrives
4. `waitpid()` could theoretically block if the SIGCHLD delivery race causes the child to be missed

This is **mitigated in practice** by SA_RESTART and the fact that `waitpid()` is run inside the same loop iteration, but the design is fragile.

**Fix**: Use `signalfd()` or `pselect()`/`ppoll()` with explicit signal masking instead of async signal handlers for child lifecycle management.

---

### 12. `init.cpp:424-428`, `607-609`, `779-783`, `1359-1361` — `execv` const-correctness violation

```cpp
// init.cpp:425-428 (throughout)
execl("/usr/sbin/mkfs.ext4", "mkfs.ext4", "-F", "-q",
      "-L", "LLAMASTE-DATA", part_dev, nullptr);
```

And similarly with `execv`:

```cpp
// init.cpp:609
const char* argv[] = {mkfs, "-q", "-F", "-L", "DATA", dev, nullptr};
execv(mkfs, (char* const*)argv);  // casting away const
```

The cast `(char* const*)` from `const char* const*` is technically undefined behavior per POSIX (the exec family takes `char *const[]` but guarantees not to modify arguments). This compiles but is undefined behavior in C++; a standards-compliant compiler could optimize incorrectly around this cast.

**Fix**: Use `char*` arrays without `const`, or use a wrapper that copies to non-const arrays.

---

### 13. `updater.cpp:397-418` — NVMe partition detection misses `/dev/nvme0n1` (no trailing 'p')

```cpp
// updater.cpp:402-404
const char* candidates[] = {
    "/dev/sda", "/dev/vda", "/dev/nvme0n1p"
};
```

The base path `/dev/nvme0n1p` + `part_num` produces `/dev/nvme0n1p3` which is syntactically correct **only if** the trailing 'p' is intentional as the NVMe partition prefix. However, some NVMe devices expose partitions as `/dev/nvme0n1p1` through the kernel but `/dev/nvme0n1` is the base block device — the 'p' serves as the partition separator for the first device. On multi-NVMe systems, the correct base could be `/dev/nvme1n1` or `/dev/nvme0n2`. The static candidate list won't find these.

**Impact**: OTA updates fail silently on non-sda/vda/nvme0n1 systems. "cannot find block device for partition" error.

**Fix**: Dynamically discover the root block device from `/proc/mounts` or `/proc/cmdline` instead of hardcoding device names.

---

### 14. `init.cpp:107-122` — `rlog` has TOCTOU race on `resize_log` initialization

```cpp
static FILE* resize_log = nullptr;
static void rlog(const char* fmt, ...) {
    // ...
    if (!resize_log) resize_log = fopen("/tmp/init-resize.log", "w");  // RACE
    if (resize_log) {
        // ...
    }
}
```

Two threads calling `rlog` could both see `resize_log == nullptr` and both call `fopen`, leaking one FILE*. In practice, init runs single-threaded during early boot, so this is theoretical but present.

---

### 15. `tools_system.cpp:258-274` — Shutdown/reboot tools have no confirmation gate in tool dispatch

The tools register with `.requires_confirmation = true`, but the `dispatch()` method in `tools.cpp` does not check this flag — it calls the handler directly. The confirmation check must be done by the HTTP route handler **before** calling dispatch. If any code path calls `dispatch()` without the confirmation check (e.g., programmatic agent invocations), shutdown/reboot executes immediately.

---

### 16. `supervisor.cpp:1491-1515` — WiFi scan waits up to 6s blocking the entire boot

```cpp
for (int try_n = 0; try_n < 6; try_n++) {
    iface = supervisor_detect_wifi_iface();
    if (!iface.empty()) break;
    usleep(1000000); // 1s per retry, up to 6s total
}
```

This blocks the supervisor's main loop from starting for up to 6 seconds. During this time, the watchdog kicker thread hasn't started yet (it starts at line 1548, after `supervisor_preflight_wifi` returns). If driver probing takes >60s total (including the subsequent scan), the hardware watchdog fires.

**Impact**: Boot-time watchdog reset on systems with slow WiFi driver initialization.

**Fix**: Start the watchdog kicker thread before the WiFi pre-flight scan.

---

### 17. `child_main.cpp:2061` — Free RAM estimate subtracts 500MB for compositor unconditionally

```cpp
if (config.boot_mode == "desktop") free_ram_estimate -= 500;  // reserve for compositor
```

This estimate is passed to `compute_context_size()` which determines the LLM context window. On 4GB RAM devices running desktop mode, `free_ram_estimate` becomes negative, and `compute_context_size` falls through to the minimum 2048-token context. The comment says "reserve 500MB for compositor" but the actual cage+cog compositor uses ~100MB — the overestimate unnecessarily starves the LLM of context.

---

## LOW

### 18. `auth.cpp:287-293` — Session cookie missing `Secure` flag (already noted in HIGH #5)

```cpp
snprintf(cookie, sizeof(cookie),
         "llamaste_sid=%s; HttpOnly; SameSite=Lax; Path=/; Max-Age=%d",
         token.c_str(), session_timeout_seconds_);
```

No `Secure` flag. Acceptable only if HTTPS is never going to be supported, but if SSL is ever enabled, these cookies need updating.

---

### 19. `init.cpp:65-73` — CRC32 for GPT partitioning is non-cryptographic (by design, but worth noting)

CRC32 is used for GPT header/partition array integrity checking. This is standard (per UEFI spec) and not a bug, but GPT's reliance on CRC32 means malicious physical disk modification is undetectable — CRC32 collisions are trivial to compute. This is a limitation of the GPT spec itself, not the implementation.

---

### 20. `wifi.cpp` / `supervisor.cpp` — Duplicate `iw scan` parsing logic (DRY violation)

`supervisor_scan_wifi()` (supervisor.cpp:1013-1283) and `WiFiManager::iw_scan()` (wifi.cpp:658-800) contain nearly identical BSS-parsing logic (~150 lines of duplicated code). Any fix to BSS parsing (e.g., handling hidden SSIDs, 6GHz support) must be applied in two places.

**Fix**: Extract shared parsing into a single function in `wifi.cpp` that both callers use.

---

### 21. `child_main.cpp:2897-3119` — Compositor launch thread is detached, no crash reporting

```cpp
std::thread([]() {
    // ... 200+ lines of compositor launch logic
}).detach();
```

If the compositor launch thread crashes (unhandled exception), the entire child process terminates with `std::terminate` — no diagnostic beyond the kernel's default core dump. Since the HTTP server runs in the same process, this kills all active connections.

---

### 22. `bcrypt.cpp:597-599` — Constant-time comparison doesn't loop full length

```cpp
for (int i = 0; i < 31 && computed_b64[i] && expected[i]; i++) {
    diff |= computed_b64[i] ^ expected[i];
}
```

The loop stops at the first null byte in either string. If the computed hash has an embedded null (unlikely due to base64 encoding, but possible if the bcrypt implementation has a bug), the comparison would be trivially shorter and easier to timing-attack. The correct approach is to always iterate all 31 bytes.

---

### 23. `agent.cpp:20-21` — Global `call_counter` across all conversations

```cpp
static std::atomic<int> call_counter{0};
```

Tool call IDs are globally sequential across all conversations. This leaks information about total tool usage across sessions and creates unnecessary coordination (atomic increment). A per-conversation counter would be simpler.

---

### 24. `voice.cpp` — Not reviewed (voice I/O pipeline)

Did not have time to review in detail. The voice pipeline involves ALSA capture, VAD, and Whisper.cpp integration — a potential attack surface if audio buffer handling has bounds issues.

---

## Strix Halo GPU Fix Assessment

### `amdgpu.modeset=0` — Correct but aggressive

The fix in commit `88e891c` adds `amdgpu.modeset=0` to the kernel command line for both server and desktop boot modes (grub.cfg line 37, 46). This is the **right call** for the Strix Halo GPU on current kernels (6.18.21 LTS) where the amdgpu driver causes boot hangs during modesetting probe. However:

- **Server mode**: `amdgpu.modeset=0` is fine — no display output needed, GPU is used for compute only (ROCm/HIP via HSA/KFD).
- **Desktop mode**: `amdgpu.modeset=0` **prevents the desktop compositor from using the dedicated GPU at all**. The system falls back to simpledrm (EFI framebuffer) and the pixman software renderer. The cage compositor's check at child_main.cpp:2934-2937 forces `WLR_RENDERER=pixman`, which is correct for simpledrm but means the amdgpu is **completely unused for display**. This is documented in the code comments (line 2932: "TODO: Re-enable GPU-accelerated rendering once Mesa GL is verified working").

**Assessment**: The fix correctly prevents the boot hang on Strix Halo. The desktop mode limitation is documented as a TODO. Once Mesa iris/radeonsi DRI drivers are verified in the Buildroot build, `amdgpu.modeset=1` can be restored for desktop mode.

### VCN 4.0.6 firmware — Correct fix

Commit `f8f48ff` updates VCN firmware from 4.0.5 to 4.0.6 for Strix Halo. This is correct — VCN 4.0.5 is for RDNA3 (Navi 3x), while Strix Halo uses RDNA3.5 which needs VCN 4.0.6.

### GPU module loading order — Correct

`init.cpp:1337-1351` loads GPU modules in correct dependency order:
1. `gpu-sched.ko` → `drm_exec.ko` → `drm_suballoc_helper.ko`
2. `ttm.ko` → `drm_buddy.ko` → `drm_ttm_helper.ko`
3. `amdxcp.ko` → `amdgpu.ko`
4. `nouveau.ko` (independent, loaded last)

The use of `finit_module()` with `EEXIST` handling is correct. Module loading is conditional on the file existing (`access()` check), so missing modules don't cause boot failures.

### Kernel 6.18.21 LTS — Good choice

Strix Halo (GFX1151) support matured significantly between 6.12 and 6.18. The jump to 6.18.21 LTS is appropriate for production use with the caveats noted above.

---

## Summary Table

| # | Severity | File | Issue |
|---|----------|------|-------|
| 1 | CRITICAL | `tools_system.cpp:266` | `reboot()` called from child, not PID 1 — shutdown silently fails |
| 2 | CRITICAL | `auth.cpp:41-45` | `/dev/urandom` failure → predictable `rand()` tokens |
| 3 | CRITICAL | `supervisor.cpp:137` | Detached thread leaks FDs, swallows write errors |
| 4 | CRITICAL | `updater.cpp:236` | `malloc(size_t)` truncation → heap overflow on OTA |
| 5 | HIGH | `child_main.cpp:37` | SSL explicitly disabled, credentials in cleartext |
| 6 | HIGH | `supervisor.cpp:401` | Password embedded unsanitized in raw HTTP |
| 7 | HIGH | `auth.cpp:229` | API key comparison not truly constant-time |
| 8 | HIGH | `wifi.cpp:257` | PSK logged to stderr on send failure |
| 9 | HIGH | `child_main.cpp` | 4835-line god-object, zero testability |
| 10 | MEDIUM | `bcrypt.cpp` | Custom bcrypt with no test vectors |
| 11 | MEDIUM | `supervisor.cpp:163` | SIGCHLD/waitpid race condition |
| 12 | MEDIUM | `init.cpp` (multiple) | `const_cast` on exec args — UB |
| 13 | MEDIUM | `updater.cpp:397` | Hardcoded device paths miss NVMe |
| 14 | MEDIUM | `init.cpp:107` | TOCTOU race on `resize_log` init |
| 15 | MEDIUM | `tools_system.cpp` | No dispatch-level confirmation gate |
| 16 | MEDIUM | `supervisor.cpp:1495` | WiFi scan blocks before watchdog starts |
| 17 | MEDIUM | `child_main.cpp:2061` | Excess compositor RAM reserve |
| 18 | LOW | `auth.cpp:287` | Cookie missing `Secure` flag |
| 19 | LOW | `init.cpp:65` | GPT CRC32 (by spec, not a bug) |
| 20 | LOW | `wifi.cpp` | Duplicate BSS parsing code |
| 21 | LOW | `child_main.cpp:2988` | Detached compositor thread |
| 22 | LOW | `bcrypt.cpp:597` | Short-circuit constant-time compare |
| 23 | LOW | `agent.cpp:20` | Global call counter leaks usage info |

---

## What's Working Well

1. **Ed25519 signature verification** for OTA updates is correctly implemented using vendored tweetnacl. Manifest is signed, payload SHA-256 is verified against manifest, and version downgrade is prevented.

2. **Watchdog architecture** is well-designed: dedicated kicker thread with all signals blocked, O_CLOEXEC on watchdog fd, and defense-in-depth timeout (300s) even though kick happens at 100ms intervals.

3. **A/B update system** with atomic grubenv writes (write-to-tmp-then-rename), partition validation before writing, and boot counter mechanism for rollback.

4. **Strix Halo GPU fixes** are correctly implemented — the `modeset=0` workaround prevents boot hangs, VCN firmware is the right version, and module loading order is correct. The desktop mode software-rendering limitation is properly documented with a TODO.

5. **WiFi PSK masking** in the `spawn_wpa_supplicant` diagnostic log (child_main.cpp:1022-1023) shows awareness of credential safety — though the `wpa()` error path still leaks (see #8).

6. **Brute-force protection** (auth.cpp:302-328) with IP-based cooldown (5 failures → 30s lockout) is solid for a local-network device.

7. **GPT partition resizing** (init.cpp:127-339) correctly handles CRC32 recomputation, backup GPT, PMBR update, and kernel table refresh via BLKPG.

8. **Live pivot** (init.cpp:1141-1265) correctly guards against double-pivot with the `/llamaste-live-iso` marker file.