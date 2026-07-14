# Llamaste — Tertiary Code Review

**Date**: 2026-07-14  
**Scope**: `/tmp/llamaste-review/src/llamaste/` (~5K LOC, C++17, 22 post-review commits)  
**Prior reviews**: REVIEW.md (primary, 24 issues), REVIEW-HARDWARE.md (supplemental, 7 issues), REVIEW-SECONDARY.md (verification + 10 new + 2 regressions)  
**Reviewer**: Automated tertiary agent review  
**Focus**: Fix verification, logic bugs, interaction bugs, boot/shutdown edge cases, dead code paths — anything all three prior reviews may have missed

---

## PART 1: FIX VERIFICATION SUMMARY

All 22 commits were spot-checked against the codebase. The secondary review's 15-verification table is confirmed correct. Additional fixes applied since the secondary review:

| Commit | What | Status |
|--------|------|--------|
| `2f2667f` | MCP key: `mt19937_64` → `getrandom()` | ✅ Fixed |
| `aadebf8` | HTTP body size limit: 10MB | ✅ Fixed |
| `7461929` | Desktop auth bypass → localhost-only | ✅ Fixed |
| `3b53ffb` | Child stderr tee → joinable + error checks | ✅ Fixed |
| `a4cb042` | `const_cast` UB on execv in child_main.cpp | ✅ Fixed (execv paths only) |
| `dc46d20` | CURL TLS verification enforced | ✅ Fixed |
| `b27c8b3` | dhcpcd-hook alarm(5) timeout | ✅ Fixed |

The `safe_spawn()` refactor (using `posix_spawnp`) correctly replaced all `fork()`+`execv()` calls in the llama-server, wpa_supplicant, and dhcpcd spawn paths. This addresses the secondary review's N4 (fork from multi-threaded context) by eliminating the fork entirely for these paths.

**Unfixed from prior reviews** (3 issues remain):

| Original ID | Issue | Current Status |
|-------------|-------|----------------|
| REVIEW.md #15 | `dispatch()` doesn't check `requires_confirmation` | ❌ Still unfixed |
| SECONDARY R1 | `strdup()` leak in `run_amixer` | ❌ Still unfixed |
| SECONDARY R2 (partial) | `const_cast` on `posix_spawnp` argv in `spawn_dhcpcd` | ⚠️ Not `execv` UB, but casts string literal/c_str to `char*` |

---

## PART 2: NEW FINDINGS — MISSED BY ALL THREE PRIOR REVIEWS

### T1 (HIGH) — WAV chunk parser infinite loop via uint32_t overflow

**File**: `voice.cpp:97-109`  
**Function**: `wav_to_float32()`

```cpp
while (pos + 8 <= wav_data.size()) {
    uint32_t chunk_size;
    memcpy(&chunk_size, wav_data.data() + pos + 4, 4);
    // ...
    pos += 8 + chunk_size;       // ← THE BUG
    if (pos % 2 != 0) pos++;
}
```

The expression `8 + chunk_size` is evaluated in `uint32_t` arithmetic (because `chunk_size` is `uint32_t`). When `chunk_size >= 0xFFFFFFF8`, the addition wraps to 0 (or a small value). `pos` does not advance, causing the loop to re-parse the same chunk indefinitely.

**Exploitation**: An attacker uploads a crafted 44-byte WAV file to `/llamaste/audio/transcribe`:

| Offset | Bytes | Meaning |
|--------|-------|---------|
| 0–3 | `RIFF` | RIFF header |
| 4–7 | 0x00000024 | file_size - 8 = 36 |
| 8–11 | `WAVE` | WAVE format |
| 12–15 | `JUNK` | Any non-"data" chunk ID |
| 16–19 | 0xFFFFFFF8 | Poisoned chunk size |
| 20–43 | arbitrary | Padding to 44 bytes |

When the parser reaches offset 12, `pos += 8 + 0xFFFFFFF8` wraps to `pos += 0` (in uint32_t), and `pos` (a 64-bit `size_t`) stays at 12. The loop never exits. The transcription request hangs, consuming 100% of one CPU core indefinitely.

