# VirtualBox Test Suite Design

**Date:** 2026-03-13
**Goal:** Comprehensive automated test suite covering 5 key Llamaste subsystems
**Architecture:** Hybrid — QEMU for functional tests, VirtualBox for PXE integration
**Tech Stack:** Bash test scripts, QEMU, curl, jq

## Approach

Extend the proven `qemu-test.sh` pattern (boot QEMU, hit HTTP endpoints, verify responses) to cover install, clustering, A/B updates, watchdog recovery, and model download. QEMU gives fast, repeatable, CI-friendly tests. The existing VBox PXE boot path remains for integration validation.

## Shared Infrastructure

### `scripts/qemu-test-lib.sh` — Common test helpers

Extracted from existing test scripts into a reusable library:

- `boot_qemu()` — Start QEMU instance with configurable RAM, CPUs, port, disk, mode
- `wait_for_health()` — Poll `/health` endpoint with timeout (default 60s)
- `http_get()` — GET request, return body
- `http_post()` — POST request with JSON body, return response
- `assert_json_field()` — Verify a JSON field equals expected value
- `assert_json_exists()` — Verify a JSON field is present
- `assert_http_status()` — Verify HTTP status code
- `kill_qemu()` — Graceful shutdown via QEMU monitor or SIGTERM
- `create_disk()` — Create a blank qcow2/raw disk image
- `log_pass()` / `log_fail()` — Test result reporting with counts
- `summary()` — Print pass/fail totals, exit with appropriate code

## Test 1: Install Flow

**Script:** `scripts/qemu-install-test.sh` (extend existing)

**Setup:**
- Boot ISO in QEMU with blank 8GB raw disk as secondary drive
- QEMU flags: `-cdrom llamaste.iso -drive file=target.img,format=raw`
- Wait for `/health`

**Test Steps:**
1. `GET /install/disks` → assert target disk detected (not boot device)
2. `POST /install/start {"device":"/dev/sda","confirm":true}` → assert `started=true`
3. Poll `GET /install/progress` every 2s until `finished=true` (timeout 120s)
4. Assert `success=true`, `percent=100`
5. Shutdown QEMU
6. Boot from installed disk (`-drive file=target.img`)
7. Wait for `/health` → assert responds 200
8. `GET /update/status` → assert `active_slot=A`

**Pass criteria:** Install completes, rebooted system serves /health

## Test 2: Multi-Node Clustering

**Script:** `scripts/qemu-cluster-test.sh` (new)

**Setup:**
- Boot 2 QEMU instances:
  - Node 1: port 8081, 2GB RAM, 2 CPUs
  - Node 2: port 8082, 1GB RAM, 1 CPU
- Both use installed disk images (from Test 1 or pre-built)
- QEMU user-mode networking with port forwards
- Each node needs a separate disk image (copy)

**Test Steps:**
1. Wait for both `/health` endpoints
2. `GET /cluster/status` on node 1 → assert `role=STANDALONE`
3. `POST /cluster/add-peer` on node 1 with node 2's IP
4. Sleep 3s (election + topology settle)
5. `GET /cluster/status` on node 1 → assert role is COORDINATOR (higher score: 2GB*10+2=22 vs 1GB*10+1=11)
6. `GET /cluster/status` on node 2 → assert role is WORKER
7. `GET /cluster/peers` on node 1 → assert `peer_count >= 1`
8. `GET /cluster/capacity` → assert `node_count=2`, `tensor_split` is non-empty
9. `GET /cluster/models` → assert `tiers` array present

**Pass criteria:** Election produces correct coordinator, capacity reports combined resources

**Note:** Tensor-split inference test requires actual model files — skip in CI, mark as manual test.

## Test 3: A/B Update

**Script:** `scripts/qemu-update-test.sh` (new)

**Setup:**
- Boot from installed disk (slot A active)
- Need a valid `.update` file with Ed25519 signature
- Alternative: test the grubenv manipulation and partition write directly

**Test Steps:**
1. `GET /update/status` → assert `active_slot=A`, capture `version`
2. `GET /update/check` → verify endpoint responds (may fail without internet — acceptable)
3. Create test update: copy current squashfs as "new version" with bumped version
4. Sign with test key (or skip signature if we can inject a test key)
5. `POST /update/install {"path":"/data/test-update.update"}` → assert success
6. Verify grubenv: `boot_counter=3`, `boot_success=0`, `active_slot=B`
7. Reboot QEMU
8. `GET /update/status` → assert `active_slot=B`
9. `POST /update/rollback` → verify switches back to A
10. Reboot → verify `active_slot=A`

