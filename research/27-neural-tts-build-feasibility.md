# Research 27: Neural TTS Build Feasibility (Piper VITS on musl Linux)

**Date**: 2026-03-06
**Purpose**: Evaluate practical build paths for neural TTS (Piper VITS) on Llamaste's musl-based Buildroot system

---

## 1. ONNX Runtime on musl/Alpine Linux

### Status
- Alpine Linux edge/community has an `onnxruntime` package (shared lib, aarch64/x86_64)
- **No official musl static libraries** from Microsoft or csukuangfj
- Building from source requires patching two known issues

### Known Build Issues

**Issue 1: `stat64()` in ONNX checker** (onnx/onnx#5668)
- `onnx/checker.cc` uses `stat64()` which is glibc-specific LFS64 API
- **Fix**: PR onnx/onnx#5669 — replace `stat64` with `stat` on non-glibc Linux
- Merged into ONNX >= 1.15.0

**Issue 2: `execinfo.h` in onnxruntime** (microsoft/onnxruntime#25726)
- `execinfo.h` (backtrace functions) is glibc-specific, not in musl
- **Fix**: PR #25726 adds `__GLIBC__` macro guards around execinfo usage

### Build Script Reference
- **RapidAI/OnnxruntimeBuilder** has `build-onnxruntime-musl.sh`
  - Targets onnxruntime 1.18.0
  - Applies `onnxruntime-1.18.0-musl.patch`
  - Uses musl cross-compilation toolchain (`musl-cross.toolchain.cmake`)
  - Builds both shared and static libraries

### Build Complexity: HIGH
- Massive C++ project (onnxruntime + protobuf + abseil-cpp + re2 + onnx)
- Expect 30-60 min build time
- Transitive dependencies all need musl compatibility
- Alpine's APKBUILD has working patches but produces shared libs

---

## 2. sherpa-onnx Overview

### What It Is
- Speech toolkit by k2-fsa (next-gen Kaldi) team
- STT + TTS + speaker diarization + VAD + speech enhancement
- Uses ONNX Runtime for inference, works offline
- Supports: embedded Linux, Android, iOS, Raspberry Pi, RISC-V, x86_64
- 12 language bindings (C/C++/Python/Go/Kotlin/Swift/C#/etc.)
- 100+ pretrained VITS models for 40+ languages

### Piper VITS Integration
- Directly loads Piper `.onnx` models (no conversion needed)
- Does NOT depend on piper-phonemize (uses espeak-ng data directly)
- Required files per voice: `model.onnx` + `tokens.txt` + `espeak-ng-data/`
- espeak-ng-data is shared across all Piper voices

### Build System
- CMake-based, self-contained (everything from source)
- **Default is static linking** on Linux (`BUILD_SHARED_LIBS=OFF`)
- Auto-downloads prebuilt onnxruntime from GitHub during cmake
- Can use pre-installed ORT via `SHERPA_ONNXRUNTIME_LIB_DIR` / `SHERPA_ONNXRUNTIME_INCLUDE_DIR`
- GCC > 10 required for static linking (otherwise use shared)

### Key CMake Options
| Option | Default | Notes |
|--------|---------|-------|
| `BUILD_SHARED_LIBS` | OFF | Static by default |
| `SHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY` | ON | |
| `SHERPA_ONNX_USE_PRE_INSTALLED_ONNXRUNTIME_IF_AVAILABLE` | ON | |
| `CMAKE_BUILD_TYPE` | - | Use Release |

### musl Compatibility: UNKNOWN (not tested)
- sherpa-onnx downloads glibc-based ORT by default
- Would need musl-built ORT supplied via env vars
- sherpa-onnx's own C++ code may also have glibc assumptions (untested)

---

## 3. Piper VITS — Model Sizes & Quality

### Architecture
- VITS (Variational Inference with adversarial learning for end-to-end TTS)
- 15-20M parameters (very lightweight for neural TTS)
- Exported to ONNX format
- Phonemization via espeak-ng (G2P separate from synthesis)

### Quality Tiers

| Quality | Sample Rate | Model Size (ONNX) | Model Size (int8) | Notes |
|---------|-----------|-------------------|-------------------|-------|
| `x_low` | 16 kHz | ~15-20 MB | ~8-10 MB | Very few voices available |
| `low` | 16 kHz | ~60-75 MB | ~22 MB | Same arch as medium, lower sample rate |
| `medium` | 22.05 kHz | ~60-75 MB | ~22 MB | Most common, good balance |
| `high` | 22.05 kHz | ~100+ MB | ~35-40 MB | Best quality, slower |

### Smallest Usable Voice
- **`en_US-amy-low`**: ~60 MB ONNX, ~22 MB quantized int8
- **x_low voices are scarce** — Piper developer didn't prioritize training them
- For absolute minimum: quantize a low/medium model to int8 (~22 MB)
- espeak-ng-data directory adds ~15 MB (shared across all voices)

### Total Disk Footprint (minimum)
- ONNX model (int8 quantized): ~22 MB
- espeak-ng-data: ~15 MB
- tokens.txt: ~1 KB
- **Total: ~37 MB** for one voice

---

## 4. CPU Inference Performance

### Real-Time Factor (RTF) Benchmarks
| Hardware | RTF | Meaning |
|----------|-----|---------|
| Intel i7-1255U (10 core) | 0.2 | 5x faster than real-time |
| Threadripper 1800X | 0.2 | 5x faster than real-time |
| RK3588 ARM CPU | 0.65 | 1.5x faster than real-time |
| Embedded ARM (OKMX8MP) | 0.61 | 1.6x faster than real-time |
| RK3588 NPU (RKNN) | 0.15 | 6.7x faster than real-time |

### Expected Performance on 2 Cores (VBox)
- Raspberry Pi 4 (4 ARM cores) is the design target — runs well
- 2 x86 VBox cores should achieve RTF ~0.3-0.5 (2-3x real-time)
- A 5-second utterance would synthesize in ~1.5-2.5 seconds
- **Comfortably real-time** for conversational TTS
- Lower quality models (x_low/low) will be faster

### Latency Characteristics
- First-word latency: typically under 1 second for short texts
- MeloTTS and Piper are the fastest open-source TTS models
- Piper processes short texts in under a second consistently

---

## 5. Alternatives to ONNX Runtime for musl

### Option A: ncnn (Tencent)
- **Zero external dependencies** — perfect for musl static linking
- nihui (ncnn author) created `ncnn-android-piper` project
- Converts Piper VITS models from ONNX checkpoint to ncnn format via PNNX
- Custom dictionary phonemizer (no espeak dependency, MIT license)
- **Drawback**: Model conversion pipeline needed (ONNX → ncnn via PNNX)
- **Drawback**: Less community support for TTS-specific models
- **Build**: `cmake` with musl toolchain, trivial

### Option B: OnnxStream
- Extremely lightweight C++ ONNX inference library
- 55x less memory than OnnxRuntime with 50-200% latency increase
- Supports ARM, x86, WASM, RISC-V
- Minimal dependencies — good musl candidate
- **Drawback**: Not designed for VITS/TTS specifically
- **Drawback**: May need custom operator support for VITS ops

### Option C: Direct VITS C++ Implementation
- Piper itself is a C++ binary that uses onnxruntime
- Could fork and replace ORT with lighter inference
- `piper-without-espeak` exists (MIT, English only, custom phonemizer)
- **Drawback**: Significant engineering effort

### Recommendation
**ncnn** is the most promising alternative:
1. Zero deps, trivially builds with musl
2. Proven to work with Piper VITS models (nihui's project)
3. PNNX conversion tool handles the ONNX→ncnn step
4. Mobile-optimized = embedded Linux friendly

---

## 6. Build Strategy Comparison

### Path 1: sherpa-onnx + onnxruntime (musl static)
**Effort**: HIGH
- Build onnxruntime from source with musl patches (stat64 + execinfo.h)
- Use RapidAI/OnnxruntimeBuilder as reference
- Supply musl ORT to sherpa-onnx via env vars
- Get sherpa-onnx's full TTS pipeline (espeak-ng, 100+ models)
- Risk: untested combination, may surface more musl issues

### Path 2: ncnn + Piper VITS (musl static)
**Effort**: MEDIUM
- Build ncnn (zero deps, trivial musl build)
- Convert Piper checkpoints to ncnn format via PNNX
- Use nihui/ncnn-android-piper as reference (adapt for Linux)
- Custom dictionary phonemizer (no espeak dependency)
- Risk: limited to models you convert yourself, phonemizer quality

### Path 3: Alpine's onnxruntime package + sherpa-onnx
**Effort**: MEDIUM-LOW
- Use Alpine's working onnxruntime shared library
- But: Llamaste uses Buildroot, not Alpine
- Would need to port Alpine's APKBUILD patches to Buildroot
- Gets shared libs, not static — adds dynamic linking complexity

### Path 4: Embed Piper directly (fork piper + musl ORT)
**Effort**: HIGH
- Fork Piper's C++ code
- Build against musl onnxruntime
- Bundle espeak-ng data
- Most control but most work

---

## 7. Recommendation for Llamaste

### Best Path: sherpa-onnx + onnxruntime (Path 1)

**Rationale**:
1. sherpa-onnx already provides C/C++ API for TTS — minimal integration code
2. Supports Piper models natively (no conversion)
3. 100+ pretrained voices available immediately
4. Static build is the default — aligns with Llamaste's architecture
5. The musl patches are known and documented
6. onnxruntime is already in Alpine edge — proof the musl build works

**Implementation Plan**:
1. Build onnxruntime 1.23.2 for musl x86_64 (use RapidAI script as base)
2. Apply stat64 + execinfo.h patches
3. Build as static library (`libonnxruntime.a`)
4. Build sherpa-onnx against musl ORT, static linking
5. Add as Buildroot package (external.mk pattern)
6. Embed one Piper voice (en_US-amy-low, ~22 MB int8 quantized)
7. Add espeak-ng-data (~15 MB) to rootfs
8. Integrate sherpa-onnx C API into child_main.cpp TTS pipeline

**Fallback**: If musl ORT build proves too painful, switch to ncnn (Path 2)

### Disk Budget
| Component | Size |
|-----------|------|
| sherpa-onnx + ORT static lib | ~15-20 MB (stripped) |
| Piper voice (int8 quantized) | ~22 MB |
| espeak-ng-data | ~15 MB |
| **Total** | **~52-57 MB** |

### Performance Expectation
- RTF ~0.3-0.5 on 2 VBox cores (x86_64)
- Sentences synthesized in 1-3 seconds
- Comfortably real-time for conversational use
- Major upgrade from espeak-ng (currently used)

---

## Sources
- onnx/onnx#5668, onnx/onnx#5669 (stat64 musl fix)
- microsoft/onnxruntime#25726 (execinfo.h musl fix)
- microsoft/onnxruntime#2909 (Alpine support request)
- github.com/RapidAI/OnnxruntimeBuilder (musl build script)
- github.com/csukuangfj/onnxruntime-libs (prebuilt ORT)
- github.com/k2-fsa/sherpa-onnx (sherpa-onnx)
- github.com/rhasspy/piper (Piper TTS)
- github.com/nihui/ncnn-android-piper (ncnn + Piper)
- github.com/vitoplantamura/OnnxStream (lightweight ORT alternative)
- huggingface.co/rhasspy/piper-voices (Piper voice models)
- k2-fsa.github.io/sherpa/onnx/tts/piper.html (sherpa-onnx Piper docs)