The 10MB body size limit (fix `aadebf8`) is not a barrier — the exploit fits in 44 bytes.

**Impact**: Remote unauthenticated denial of service via crafted WAV upload. Process CPU starvation; multiple requests could exhaust all cores.

**Fix**: Break the `8 + chunk_size` addition into a `uint64_t` expression before adding to `pos`:

```cpp
pos += 8ULL + chunk_size;
```

### T2 (HIGH) — WAV zero-sample-rate division by zero → undefined behavior

**File**: `voice.cpp:90, 145-149`  
**Function**: `wav_to_float32()`

```cpp
int src_rate = hdr->sample_rate;  // attacker-controlled: can be 0
// ...
if (src_rate != target_sample_rate && src_rate > 0) {
    double ratio = (double)target_sample_rate / src_rate;  // DIVISION BY ZERO
    size_t new_len = (size_t)(num_samples * ratio);
    out_samples.resize(new_len);  // UB / OOM
}
```

When `sample_rate` in the WAV header is `0`, the guard `src_rate > 0` is false — so the `if` block is NOT entered. The code falls through to the `else` branch at line 160-162:

```cpp
} else {
    out_samples = std::move(raw);
}
```

Wait — re-reading: the `if` condition is `src_rate != target_sample_rate && src_rate > 0`. If `src_rate == 0` and `target_sample_rate == 16000`, then:
- `src_rate != target_sample_rate` → `0 != 16000` → **true**
- `src_rate > 0` → **false**
- `true && false` → **false**

So the `if` block is NOT entered. The `else` branch (line 160-162) runs instead, which just moves `raw` samples into `out_samples` without resampling. This is correct behavior — the guard prevents division by zero.

**Verdict**: NOT a bug. The `src_rate > 0` guard on line 145 correctly prevents division by zero. Retracted.

### T3 (MEDIUM) — Desktop mode root route (`/`) has unconditional auth bypass

**File**: `child_main.cpp:3309-3321`

```cpp
svr.Get("/", [](const httplib::Request& req, httplib::Response& res) {
    // Desktop mode: skip auth/setup gates — go straight to the main UI.
    // cog only accesses localhost so trust is implicit.
    if (g_boot_mode == "desktop") {
        // ... serve index.html immediately
        return;
    }
    // ... normal setup/login gates
});
```

The `require_auth` gate was correctly fixed (commit `7461929`) to only bypass for `127.0.0.1` / `::1`. However, the root route (`/`) has its OWN auth bypass in a separate handler, and this was NOT updated. It still bypasses ALL auth and setup gates unconditionally for any remote_addr when `g_boot_mode == "desktop"`.

The impact is lower than the original N3 because:
- The root route only serves the SPA (index.html)
- API calls from the SPA would still fail authentication for non-localhost clients
- An attacker gets the UI shell but no data or tool execution

However, the inconsistency is confusing and the SPA exposes UI details (model name, system info from static HTML, etc.).

**Impact**: Information disclosure of UI assets. Inconsistent with the `require_auth` desktop fix.

**Fix**: Add the same `remote_addr` check as the `require_auth` fix, or serve a different page for remote clients.

### T4 (MEDIUM) — `dispatch()` still doesn't enforce `requires_confirmation`

**File**: `tools.cpp:23-42`

```cpp
std::string ToolRegistry::dispatch(const std::string& name,
                                    const std::string& args_json) const {
    auto it = tools_.find(name);
    if (it == tools_.end()) { /* error */ }
    try {
        return it->second.handler(args_json);  // NO confirmation check
    } catch (...) { /* error */ }
}
```

The `needs_confirmation()` method exists and returns the correct value (line 92-96), but `dispatch()` never calls it. REVIEW.md #15 flagged this as MEDIUM. It was never fixed.