**Challenge:** Creating a valid `.update` file requires Ed25519 signing with the embedded public key's corresponding private key. Options:
- A: Build a test update with the real signing key (if available)
- B: Test only the status/check endpoints and grubenv parsing (no actual install)
- C: Add a `--test-mode` flag that skips signature verification

**Decision:** Start with Option B (status + grubenv verification). Full update install test is a stretch goal requiring the signing key.

**Pass criteria:** Status endpoint reports correct slot, grubenv readable

## Test 4: Watchdog/Recovery

**Script:** `scripts/qemu-recovery-test.sh` (new)

**Setup:**
- Boot installed image with serial console (`-serial stdio`)
- Wait for `/health`

**Test Steps:**
1. `GET /health` → assert 200 (child running)
2. Find child PID: parse serial output for llama-server PID, or use a debug endpoint
3. Kill child: send command via QEMU monitor (`sendkey` sequence) or use a tool endpoint
4. Poll `/health` with 1s interval — expect failure then recovery
5. Assert `/health` returns 200 within 15s of kill
6. Repeat kill 4 more times rapidly (within 60s)
7. After 4th kill, assert recovery takes ~30s (backoff engaged)
8. Final `/health` → assert 200

**Challenge:** Killing the child process from outside QEMU requires either:
- A serial console command (but there's no shell)
- A debug/admin HTTP endpoint that triggers kill
- QEMU monitor `system_reset` (but that reboots the whole VM)

**Decision:** Add a test-only endpoint `POST /debug/kill-child` that sends SIGKILL to the child process. Guard it behind a `LLAMASTE_TEST_MODE` kernel parameter. This is the cleanest approach — no shell needed, works over HTTP.

**Pass criteria:** Child recovers after kill, backoff engages after rapid crashes

## Test 5: Model Download

**Script:** `scripts/qemu-model-test.sh` (new)

**Setup:**
- Boot installed image with network access (QEMU user-mode networking with DNS)
- Wait for `/health`

**Test Steps:**
1. `GET /model/list` → assert empty array (fresh install)
2. `GET /model/recommended` → assert returns recommendation with `model_name`, `filename`, `repo_id`
3. `GET /model/current` → capture current model info
4. `POST /model/download {"repo_id":"...","filename":"..."}` with smallest model (Qwen2.5-0.5B, ~400MB)
5. Poll download progress (or wait for completion) — timeout 300s
6. `GET /model/list` → assert GGUF file present
7. Verify file size > 300MB

**Challenge:** Requires internet access from QEMU. User-mode networking (`-netdev user`) provides NAT with DNS. Should work in WSL2 QEMU.

**Alternative:** If no internet, test only the recommendation and list endpoints (no actual download).

**Pass criteria:** Model recommended correctly, download completes (or endpoints respond correctly if offline)

## Test Runner

### `scripts/run-all-tests.sh` — Master test orchestrator

```
Usage: ./run-all-tests.sh [--quick] [--test NAME]

Tests (in order):
  1. install    — Install flow (qemu-install-test.sh)
  2. cluster    — Multi-node clustering (qemu-cluster-test.sh)
  3. update     — A/B update status (qemu-update-test.sh)
  4. recovery   — Watchdog recovery (qemu-recovery-test.sh)
  5. model      — Model download (qemu-model-test.sh)

Options:
  --quick     Skip model download (slow, needs internet)
  --test NAME Run only the named test
```

Produces summary:
```
=== Llamaste Test Suite Results ===
  install:  PASS (5/5 assertions)
  cluster:  PASS (8/8 assertions)
  update:   PASS (3/3 assertions)
  recovery: PASS (4/4 assertions)
  model:    SKIP (no internet)
  TOTAL:    4/4 PASS, 1 SKIP
```

## C++ Changes Required

### `POST /debug/kill-child` endpoint (for Test 4)

- Only registered when kernel cmdline contains `llamaste.test=1`
- Sends SIGKILL to child PID
- Returns `{"killed":true,"pid":1234}`
- File: `src/llamaste/tools_debug.cpp` (new, small)

## File Summary

| File | Action | Purpose |
|------|--------|---------|
| `scripts/qemu-test-lib.sh` | New | Shared test helpers |
| `scripts/qemu-install-test.sh` | Extend | Install flow test |
| `scripts/qemu-cluster-test.sh` | New | Multi-node cluster test |
| `scripts/qemu-update-test.sh` | New | A/B update status test |
| `scripts/qemu-recovery-test.sh` | New | Watchdog recovery test |
| `scripts/qemu-model-test.sh` | New | Model download test |
| `scripts/run-all-tests.sh` | New | Master test runner |
| `src/llamaste/tools_debug.cpp` | New | Debug kill-child endpoint |
| `src/llamaste/child_main.cpp` | Modify | Register debug tools when test mode |
