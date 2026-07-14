# Llamaste — Supplemental Hardware Architecture Review

**Date**: 2026-07-13  
**Scope**: GPU compatibility (AMD/Intel/NVIDIA), compute backend selection (CUDA/Vulkan/ROCm), network priority (ethernet vs WiFi), unified vs dedicated memory  
**Files examined**: `hwdetect.cpp/h`, `child_main.cpp`, `supervisor.cpp`, `init.cpp`, `llama-server.mk`, `linux.config`

---

## 1. GPU VENDOR COMPATIBILITY — Mixed

### 1.1 Detection (`hwdetect.cpp:67-123`)

Correctly identifies GPU vendor via PCI vendor IDs (`0x1002`=AMD, `0x10de`=NVIDIA, `0x8086`=Intel) and reads the kernel driver name via `/sys/class/drm/cardX/device/driver` symlink. Has a PCI-bus fallback for GPUs not exposed via DRM. **Detection itself is solid.**

### 1.2 AMD — GOOD

- Module loading (`init.cpp:1346-1348`): `amdxcp.ko` → `amdgpu.ko` in correct dependency order.
- Kernel config: full amdgpu support, HSA/KFD for compute (`CONFIG_HSA_AMD=y`), AMD DC display engine.
- Strix Halo firmware: VCN 4.0.6 correct, `amdgpu.modeset=0` workaround prevents boot hang.
- **Verdict**: AMD GPU display and compute infrastructure is ready. Only blocker is no ROCm/HIP llama.cpp build (see §2).

### 1.3 Intel — PARTIAL (gap)

- Kernel config: `CONFIG_DRM_XE=m` (Intel Xe — Arc, Battlemage, Lunar Lake, Meteor Lake). **But `CONFIG_DRM_I915 is not set`** — disabled because i915 conflicts with PREEMPT_RT on kernel 6.12. This means **all pre-Xe Intel GPUs (11th gen and older) have NO kernel driver**.
- Module loading: `init.cpp:1337-1351` loads amdgpu and nouveau explicitly. **Intel Xe (`xe.ko`) is NEVER loaded.** The child `hwdetect` will see Intel GPU on PCI bus but the DRM module was never loaded, so `/sys/class/drm/cardX` may not exist, and the fallback path hits simpledrm.
- **Verdict**: Intel Xe GPUs are partially in kernel config but not initialized. Older Intel GPUs (pre-12th gen) are completely unsupported. Desktop mode falls back to simpledrm+pixman on all Intel hardware.

### 1.4 NVIDIA — WEAK

- Kernel config: `CONFIG_DRM_NOUVEAU=m` only. No proprietary NVIDIA driver.
- Module loading (`init.cpp:1351`): `nouveau.ko` loaded. No NVIDIA-specific firmware handling.
- **Verdict**: Only open-source nouveau. Most RTX 20/30/40/50 series cards won't work or will be severely limited. No CUDA possible without the proprietary driver.

---

## 2. CUDA/VULKAN/ROCm AUTO-SELECT — MISSING (CRITICAL)

### 2.1 Build Configuration (`llama-server.mk:17-22`)

```makefile
-DGGML_CPU=ON \
-DGGML_CUDA=OFF \
-DGGML_VULKAN=OFF \
-DGGML_METAL=OFF \
-DGGML_RPC=ON \
-DGGML_BLAS=OFF
```

**All GPU backends are explicitly disabled at compile time.** Only CPU inference is available. `GGML_HIP` (ROCm) isn't even listed — it was never considered.

### 2.2 Runtime (`spawn_llama_server`, `child_main.cpp:830-916`)

The spawn function passes these flags to llama-server:
```
-m <model> --host 127.0.0.1 --port <port>
-c <context> -t <cpu_threads> -tb <batch_threads>
-fa (flash attention)
--cpu-range <p_cores> --cpu-strict 1   (hybrid CPU pinning)
--rpc <endpoints>                       (cluster only)
--tensor-split <ratio>                  (cluster only)
```

