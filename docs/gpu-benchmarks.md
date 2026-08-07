# Llamaste GPU Inference Benchmarks — AMD Strix Halo (gfx1151) via Vulkan/RADV

Reference benchmarks for **Llamaste 0.5.0-beta**, the first build that offloads
LLM inference onto the AMD Strix Halo integrated GPU (Radeon 8060S, gfx1151)
through the Mesa **RADV** Vulkan driver, instead of running CPU-only.

## Test system

| | |
|---|---|
| Device | AMD EVO-X2 (Ryzen AI Max, "Strix Halo") |
| iGPU | Radeon 8060S, **gfx1151**, RADV (ACO) |
| System RAM | 128 GB LPDDR5X. BIOS assigns 96 GB to the iGPU: amdgpu reports `mem_info_vram_total` = **98304 MiB (96 GB dedicated VRAM)** + `mem_info_gtt_total` ≈ 15.9 GB; Linux sees ~31 GB as system RAM |
| **Boot requirement** | **`amdgpu.modeset=1`** — with `nomodeset` (old installed default) amdgpu is disabled, RADV finds no device, and llama-server silently runs on CPU. Fixed in the installed grub.cfg from 0.5.3-beta (default "Llamaste Server (GPU)" entry) |
| OS | Llamaste **0.5.0-beta**, installed to internal disk (`server` mode) |
| Kernel | 6.18.21 |
| Graphics stack | Mesa **24.2.8** RADV, Vulkan loader 1.3.262, `libvulkan_radeon.so` |
| Inference engine | llama.cpp (`llama-server`, tag b5460), `GGML_VULKAN=ON` |
| Backend confirmed | `Vulkan0` — all layers offloaded to GPU (`offloaded N/N layers`) |

## Method

- Models are Qwen2.5-Instruct GGUF, Q4_K_M quantization, loaded one at a time.
- Each row: one warmup request, then the median of 3 measured requests.
- **prompt eval tok/s** (prefill) and **generation tok/s** (decode) are read from
  `llama-server`'s own per-request timing log (`/debug/logs`), not wall-clock.
- Fixed generation length per request (`max_tokens = 128`) with a short prompt.
- All layers are on the GPU (`Vulkan0`); CPU is only the driver/host.

## Results

> **Note on 0.5.0-beta vs 0.5.1-beta.** 0.5.0-beta shipped a GPU-offload bug: on the
> *installed* system `hwdetect` mis-classified the Strix Halo APU as a discrete GPU
> (`mem_info_vram_total` reads 0), so no `-ngl` was passed and models ran on **CPU**
> (`offloaded 0/N`). Fixed in **0.5.1-beta** (unified/APU → `-ngl 99`). The numbers
> below measured on 0.5.0-beta are therefore the **CPU baseline**; the GPU column is
> filled from 0.5.1-beta.

### Strix Halo (Ryzen AI Max+ 395) — CPU baseline vs GPU (RADV)

All Qwen2.5-Instruct Q4_K_M. Prefill measured over Llamaste's real ~900-token system
prompt; generation = decode. Median of 3 cache-busted runs.

| Model | CPU prefill | CPU gen | GPU prefill | GPU gen | GPU layers | Speedup (gen) |
|-------|------------:|--------:|------------:|--------:|:----------:|:-------------:|
| 0.5B | 896 | 139 | _n/m_ | _n/m_ | 25/25 | ~1× |
| 3B | 279 | 43.6 | 346 | **78.1** | 37/37 | **1.79×** |
| 7B | _n/m_ | _n/m_ | 201 | **40.4** | 29/29 | — |
| 14B | _n/m_ | _n/m_ | 111 | **21.1** | 49/49 | — |
| 32B | 26 | 4.4 | 54 | **10.3** | 65/65 | **2.34×** |

_(tok/s. CPU = 16 inference threads on the Ryzen AI Max+ 395; GPU = all layers on
`Vulkan0` / RADV gfx1151.)_

Prior-hardware reference: 14B Q4_K_M on Core Ultra 9 275HX (24-core, AVX-512) ≈ 2.2 tok/s gen (CPU).

## Takeaways

- **GPU decode scales cleanly with size** — 78 → 40 → 21 → 10 tok/s across 3B/7B/14B/32B,
  roughly halving per ~2× parameters. It's memory-bandwidth-bound (CPU and iGPU share the
  same LPDDR5X), which is why the curve is so smooth.
- **The GPU speedup *grows* with model size**: 1.79× at 3B → 2.34× at 32B (same-box CPU vs
  GPU). Bigger models are more compute-bound, so the iGPU's parallelism pays off more; tiny
  models are near-even because both backends are bandwidth-limited and the CPU has less
  dispatch overhead.
- **The real win is making big models usable.** 32B goes from 4.4 tok/s (painful) to 10.3
  (fine for chat); 14B hits 21 tok/s (snappy). That's the difference the iGPU makes.
- **Everything fits on the GPU.** Even 32B Q4 is fully offloaded (65/65 layers, 18.5 GB on
  `Vulkan0`) inside the 96 GB VRAM carveout, with room for far larger models.
- **One gotcha that matters:** the box must boot with kernel mode-setting enabled
  (`amdgpu.modeset=1` or simply *not* `nomodeset`). With `nomodeset`, RADV finds no device
  and llama.cpp silently falls back to CPU — it still prints `offloaded N/N layers to GPU`
  but the buffers are `CPU_Mapped`. Always confirm `Vulkan0` buffers in the load log.

## Notes

- **Prefill is measured over Llamaste's real system prompt** (~900+ tokens: the agent
  instructions + 64 tool definitions), so prefill tok/s reflects the true cold-request
  cost, not a trivial 5-token prompt.
- Generation (decode) tok/s is bandwidth-bound and very stable run-to-run (0.5B varied
  by <0.4 tok/s across 3 runs). Prefill (compute-bound) varies a few %.
- Prompts are varied per run to defeat llama-server's KV-cache reuse (identical prompts
  otherwise return cached prefill and skew the numbers).
- 0.5B GPU footprint: 374 MiB weights + 96 MiB KV + ~299 MiB compute ≈ 770 MiB on Vulkan0.
- _More observations added as larger models are tested._