The confirmation gate is currently enforced only at the HTTP route level (the `/llamaste/tool` handler checks `requires_confirmation` before calling `dispatch()`). Any code path that calls `dispatch()` directly — programmatic agent invocations, cluster RPC, future automation — will execute shutdown/reboot/format tools without confirmation.

**Impact**: No immediate exploit path exists (HTTP routes check confirmation separately), but any future caller of `dispatch()` inherits this gap. Defense-in-depth failure.

**Fix**: Add a `needs_confirmation` check inside `dispatch()` that returns an error, or make `dispatch()` take a confirmation flag parameter.

### T5 (MEDIUM) — `dhcpcd-hook.c` alarm(5) timeout is not the full fix for `ip route add`

**File**: `dhcpcd-hook.c:77-81, 84-97`

```cpp
// Delete old default route — has alarm(5) ✓
pid = fork();
if (pid == 0) {
    execl("/sbin/ip", "ip", "route", "del", "default", (char*)NULL);
    _exit(0);
}
if (pid > 0) {
    alarm(5);
    waitpid(pid, NULL, 0);
    alarm(0);
}

// Add new default route — NO alarm() timeout ✗
pid = fork();
if (pid == 0) {
    execl("/sbin/ip", "ip", "route", "add", "default",
          "via", gateway, "dev", ifname, (char*)NULL);
    _exit(1);
}
if (pid > 0) {
    int st = 0;
    waitpid(pid, &st, 0);  // BLOCKS INDEFINITELY — no alarm()
    // ...
}
```

The secondary review's N10 identified the missing timeout on `waitpid` for `ip route del`. Commit `b27c8b3` added `alarm(5)` — but ONLY for the `ip route del` block. The `ip route add` block at line 84-97, executed immediately after, still has NO timeout. If `/sbin/ip route add` hangs (kernel route table lock contention, NFS failure, etc.), the dhcpcd hook blocks forever.

**Impact**: dhcpcd hook hangs on route addition, dhcpcd may timeout waiting for its hook, DHCP renewal fails, network connectivity lost after lease expiry.

**Fix**: Add `alarm(5)` to the `ip route add` fork block as well. Or extract both fork+exec+waitpid patterns into a single helper with timeout.

### T6 (LOW) — `const_cast` in `spawn_dhcpcd` argv for `posix_spawnp`

**File**: `child_main.cpp:1297-1302`

```cpp
char* const argv[] = {
    const_cast<char*>("dhcpcd"),
    const_cast<char*>("-b"),
    const_cast<char*>(iface.c_str()),   // casting away const from std::string::c_str()
    nullptr
};
pid_t pid = safe_spawn("dhcpcd", argv);
```

Commit `a4cb042` correctly removed `const_cast<char**>` from all `execv` call sites. But `spawn_dhcpcd` uses `posix_spawnp` (via `safe_spawn`), not `execv`, so it was not in scope. The `const_cast` remains:
- `const_cast<char*>("dhcpcd")` — casting string literal to `char*`
- `const_cast<char*>(iface.c_str())` — casting `const char*` from `std::string` to `char*`

POSIX guarantees `posix_spawnp` does not modify `argv` elements, so this is safe in practice — but the same was true of `execv`. The inconsistency with the `execv` fix is worth noting.