**Notably absent:** `-ngl` (number of GPU layers), `--gpu-layers`, `--main-gpu`, `--tensor-split` for single-node GPU. There is **zero GPU offload capability** in the inference pipeline.

### 2.3 Auto-Select Logic — Does Not Exist

No code anywhere in the codebase:
- Checks which GPU backends llama-server was compiled with
- Selects CUDA vs Vulkan vs ROCm based on detected GPU
- Computes `-ngl` value based on VRAM size
- Passes any GPU flag to llama-server

The `HardwareInfo` struct (`hwdetect.h:5-29`) has `gpu_name` and `gpu_driver` but **no GPU memory size, no GPU compute capability flags, no backend preference field**. The cluster's `compute_tensor_split()` and `auto_select_model()` work on system RAM only.

### 2.4 What Would Be Needed

```
1. Compile llama-server with:
   -DGGML_CUDA=ON     (for NVIDIA)
   -DGGML_VULKAN=ON   (for AMD/Intel — portable)
   -DGGML_HIP=ON      (for AMD — ROCm native)

2. Add GPU VRAM detection to HardwareInfo:
   - Query /sys/class/drm/cardX/device/mem_info_vram_total
   - Or lspci -v for BAR size
   - Flag: is_unified_memory (Strix Halo) vs dedicated VRAM

3. Backend selection logic:
   - NVIDIA → CUDA (if compiled) → Vulkan fallback
   - AMD → ROCm (if compiled + /dev/kfd exists) → Vulkan fallback
   - Intel → Vulkan (only option)
   - All: CPU fallback

4. Compute -ngl from available VRAM vs model size, pass to spawn_llama_server()
```

---

## 3. ETHERNET PRIMARY / WiFi FALLBACK — PARTIALLY CORRECT

### 3.1 IP Display Priority — CORRECT

`supervisor.cpp:323-362` (`read_ip_address()`): Explicitly "Priority: wired ethernet > WiFi > anything else". Interface classification is name-based (`wl*` = WiFi). Correct for display purposes.

### 3.2 Boot-Time Network Init Order — WRONG (MEDIUM)

The actual network initialization order in child_main.cpp:

```
Line 2529-2680: WiFi init → wpa_supplicant spawn → dhcpcd on WiFi → 20s connection poll
Line 2701-2850: Wired ethernet auto-DHCP → carrier wait (10s RTL8125B) → dhcpcd
```

**WiFi connects before wired ethernet is even scanned.** This means:
- If WiFi is available but slow, the system waits 20s for WiFi DHCP before touching the ethernet port
- If WiFi connects with a bad route, it may interfere with later ethernet DHCP
- The wired ethernet carrier wait (RTL8125B, MT7925) comes after WiFi timeout, adding up to 30s of total boot delay

In server mode, the supervisor runs `supervisor_preflight_wifi()` at line 1557 (before spawning the child), which adds another 6s of WiFi interface probing. Ethernet isn't touched until the child process starts — which is after WiFi pre-flight.

### 3.3 Supervisor Pre-flight — Blocks on WiFi only

```cpp
// supervisor.cpp:1488-1515
static void supervisor_preflight_wifi(const SupervisorConfig& config) {
    if (config.boot_mode == "desktop") return;  // skips in desktop
    // ... up to 6s of WiFi interface probing
    // ... console WiFi setup
}
```

The supervisor has NO wired ethernet pre-flight. No early DHCP on eth0/enp*. If the system is a headless server with no WiFi card, it wastes 6s probing for a nonexistent WiFi interface before the child eventually discovers wired ethernet.

### 3.4 Fix

Swap the order: wired ethernet auto-DHCP FIRST, then WiFi if no wired connection obtained. In the supervisor, add a parallel quick ethernet DHCP attempt alongside the WiFi pre-flight.

---

## 4. DEDICATED GPU vs SHARED MEMORY — NOT DETECTED

### 4.1 Current State

`HardwareInfo` (`hwdetect.h:5-29`) has **no VRAM field**. The struct tracks:
- `ram_total_mb`, `ram_free_mb` — system RAM only
- `gpu_name`, `gpu_driver` — vendor identity, no memory info

