# Research 17: Failure Modes, Recovery Strategies, and Graceful Degradation

## Context

Llamaste runs a single static C++ binary as PID 1 on a minimal Linux kernel. The binary
combines llama.cpp inference + agent loop + system tools + web UI. There is no init system,
no systemd, no shell, no BusyBox. If this binary crashes, the system is dead. This document
analyzes every meaningful failure mode, how to detect it, how to recover, and how to degrade
gracefully when full recovery is not possible.

---

## 1. PID 1 Crash Behavior

### What Happens When PID 1 Dies

When PID 1 exits for any reason on Linux, the kernel immediately calls `panic()` and halts
the system. The message is: `Kernel panic - not syncing: Attempted to kill init!`

This is non-negotiable kernel behavior. The call path is:
`do_exit() -> panic()` when `current->pid == 1`.

There is no recovery from this without a full reboot. The system freezes or reboots depending
on the `kernel.panic` sysctl setting.

### Signal Handling for PID 1

PID 1 has unique signal behavior on Linux:

- **Default signal actions are suppressed.** If PID 1 has no handler for a signal, the kernel
  ignores that signal entirely (even SIGTERM, SIGINT, SIGSEGV).
- **SIGKILL cannot kill PID 1.** The kernel explicitly protects PID 1 from SIGKILL.
- **SIGSEGV with no handler:** The default core dump action is suppressed. PID 1 survives a
  segfault if no handler is registered, but the process state is corrupt. This is dangerous
  because the process continues running in an undefined state.

**Recommendation for Llamaste:** Register explicit signal handlers for SIGSEGV, SIGBUS,
SIGFPE, SIGABRT, and SIGTERM. The SIGSEGV handler should log the fault to a persistent
location (ramoops or direct disk write) and then trigger a clean reboot via
`reboot(RB_AUTOBOOT)` rather than continuing in a corrupt state.

### The Supervisor Pattern: Fork a Child

The strongest pattern for PID 1 reliability is to keep PID 1 as a minimal supervisor and
fork the actual workload into a child process:

```
PID 1 (supervisor, ~500 lines of C)
  |
  +-- fork() --> Child (llamaste inference + agent + web UI)
```

PID 1 responsibilities (minimal supervisor):
- Open and pet `/dev/watchdog` every N seconds
- Fork the child process
- Wait for child exit (`waitpid`)
- If child dies: log reason, increment crash counter, re-fork
- If crash counter exceeds threshold: reboot
- Handle SIGCHLD to reap zombies
- Monitor memory pressure via `/proc/meminfo`
- Respond to SIGTERM by sending SIGTERM to child, then clean shutdown

This is exactly how tini, dumb-init, and s6-overlay work in containers. The key advantage:
if the inference engine crashes (segfault, OOM in child, assertion failure), PID 1 survives
and can restart the child. Only if PID 1 itself crashes does the kernel panic.

**Implementation cost:** ~500 lines of C code added to the binary. The binary starts as
the supervisor, forks itself with a `--child` flag for the real workload.

### Watchdog Integration

Hardware watchdog (`/dev/watchdog`) provides the last line of defense. The protocol:

1. Open `/dev/watchdog` at boot
2. Write any character (except 'V') to it every N seconds to reset the timer
3. If the write stops (crash, hang, deadlock), the hardware reboots the system

Key kernel config: `CONFIG_WATCHDOG_NOWAYOUT=y` -- ensures the watchdog cannot be disabled
once started, even if `/dev/watchdog` is closed. This is critical for embedded systems.

For llamaste, the PID 1 supervisor should pet the watchdog. If the supervisor itself hangs
or crashes, the hardware watchdog triggers a cold reboot.

Typical watchdog timeouts: 30-60 seconds. The supervisor should pet at half the timeout
interval (e.g., every 15 seconds for a 30-second timeout).

**Kernel boot parameter:** `kernel.panic=5` -- auto-reboot 5 seconds after kernel panic.
Combined with the watchdog, this means the system recovers from any crash within ~35 seconds.

---

## 2. Out of Memory (OOM)

### What Happens

When total memory consumption (model + KV cache + agent state + web server + kernel)
exceeds physical RAM, the Linux OOM killer activates. It scores every process and kills the
one with the highest "badness" score.

### Can the OOM Killer Kill PID 1?

The kernel explicitly skips PID 1 when selecting OOM victims. However, if PID 1 is the
only userspace process (as in Llamaste without the supervisor fork pattern), the kernel has
no other process to kill. The result: the system enters an OOM deadlock where no process
can be killed and no memory can be freed. This effectively freezes the system until the
hardware watchdog reboots it.