**Fix**: Either accept the risk (posix_spawnp doesn't modify argv) or use `strdup` consistently, or use `std::vector<char*>` as done in `spawn_llama_server`.

### T7 (LOW) — `g_is_live_iso` should be a global but is shadowed by a local

**File**: `child_main.cpp:2067`

```cpp
bool g_is_live_iso = (access("/cdrom/llamaste-live-iso", F_OK) == 0);
```

The `g_` prefix convention is used for global variables throughout the codebase (`g_running`, `g_boot_mode`, `g_hwinfo`, etc.). This variable is declared inside `child_main()` as a local — the `g_` prefix is misleading. The variable is only used in the immediate few lines (2067-2072), so it functions correctly, but a future maintainer might assume it's accessible from other functions.

**Fix**: Rename to `is_live_iso` (local) or move to file scope if needed elsewhere.

### T8 (LOW) — `voice.cpp` PIMPL pattern with raw `new`/`delete`, no `unique_ptr`

**File**: `voice.cpp:205-233`

```cpp
VoicePipeline::VoicePipeline() : impl_(new Impl) {}
VoicePipeline::~VoicePipeline() {
    stop();
    // ...
    delete impl_;
}
```

The destructor calls `stop()` which acquires `config_mutex_`. If `stop()` throws (unlikely but possible if the mutex is corrupted), `impl_` is never deleted — memory leak. More importantly, if an exception is thrown during construction after `impl_ = new Impl`, the memory leaks because there's no `unique_ptr` to clean up.

This is the only review to examine `voice.cpp` in detail (REVIEW.md #24 explicitly deferred it).

**Impact**: Theoretical memory leak on exception — unlikely in practice for this embedded appliance, but a code smell in modern C++.

**Fix**: Replace raw `Impl*` with `std::unique_ptr<Impl>`. Move `stop()` before `delete` is fine with `unique_ptr` semantics.

---

## PART 3: INTERACTION AND EDGE-CASE ANALYSIS

### 3.1 Supervisor → Child shutdown interaction — CORRECT

When the child sends `SIGTERM` (shutdown) or `SIGUSR1` (reboot) to PID 1:

1. Supervisor signal handler sets `g_shutdown_requested = 1` and kills child with `SIGTERM`
2. Child's `child_signal` sets `g_running = false` (atomic)
3. Child's `shutdown_watcher` thread calls `svr.stop()`, unblocking `svr.listen()`
4. Child cleanup: kills RPC server, llama-server, joins tee thread, returns
5. Supervisor's main loop sees `g_shutdown_requested`, breaks out
6. Supervisor kills child again (harmless — already dead), joins tee thread, sync, unmount, reboot

The interaction is correct. No races found.

### 3.2 Supervisor ethernet pre-flight + Child ethernet init — CORRECT

The supervisor's `supervisor_preflight_ethernet()` spawns dhcpcd with `-b` on wired interfaces. The child's ethernet init later checks `already_has_ip` via `SIOCGIFADDR` ioctl and skips re-spawning dhcpcd if an IP is already configured. If the supervisor's dhcpcd died, the check still works (IP remains configured until release). No conflict.

### 3.3 Boot sequence: watchdog starts BEFORE WiFi pre-flight — CORRECT

The watchdog kicker thread starts at `supervisor_run` line 1651, BEFORE `supervisor_preflight_ethernet()` and `supervisor_preflight_wifi()` (lines 1661-1662). This addresses REVIEW.md #16. The kicker runs every 1 second with all signals blocked. Correct.

### 3.4 llama-server monitor thread during shutdown — LOW RISK

The monitor thread (detached at child_main.cpp:2133) may be mid-respawn when `g_running` becomes false. It checks `g_running.load()` before spawning and breaks out (lines 1355, 1363). However, there's a narrow window where the monitor spawns llama-server between the cleanup code killing it and `child_main` returning. The spawned process becomes an orphan that init (PID 1) reaps. No zombie — the supervisor's SIGCHLD handler uses `waitpid(g_child_pid, ...)`, so orphan llama-server processes are reaped by PID 1 without issue. This is correct behavior.

### 3.5 `posix_spawnp` with `environ` — CORRECT

`safe_spawn` at line 846 passes `environ`:
```cpp
int ret = posix_spawnp(&pid, path, file_actions, attrp, argv, environ);
```

`environ` is declared in `<unistd.h>` on POSIX systems and is the correct environment to pass. No issue.

### 3.6 CORS headers after fix — CORRECT

The `Access-Control-Allow-Origin: *` was removed (fix N9). The OPTIONS handler still returns 204 for all preflight requests, but without the Allow-Origin header, cross-origin requests are properly rejected by browser same-origin policy. Same-origin access (the only legitimate use case) is unaffected. Correct.

---

## PART 4: VERIFIED — NO ISSUES FOUND

The following areas were re-examined in detail and no defects were found:

1. **Ed25519 signature verification** — `verify_update_signature()` uses vendored tweetnacl correctly
2. **A/B update system** — `install_update()` flow: parse → version check → verify signature → verify SHA-256 → write. All guards present.
3. **Watchdog architecture** — kicker thread with blocked signals, O_CLOEXEC, 300s timeout. Correct.
4. **GPT partition resizing** — CRC32 recomputation, backup GPT, PMBR, BLKPG refresh. Correct.
5. **Live ISO pivot** — `/cdrom/llamaste-live-iso` marker correctly guards double-pivot.
6. **WiFi PSK masking** — confirmed correct in both `spawn_wpa_supplicant` diagnostics and `wpa()` error path.
7. **Brute-force protection** — IP-based cooldown (5 failures → 30s lockout) verified.
8. **GPU module loading order** — amdgpu dependencies loaded in correct sequence.
9. **GPU VRAM detection** — `mem_info_vram_total` (AMD) and `total_vram` (Intel) paths confirmed.
10. **Root block device discovery** — `/proc/mounts` parsing handles NVMe (`pN`), mmcblk, SATA/VirtIO correctly.
11. **`safe_spawn` strdup cleanup** — `spawn_llama_server` correctly frees strdup'd argv after `safe_spawn` returns (line 934-935).
12. **Cluster election** — `run_election()` calls topology callback outside lock. Correct.

---

## PART 5: SUMMARY TABLE — NEW FINDINGS

| # | Severity | File(s) | Issue |
|---|----------|---------|-------|
| T1 | **HIGH** | `voice.cpp:106` | WAV chunk parser infinite loop via `uint32_t` overflow in `pos += 8 + chunk_size` |
| T2 | ~~HIGH~~ | ~~`voice.cpp:145`~~ | RETRACTED — `src_rate > 0` guard correctly prevents division by zero |
| T3 | **MEDIUM** | `child_main.cpp:3312` | Desktop root route (`/`) unconditional auth bypass without remote_addr check |
| T4 | **MEDIUM** | `tools.cpp:32` | `dispatch()` doesn't enforce `requires_confirmation` — REVIEW.md #15 unfixed |
| T5 | **MEDIUM** | `dhcpcd-hook.c:84-97` | `ip route add` fork has no timeout — only `ip route del` was fixed |
| T6 | **LOW** | `child_main.cpp:1299` | `const_cast` on `posix_spawnp` argv not removed (only `execv` casts fixed) |
| T7 | **LOW** | `child_main.cpp:2067` | `g_is_live_iso` is a local variable — misleading `g_` prefix |
| T8 | **LOW** | `voice.cpp:205,233` | PIMPL with raw `new`/`delete` — not exception-safe |

### Previously reported, still unfixed

| Original ID | Severity | Issue |
|-------------|----------|-------|
| REVIEW.md #15 | MEDIUM | `dispatch()` doesn't check `requires_confirmation` (= T4 above) |
| SECONDARY R1 | LOW | `strdup()` leak in `run_amixer` (init.cpp:778-798) |
| SECONDARY R2 partial | LOW | `const_cast` on `posix_spawnp` argv in `spawn_dhcpcd` (= T6 above) |

---

## PART 6: SUMMARY

**The codebase is in good shape after 22 rounds of fixes.** All critical and high-severity findings from the prior reviews have been competently addressed. The tertiary review found:

- **1 new HIGH-severity bug**: WAV chunk parser infinite loop (T1) — a genuine DoS vector in the voice transcription path that all three prior reviews missed. This is the only finding requiring immediate attention.
- **3 MEDIUM issues**: inconsistent desktop auth bypass at root route, unfixed dispatch confirmation gate, and missing timeout on `ip route add` in dhcpcd hook.
- **4 LOW issues**: const_cast cleanup, misleading variable name, exception-unsafe PIMPL, and the unfixed strdup leak.

No interaction bugs between fixes were found. The boot sequence, shutdown path, and recovery logic are architecturally sound. The signal handling between supervisor and child is correct.