Model loading (`child_main.cpp:2059-2061`):
```cpp
int free_ram_estimate = g_hwinfo.ram_free_mb - 512;  // system reserve
if (config.boot_mode == "desktop") free_ram_estimate -= 500;  // compositor
```

This uses **total system RAM** to size the model context. It has no awareness of:
- Whether the GPU has dedicated VRAM (e.g., RTX 4090 = 24GB VRAM separate from system RAM)
- Whether the GPU uses unified memory (e.g., Strix Halo = 128GB shared)
- What the GPU VRAM budget is

### 4.2 Impact by Platform

| Platform | GPU | Memory | Current behavior | What should happen |
|---|---|---|---|---|
| EVO-X2 (Strix Halo) | AMD RDNA3.5 | 128GB unified | Uses system RAM estimate — accidentally correct | Should detect unified memory, use full 128GB for model |
| Server + RTX 4090 | NVIDIA | 24GB VRAM + 64GB system | Only uses system RAM, ignores 24GB VRAM entirely | Should detect 24GB VRAM, offload model layers via CUDA |
| Laptop + Iris Xe | Intel Xe | Shared system RAM | Uses system RAM — correct | Correct but i915/Xe driver not loaded (see §1.3) |

### 4.3 Strix Halo Specific

The EVO-X2's 128GB unified memory is Strix Halo's killer feature — it's the whole point. The code doesn't know about it. It's only working because unified memory looks like "lots of system RAM" to the naive RAM estimator. This is accidental correctness.

### 4.4 Fix

```cpp
// Add to HardwareInfo:
uint64_t gpu_vram_mb = 0;      // 0 = unified/shared/unknown
bool gpu_is_discrete = false;  // dedicated GPU with separate VRAM
bool gpu_is_unified = false;   // unified memory (Strix Halo, Apple M-series, console APUs)

// Detect VRAM:
// - /sys/class/drm/cardX/device/mem_info_vram_total (AMDGPU)
// - nvidia-smi or /proc/driver/nvidia/gpus/*/information (NVIDIA proprietary)
// - /sys/class/drm/cardX/device/total_vram (Intel)
// If none exist → assume unified/shared (APU, iGPU)

// In spawn_llama_server():
// - If dedicated GPU + VRAM detected + CUDA/Vulkan available:
//     compute -ngl from VRAM, pass to llama-server
// - If unified memory:
//     use system RAM estimate (current behavior — correct for Strix Halo)
```

---

## Summary Table

| # | Area | Finding | Severity |
|---|------|---------|----------|
| S1 | GPU Backend | All GPU compute backends (CUDA, Vulkan, ROCm) disabled at compile time. CPU-only inference. | **CRITICAL** |
| S2 | GPU Backend | No GPU backend auto-select logic exists; no `-ngl` flag passing | **HIGH** |
| S3 | Intel GPU | `DRM_XE=m` in kernel but never loaded in init.cpp. `DRM_I915` disabled entirely. | **HIGH** |
| S4 | NVIDIA GPU | Nouveau only. No proprietary driver path, no CUDA possible. | **HIGH** |
| S5 | Network Order | WiFi init runs BEFORE wired ethernet. In server mode, adds 6s WiFi probe with no ethernet pre-flight. | **MEDIUM** |
| S6 | GPU Memory | No VRAM detection. Unified vs dedicated memory not distinguished. | **HIGH** |
| S7 | Strix Halo | 128GB unified memory works by accident (looks like system RAM). Not intentional. | **MEDIUM** |

---

## What Works

1. **GPU vendor detection** is correct — PCI vendor IDs, driver symlink, all three vendors covered
2. **AMD module loading** is correct — amdgpu dependencies, firmware, Strix Halo workaround
3. **IP address display priority** (wired > WiFi) is correct in `read_ip_address()`
4. **CPU inference pipeline** is well-optimized — P-core pinning, flash attention, semantic cache, grammar retry
5. **Mesh clustering** (RPC-based distributed CPU inference) is a creative use of available resources given no GPU backends
6. **Ethernet carrier detection** (RTL8125B 10s retry) is properly implemented — just runs in the wrong order