With the supervisor pattern, the child process IS killable by the OOM killer. PID 1
(supervisor) survives, detects child death via `waitpid`, and can re-fork with different
parameters (e.g., load a smaller model).

### Memory Management Strategy

**Proactive monitoring** is far better than reactive OOM handling:

```c
// Read /proc/meminfo every 5 seconds
// MemAvailable is the key metric (not MemFree)
//
// Thresholds:
//   MemAvailable < 15% of total: WARNING -- log, reduce KV cache
//   MemAvailable < 10% of total: CRITICAL -- unload model, serve status page
//   MemAvailable <  5% of total: EMERGENCY -- reboot
```

**Sysctl settings for Llamaste:**

| Setting | Value | Rationale |
|---------|-------|-----------|
| `vm.overcommit_memory` | `2` | Never overcommit. Fail allocations early. |
| `vm.overcommit_ratio` | `95` | Allow 95% of RAM for userspace. |
| `vm.panic_on_oom` | `0` | Let OOM killer act (don't panic). |
| `vm.oom_kill_allocating_task` | `1` | Kill the allocating task, not a random one. |

**oom_score_adj settings:**
- PID 1 supervisor: `-1000` (never kill)
- Child inference process: `0` (normal, killable)

### mlock Considerations

llama.cpp supports `--mlock` to pin model weights in physical RAM. This prevents swapping
but has dangerous implications:

- Locked memory CANNOT be reclaimed by the OOM killer
- If the model + locked KV cache exceeds available RAM, the system deadlocks
- On a system without swap (Llamaste has no swap), mlock provides no benefit for the model
  weights since there is nothing to swap to

**Recommendation:** Do NOT use `mlock` on Llamaste. Without swap, all resident memory is
already pinned. Instead, use `mmap` with `MAP_POPULATE` to fault in pages at load time,
which gives deterministic loading without the OOM deadlock risk.

### Model Fallback on OOM

If the child process is killed by OOM:

1. PID 1 supervisor detects child death with signal `SIGKILL` (signal 9, OOM kill)
2. Check `/proc/meminfo` -- how much RAM is actually available now?
3. Select a smaller model from the available set (e.g., drop from 7B to 1.5B to 0.5B)
4. Re-fork child with `--model <smaller_model>` argument
5. If no model fits: serve a static "out of memory" status page on the web UI

The llama.cpp server now supports runtime model management with `/models/load` and
`/models/unload` API endpoints, plus `--models-max 1` for single-model-at-a-time operation.

---

## 3. Model File Corruption

### GGUF Integrity

The GGUF format has basic structural validation:
- 4-byte magic number (`GGUF`) at file start
- Endianness markers
- Metadata key-value pairs with typed values
- Tensor info table with names, shapes, and offsets

However, GGUF currently **lacks built-in cryptographic integrity verification**. There are no
per-tensor checksums and no whole-file hash in the standard format. A community proposal
exists to add xxhash (per-tensor) and SHA256 (whole-file) hashing, but it is not yet standard.

Additionally, security researchers at Databricks discovered exploitable vulnerabilities in
GGUF parsing (heap overflow in array unpacking, buffer overflow in string reading). These
were patched in early 2024 but highlight that GGUF parsers need defensive coding.

### What llama.cpp Does on Corruption

When llama.cpp encounters a corrupted GGUF file:
- Magic number mismatch: returns an error, model fails to load
- Truncated file: segfault or error during tensor loading (depends on corruption location)
- Bitrot in tensor data: silent corruption -- model loads but produces garbage output

The third case is the most dangerous. There is no runtime detection of tensor data corruption.

### Llamaste Integrity Strategy

**At image build time:**
- Compute SHA256 of every `.gguf` file in the image
- Store hashes in a manifest file (`/data/models/MANIFEST.sha256`)

**At boot / before model load:**
1. Verify GGUF magic bytes (fast, catches gross corruption)
2. Verify file size matches manifest (fast, catches truncation)
3. Full SHA256 verification (slow: ~2 seconds per GB on modern CPU)
   - Do full verify on first boot after install
   - Do full verify if model load fails
   - Skip on normal boot for speed (rely on squashfs integrity for system partition models)

**On corruption detection:**
1. Log corruption details to persistent storage
2. Try next model in priority list (smaller or backup copy)
3. If no valid model exists: serve static web UI with download instructions
4. Expose `/api/health` endpoint with model integrity status

**Data partition models** (user-downloaded) should always be verified before loading, as the
data partition uses ext4 which is writable and can be corrupted by power loss.

---

## 4. Disk Full Scenarios

### What Fills Up

The system partition is squashfs (read-only, compressed) and cannot fill up. The data
partition (ext4, writable) stores:
- Conversation history / logs
- User configuration
- Downloaded models
- Cached data

### What Breaks

When `/data` is full:
- Logging fails silently or crashes the logger
- Configuration writes fail (settings changes lost)
- Model downloads fail mid-transfer (leaving corrupt partial files)
- Conversation persistence fails (current session lost on reboot)

### Proactive Monitoring

```
Disk usage thresholds (check every 60 seconds):
  < 80% used: NORMAL
  80-90% used: WARNING -- log, expose in web UI health dashboard
  90-95% used: ALERT -- start cleanup (old logs, expired cache)
  > 95% used: EMERGENCY -- delete old conversations, refuse new downloads
```

### Reserved Space

ext4 reserves 5% of blocks for root by default. On a 16 GB data partition, this is 800 MB.
For Llamaste:
- Set reserved blocks to 2% (`tune2fs -m2 /dev/sdXn`) -- 320 MB reserved
- This reserved space is only writable by root (which is our process)
- Even when the partition appears "full" to normal calculations, critical operations
  (config writes, crash logs) can still succeed using reserved blocks

### Log Rotation

Without logrotate (no cron, no shell), implement rotation in the binary:
- Ring buffer log: fixed-size log file (e.g., 10 MB), overwrite oldest entries
- Or: numbered log files with max count (e.g., `llamaste.log.{0..4}`, 2 MB each)
- Rotate on file size, not time
- Compress rotated logs with zstd (already linked for squashfs)

### Emergency Cleanup Priority

When disk reaches 95%, auto-delete in this order:
1. Compressed old log files (`.log.*.zst`)
2. Conversation history older than 30 days
3. Cached model metadata
4. Partial/failed model downloads (`.gguf.part` files)
5. NEVER delete: current configuration, model files, boot-critical data

---

## 5. Network Failures

### DHCP Failure at Boot

Without systemd or NetworkManager, Llamaste must handle DHCP itself. Strategy:

1. Send DHCP DISCOVER on all detected Ethernet interfaces
2. Wait up to 10 seconds for DHCP OFFER
3. If no response: assign link-local address (`169.254.x.x/16`) via RFC 3927
4. Also assign a configurable fallback static IP (default: `10.0.0.100/24`)
5. Continue DHCP attempts in background every 60 seconds
6. When DHCP succeeds: add the dynamic address, keep link-local as secondary

This requires implementing a minimal DHCP client in the binary (or statically linking a
lightweight one like `udhcpc` from BusyBox source -- just the DHCP client, ~15 KB).

### Network Down Mid-Operation

- **Model download interrupted:** Use HTTP Range requests for resumable downloads. Keep a
  `.gguf.part` file with download progress metadata. Resume on reconnection.
- **Cluster communication lost:** Mark cluster peers as "unreachable" after 3 missed
  heartbeats. Continue operating standalone. Rejoin automatically when connectivity returns.
- **Web UI accessed locally:** Always works (bound to 0.0.0.0 and 127.0.0.1).
- **DNS failure:** Cache DNS results. Provide fallback to IP-based access. The web UI
  works by IP address regardless.

### What the Web UI Should Show

When the network is partially down:
- Show a yellow status indicator: "Network: Limited (DHCP failed, using fallback IP)"
- Show which interfaces are up/down
- Show the current IP addresses
- If no model is loaded due to network-dependent download: show download progress or
  instructions for offline model installation (USB stick)

---

## 6. LLM Hallucination Safety

This is arguably the most important section. The LLM IS the operating system, which means
LLM hallucinations can become real system actions.

### Threat Model

The LLM might:
1. Generate tool calls to nonexistent tools
2. Pass invalid parameters to valid tools (wrong types, out of bounds)
3. Hallucinate destructive operations (`fs.delete /`, `process.kill 1`)
4. Chain multiple tool calls in a way that creates unintended effects
5. Misinterpret user intent and execute the wrong operation
6. Enter infinite tool-calling loops

### Tool Call Validation (Three Layers)

**Layer 1: Schema Validation (mandatory, every call)**
- Every tool call must match a JSON schema definition compiled into the binary
- Unknown tool names: reject immediately with error message to LLM
- Wrong parameter types: reject with specific error
- Missing required parameters: reject with specific error
- Use grammar-constrained generation to force valid JSON (already planned for speed)

**Layer 2: Semantic Validation (parameter bounds)**
- Path restrictions: tools can only access `/data/` (writable) and `/` (read-only)
  - Reject any path containing `..` after normalization
  - Reject paths to `/proc/`, `/sys/`, `/dev/` (except explicitly allowed devices)
- Process restrictions: cannot kill PID 1, cannot kill the inference child process
- Network restrictions: cannot bind to privileged ports (< 1024) except 80 and 443
- Size restrictions: file writes capped at 100 MB, log reads capped at 1 MB output
- Rate limits: max 20 tool calls per conversation turn, max 100 per minute

**Layer 3: Confirmation Gates (destructive operations)**

Operations by risk level:

| Risk Level | Examples | Behavior |
|------------|----------|----------|
| **Read-only** | `fs.list`, `fs.read`, `system.status`, `network.info` | Execute immediately |
| **Low-risk write** | `config.set theme dark`, `fs.write /data/notes/...` | Execute, log action |
| **Medium-risk** | `fs.delete /data/user-file`, `network.set-ip` | Require user confirmation in web UI |
| **High-risk** | `system.reboot`, `model.download`, `config.reset` | Require explicit user confirmation + 5s delay |
| **Forbidden** | `fs.delete /`, `process.kill 1`, any path outside /data for writes | Hard-blocked, never executed |

### Rate Limiting

```
Per-turn limits:
  Max tool calls per turn: 20
  Max consecutive tool errors: 5 (then force a text response)

Per-minute limits:
  Max tool calls per minute: 100
  Max file writes per minute: 50
  Max file deletes per minute: 10
  Max network operations per minute: 20

Per-session limits:
  Max total file deletes: 100 (then require session reset)
  Max disk usage from writes: 1 GB (then warn user)
```

### How Other Agent Systems Handle This

- **LangChain/LangGraph:** Uses "guardrails" middleware that intercepts tool calls before
  execution. Supports input rails (validate user prompt), execution rails (validate tool
  calls), and output rails (validate responses).
- **NVIDIA NeMo Guardrails:** Uses a Colang policy language to define allowed/disallowed
  patterns. Supports hallucination detection via trustworthiness scoring.
- **AutoGPT:** Has a `human_in_the_loop` flag that pauses for user approval on every action.
- **Open Interpreter:** Has a "safe mode" that blocks file system writes and code execution
  without confirmation.

**Llamaste approach:** Compile the validation rules into the binary. No external guardrail
service needed. The tool dispatch layer validates every call before execution. This is the
"neurosymbolic" approach -- symbolic rules enforced at the framework level that the LLM
cannot bypass through prompt manipulation.

---

## 7. Hardware Failures

### CPU Overheating

Sustained LLM inference is CPU-intensive. On passive-cooled or poorly ventilated systems,
thermal throttling or shutdown is possible.

**Detection:**
- Read `/sys/class/thermal/thermal_zone*/temp` every 10 seconds
- Typical thresholds: 80C warning, 90C critical, 100C emergency

**Response:**
- 80C: Log warning, reduce inference thread count (lower CPU load)
- 90C: Pause inference, serve "cooling down" message on web UI
- 100C: The kernel's thermal management will throttle or shut down the CPU
  - Llamaste should initiate a clean shutdown before the kernel forces one

### Disk I/O Errors

**Detection:**
- Monitor `read()`/`write()` return values -- every single one
- Check `dmesg` (or `/dev/kmsg`) for `ata` or `usb` error messages
- Increment error counter per device

**Response:**
- Transient errors (1-2): retry with backoff
- Persistent errors (5+): mark partition read-only, log error
- USB disconnect: if data partition was on USB, serve from squashfs only (Level 3 degradation)

### RAM Errors

Without ECC RAM (most consumer hardware), bit-flip errors are undetectable at the hardware
level. The model weights in RAM could silently corrupt, causing garbage output.

**Mitigation:**
- There is no reliable software-only detection of random bit flips
- If model output becomes consistently incoherent, the user can trigger a model reload
  which re-reads weights from disk (a fresh mmap)
- For critical deployments: recommend ECC RAM in hardware requirements
- The kernel's EDAC subsystem can detect and report ECC errors if ECC RAM is present:
  check `/sys/devices/system/edac/`

### Power Loss

**Filesystem protection:**
- System partition (squashfs): immune to power loss (read-only, compressed)
- Data partition (ext4 with journal): recovers via journal replay at next boot
  - Mount with `data=journal` (not default `data=ordered`) for maximum safety
  - Trade-off: ~5-10% write performance hit, but all data writes are journaled

**Boot-time recovery:**
- Run `e2fsck -p /dev/data-partition` at boot (automatic, non-interactive repair)
- If `e2fsck` fails: mount data partition read-only, operate in degraded mode

### USB Stick Wear

Consumer USB flash drives have limited write endurance (3,000-10,000 P/E cycles for TLC
NAND) and often lack proper wear leveling.

**Mitigation strategies:**
- Mount data partition with `noatime` (eliminates access time writes)
- Use a ring buffer for logs (bounded writes)
- Batch configuration writes (don't write on every change, write every 30 seconds or on
  explicit save)
- Recommend industrial-grade USB drives (SLC NAND, 100,000 P/E cycles) for production
- Monitor write volume: estimate remaining life based on total bytes written
  (`/sys/block/sdX/stat` field 7 = sectors written)

---

## 8. Graceful Degradation Ladder

Design the system so that partial failures result in partial functionality, not total failure.

### Level 0: Full Operation

Everything works. LLM inference + all tools + web UI + network + persistence.

**Requirements:** Model loaded, data partition writable, network up.

### Level 1: Model Unavailable

The model file is corrupted, too large for available RAM, or not yet downloaded.

**What works:**
- Web UI serves a status dashboard (system health, network info, disk usage)
- `/api/health` returns JSON system status
- Model download page allows user to download/install a model
- All system monitoring tools work (just no LLM to interpret results)
- Configuration UI works (direct form-based, no LLM needed)

**What doesn't work:**
- Chat interface (no model to run)
- LLM-driven tool calls (no inference)

**Detection:** Model load returns error, or SHA256 mismatch, or file not found.

### Level 2: Network Failed

No IP address obtained. System is operating on link-local only or not accessible remotely.

**What works:**
- Everything in Level 1
- LLM inference (if model is loaded -- model is on disk, no network needed)
- Local web UI accessible at link-local address or fallback static IP
- Serial console output (if kernel console is configured on ttyS0)

**What doesn't work:**
- Remote access (unless user knows the link-local/fallback IP)
- Model downloads
- Cluster operations
- NTP (clock may drift)

**Detection:** DHCP timeout, no carrier on all interfaces.

### Level 3: Data Partition Read-Only

Disk I/O errors, filesystem corruption, or USB disconnect forces read-only operation.

**What works:**
- LLM inference (model loaded from squashfs or already in RAM)
- Web UI (embedded in binary)
- Read-only tools (system status, file listing)
- Network operations

**What doesn't work:**
- Conversation persistence (history lost on reboot)
- Configuration changes (reverts to defaults on reboot)
- Log persistence
- Model downloads

**Detection:** `write()` returns `EROFS` or `EIO`, `e2fsck` fails at boot.

### Level 4: Critical Failure

Kernel panic, PID 1 crash, hardware watchdog timeout.

**What works:** Nothing. System reboots.

**Recovery path:**
1. Hardware watchdog triggers reboot (30-60 seconds)
2. OR: `kernel.panic=5` triggers reboot after 5 seconds
3. GRUB loads kernel, kernel loads llamaste
4. llamaste runs `e2fsck` on data partition
5. llamaste checks crash counter in persistent storage
6. If crash counter < 5: normal boot with crash reason logged
7. If crash counter >= 5: boot into "safe mode" (smaller model, reduced features)
8. If crash counter >= 10: boot into "recovery mode" (web UI only, no inference)

### Degradation Detection and Transitions

```
Boot sequence:
  1. PID 1 supervisor starts
  2. Open /dev/watchdog
  3. Check/repair data partition (e2fsck)
  4. Read crash counter from /data/.crash_count
  5. Determine boot mode (normal / safe / recovery)
  6. Fork child process
  7. Child: attempt network (DHCP, 10s timeout)
  8. Child: verify model integrity (magic + size check)
  9. Child: load model (may fail -> Level 1)
  10. Child: start web server
  11. Child: enter agent loop

Runtime transitions:
  - Any tool error -> log, return error to LLM, continue
  - Disk write error -> transition to Level 3
  - Network loss -> transition to Level 2
  - Model crash -> child restart, possible model downgrade
  - OOM kill -> child restart with smaller model
  - Watchdog timeout -> Level 4 reboot
```

---

## 9. State Recovery

### Conversation History

**Where:** `/data/conversations/` directory, one JSON file per session.

**How often:** Write-behind with 30-second flush interval. Also flush on:
- User explicitly saves
- Before model unload
- On SIGTERM to child process
- Before clean reboot

**Format:** JSON Lines (one JSON object per message), append-only. This is crash-safe:
a partial last line is simply truncated on recovery.

### Configuration State

**Strategy:** Atomic write via rename.

```c
// To update /data/config/settings.json:
// 1. Write to /data/config/settings.json.tmp
// 2. fsync() the file
// 3. rename() .tmp to .json (atomic on ext4)
// 4. fsync() the directory
```

Keep one backup: `/data/config/settings.json.bak` (the previous version).

If the primary config file is corrupt or missing at boot:
1. Try `.bak` file
2. If `.bak` is also corrupt: use compiled-in defaults
3. Log the recovery action

### Tool Operation Journal

Log every tool execution to `/data/journal/tool-ops.jsonl`:

```json
{"ts": 1709000000, "tool": "fs.write", "args": {"path": "/data/notes/todo.txt"}, "result": "ok"}
{"ts": 1709000001, "tool": "config.set", "args": {"key": "theme", "value": "dark"}, "result": "ok"}
```

Purpose:
- Audit trail (what did the LLM do?)
- Debug tool for users ("show me what you did in the last hour")
- NOT for replay (too dangerous to auto-replay tool calls after crash)

Rotate this journal at 5 MB, keep 3 rotations.

### Recovery After Unexpected Reboot

Boot sequence recovery:

1. Read `/data/.crash_count` -- increment if last shutdown was not clean
2. Read `/data/.last_shutdown` -- "clean" or missing (crash)
3. If crash: log previous boot's crash info from pstore/ramoops if available
4. Load config from `/data/config/settings.json` (with fallback chain)
5. List conversations in `/data/conversations/` -- the most recent one may be incomplete
6. Present "Welcome back" message with crash notification if applicable
7. Write "booting" to `/data/.last_shutdown`
8. On clean shutdown: write "clean" to `/data/.last_shutdown`

---

## 10. Testing Failure Modes in QEMU

### Simulating Each Failure

| Failure | QEMU Simulation Method |
|---------|----------------------|
| **PID 1 crash** | Send signals to PID 1 from within the guest: `kill -SEGV 1` (won't work due to PID 1 protection, but can test handler). Use `gdb` attached to QEMU to inject faults. |
| **OOM** | Start QEMU with limited RAM: `-m 512M` and load a model that needs 1 GB. Or use cgroups inside guest to limit child memory. |
| **Model corruption** | Truncate the GGUF file: `truncate -s -1000 model.gguf`. Flip random bytes: `dd if=/dev/urandom of=model.gguf bs=1 count=100 seek=1000 conv=notrunc`. |
| **Disk full** | Create a small data partition (50 MB) and fill it: `dd if=/dev/zero of=/data/fill bs=1M count=45`. |
| **Disk I/O errors** | QEMU `blkdebug` driver: configure it to inject errors on specific sectors. Also: `echo 1 > /sys/block/vda/device/delete` to simulate USB disconnect. |
| **Network failure** | QEMU `-net none` for no network. Or use `tc netem` inside guest: `tc qdisc add dev eth0 root netem loss 100%`. Disconnect QEMU's network tap from host. |
| **DHCP failure** | Run QEMU with network but no DHCP server on the virtual network. |
| **CPU overheating** | Write fake temperature values to a mock thermal zone (requires custom kernel module or sysfs override). Test the response logic with unit tests. |
| **Power loss** | Kill the QEMU process with `kill -9` mid-operation. Restart and check filesystem/data integrity. |
| **Watchdog timeout** | Enable QEMU's `i6300esb` watchdog: `-device i6300esb -watchdog-action reset`. Stop petting it to trigger reboot. |

### Chaos Engineering Script

Create a test harness that:

1. Boots Llamaste in QEMU
2. Waits for the web UI to respond on the expected port
3. Runs a series of chaos scenarios in sequence:
   - Kill child process, verify restart within 10 seconds
   - Fill disk to 95%, verify warning in web UI
   - Fill disk to 100%, verify emergency cleanup
   - Corrupt model file, verify fallback behavior
   - Disconnect network, verify link-local fallback
   - Limit RAM and trigger OOM, verify model downgrade
4. Verify system health after each scenario
5. Produce a pass/fail report

### Monitoring and Health Metrics

Expose at `/api/health`:

```json
{
  "status": "ok|degraded|critical",
  "level": 0,
  "uptime_seconds": 12345,
  "boot_count": 3,
  "crash_count": 1,
  "last_crash_reason": "OOM",
  "model": {
    "name": "qwen2.5-7b-q4_k_m",
    "loaded": true,
    "integrity": "verified"
  },
  "memory": {
    "total_mb": 16384,
    "available_mb": 4200,
    "model_mb": 4500,
    "kv_cache_mb": 512
  },
  "disk": {
    "data_total_mb": 16000,
    "data_used_percent": 42,
    "data_writable": true
  },
  "network": {
    "interfaces": [
      {"name": "eth0", "ip": "192.168.1.100", "source": "dhcp", "carrier": true}
    ]
  },
  "cpu": {
    "temperature_c": 55,
    "throttled": false
  },
  "watchdog": {
    "enabled": true,
    "timeout_seconds": 30
  }
}
```

Also expose at `/api/metrics` in Prometheus format for external monitoring.

---

## Failure Mode Summary Table

| # | Failure Mode | Severity | Likelihood | Detection Method | Recovery Strategy | Degradation Level |
|---|-------------|----------|------------|-----------------|-------------------|-------------------|
| 1 | PID 1 segfault | **Critical** | Low | Signal handler for SIGSEGV | Log to ramoops, reboot via `reboot()` syscall | 4 (reboot) |
| 2 | PID 1 hang/deadlock | **Critical** | Low | Hardware watchdog timeout | Hardware watchdog forces reboot | 4 (reboot) |
| 3 | Child process crash | **High** | Medium | `waitpid()` returns, check exit signal | PID 1 re-forks child, increment crash counter | 0 (restored) |
| 4 | OOM kill of child | **High** | Medium | `waitpid()` SIGKILL + check `/proc/meminfo` | Re-fork with smaller model | 0 (reduced model) |
| 5 | OOM deadlock (no child pattern) | **Critical** | Medium | System freeze, watchdog timeout | Watchdog reboot, boot with smaller model | 4 (reboot) |
| 6 | Model file corrupted | **High** | Low | SHA256 mismatch, magic byte check | Load fallback model; serve status page if none | 1 (no model) |
| 7 | Model file missing | **High** | Low | File not found on load attempt | Serve download/install page | 1 (no model) |
| 8 | Model too large for RAM | **High** | Medium | `mmap()` or allocation failure | Select smaller quantization or smaller model | 0 (reduced model) |
| 9 | GGUF parsing vulnerability | **Medium** | Very Low | Validation checks during parsing | Reject file, log error, try next model | 1 (no model) |
| 10 | Data partition full | **Medium** | Medium | `statfs()` check every 60 seconds | Auto-cleanup by priority; reserve 2% space | 0 (reduced) |
| 11 | Data partition corrupt | **High** | Low | `e2fsck` at boot, `write()` returns EIO | Repair or mount read-only | 3 (read-only) |
| 12 | Data partition unmountable | **High** | Very Low | `mount()` failure at boot | Operate without data partition (tmpfs fallback) | 3 (read-only) |
| 13 | DHCP failure | **Low** | Medium | 10-second timeout on DHCP DISCOVER | Assign link-local + fallback static IP | 2 (limited net) |
| 14 | Network cable unplugged | **Low** | Medium | Carrier detect via netlink socket | Retry every 30 seconds; log status | 2 (no net) |
| 15 | DNS failure | **Low** | Low | DNS query timeout | Use cached results; fallback to IP access | 0 (reduced) |
| 16 | Download interrupted | **Low** | Medium | HTTP error or connection reset | Resume via HTTP Range header on reconnection | 0 (retry) |
| 17 | LLM hallucinates invalid tool | **Medium** | High | Schema validation rejects call | Return error to LLM, let it retry | 0 (normal) |
| 18 | LLM hallucinates destructive op | **High** | Medium | Semantic validation + confirmation gate | Block execution, require user confirmation | 0 (blocked) |
| 19 | LLM infinite tool loop | **Medium** | Low | Rate limiter (20 calls/turn, 100/min) | Force text response after limit | 0 (limited) |
| 20 | CPU overheating | **Medium** | Low | `/sys/class/thermal/` polling | Reduce threads, pause inference, clean shutdown | 0 (throttled) |
| 21 | Disk I/O errors | **High** | Low | `read()`/`write()` error returns | Retry transient; go read-only on persistent | 3 (read-only) |
| 22 | USB drive disconnected | **High** | Low | I/O error on data partition | Operate from squashfs + tmpfs | 3 (read-only) |
| 23 | USB drive wear-out | **High** | Medium (long-term) | Increasing I/O error rate, sectors written counter | Warn user, recommend replacement | 3 (eventually) |
| 24 | RAM bit-flip (no ECC) | **Low** | Very Low | Not reliably detectable in software | User-triggered model reload; recommend ECC | 0 (undetected) |
| 25 | Power loss during write | **Medium** | Medium | Unclean shutdown flag at next boot | ext4 journal replay, `e2fsck -p` | 0 (recovered) |
| 26 | Kernel panic (not PID 1) | **Critical** | Very Low | `kernel.panic=5` auto-reboot | Hardware watchdog + auto-reboot + crash log | 4 (reboot) |
| 27 | Clock drift (no NTP) | **Low** | Medium | Compare with expected time range | Attempt NTP on reconnection; warn if >5 min drift | 0 (reduced) |

---

## Key Recommendations Summary

### Must-Have (Phase 1)

1. **Supervisor/child fork pattern.** PID 1 is a minimal supervisor (~500 lines). The real
   workload runs as a child process. This is the single most important reliability decision.

2. **Hardware watchdog integration.** Open `/dev/watchdog` at boot, pet it from the
   supervisor. Configure `CONFIG_WATCHDOG_NOWAYOUT=y` in the kernel.

3. **Signal handlers for PID 1.** Handle SIGSEGV, SIGBUS, SIGABRT with crash logging and
   clean reboot. Handle SIGCHLD for child process monitoring.

4. **Tool call validation.** Three layers: schema, semantic, confirmation gates. Compile
   the rules into the binary. This is not optional when an LLM controls system operations.

5. **Atomic configuration writes.** Write-to-temp + fsync + rename pattern. Always.

6. **`kernel.panic=5` boot parameter.** Auto-reboot on kernel panic.

7. **Disk space monitoring.** Check every 60 seconds. Auto-cleanup at 95%.

8. **Memory monitoring.** Check `/proc/meminfo` every 5 seconds. Model downgrade on OOM.

### Should-Have (Phase 1-2)

9. **Model integrity verification.** SHA256 manifest for all GGUF files.

10. **DHCP with fallback.** Implement minimal DHCP client with link-local fallback.

11. **Crash counter and safe mode.** Persistent crash counter triggers reduced-feature boot.

12. **Health API endpoint.** `/api/health` JSON for monitoring.

13. **Conversation write-behind.** 30-second flush interval, JSONL format.

14. **Boot-time filesystem check.** `e2fsck -p` on data partition.

### Nice-to-Have (Phase 2+)

15. **Prometheus metrics endpoint.** `/api/metrics` for external monitoring.

16. **Serial console output.** For Level 2 degradation (no network).

17. **pstore/ramoops.** Kernel crash log persistence across reboots.

18. **QEMU chaos test suite.** Automated fault injection testing.

19. **Tool operation journal.** Audit trail of LLM actions.

20. **Temperature-based inference throttling.** Reduce threads on CPU overheat.

---

## References

- Linux kernel source: `kernel/exit.c` -- PID 1 exit triggers panic
- Linux watchdog API: https://www.kernel.org/doc/html/latest/watchdog/watchdog-api.html
- GGUF format vulnerabilities: https://www.databricks.com/blog/ggml-gguf-file-format-vulnerabilities
- GGUF integrity discussion: https://github.com/ggml-org/llama.cpp/discussions/7891
- llama.cpp model management: https://huggingface.co/blog/ggml-org/model-management-in-llamacpp
- OOM killer configuration: https://www.oracle.com/technical-resources/articles/it-infrastructure/dev-oom-killer.html
- Linux overcommit in embedded: https://linuxembedded.fr/2020/01/overcommit-memory-in-linux
- tini (container init): https://github.com/krallin/tini
- dumb-init: https://github.com/Yelp/dumb-init
- s6-overlay: https://github.com/just-containers/s6-overlay
- NVIDIA NeMo Guardrails: https://github.com/NVIDIA-NeMo/Guardrails
- LLM guardrails best practices: https://www.datadoghq.com/blog/llm-guardrails-best-practices/
- ext4 atomic writes: https://www.kernel.org/doc/html/next/filesystems/ext4/atomic_writes.html
- Embedded Linux watchdog: https://circuitcellar.com/research-design-hub/watchdogs-in-embedded-linux/
- QEMU fault injection (FIES): https://github.com/ahoeller/fies
- Memfault reboot tracking: https://docs.memfault.com/docs/linux/reboot-reason-tracking
- PID 1 signal handling: https://petermalmgren.com/signal-handling-docker/
