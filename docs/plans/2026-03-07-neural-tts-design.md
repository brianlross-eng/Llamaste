# Neural TTS Design — Phase B

**Date**: 2026-03-07
**Status**: Design
**Depends on**: Phase 3a voice pipeline (complete)

---

## Goal

Replace Flite (8kHz robotic) with Piper VITS neural TTS via sherpa-onnx, giving Llamaste natural-sounding speech synthesis. Espeak-ng remains as fallback for low-RAM systems.

## Architecture Decision: musl + onnxruntime

**Problem**: sherpa-onnx auto-downloads prebuilt onnxruntime linked against glibc. Llamaste uses musl (Buildroot).

**Decision**: Build onnxruntime from source with musl, then build sherpa-onnx against it.

**Rationale**:
- We already have a sherpa-onnx Buildroot package (shared lib approach)
- Alpine Linux proves musl + onnxruntime works (edge/community package)
- RapidAI has a working `build-onnxruntime-musl.sh` reference script
- Only two known musl patches needed: `stat64→stat` (merged in ONNX ≥1.15) and `execinfo.h` guards (merged in ORT)
- Keeps the build system clean (no glibc compat shims)

**Fallback**: If musl ORT build proves intractable after 2 days, switch to ncnn backend (nihui/ncnn-android-piper — zero deps, trivial musl build, requires model conversion via PNNX).

## Component Overview

```
┌──────────────────────────────────────────────────────┐
│  Llamaste Binary                                      │
│                                                        │
│  voice.cpp (already scaffolded)                       │
│    ├─ #ifdef HAVE_SHERPA_ONNX  ← enable this          │
│    │    └─ SherpaOnnxCreateOfflineTts()                │
│    │    └─ SherpaOnnxOfflineTtsGenerate()              │
│    ├─ #ifdef HAVE_ESPEAK_NG   ← fallback (existing)   │
│    └─ #ifdef HAVE_FLITE       ← last resort (existing)│
│                                                        │
│  Links against:                                        │
│    libsherpa-onnx-c-api.so → libonnxruntime.so         │
│    libespeak-ng.so (for phonemization)                 │
│    libpiper_phonemize.so                               │
└──────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────┐
│  /data/models/tts/ (on DATA partition)                │
│    en_US-amy-low.onnx          (~63 MB)               │
│    tokens.txt                   (~1 KB)               │
│    espeak-ng-data/              (~18 MB)              │
│                                 ─────────             │
│                          Total: ~81 MB                │
└──────────────────────────────────────────────────────┘
```

## Buildroot Packages Required

### 1. onnxruntime (NEW)

Build from source with musl toolchain:
- Version: 1.23.2 (matches sherpa-onnx 1.12.x expectation)
- CMake options: `--minimal_build`, `--disable_ml_ops`, CPU only
- Patches: `execinfo.h` guard (may be upstream already), `stat64→stat` (upstream in ONNX ≥1.15)
- Output: `libonnxruntime.so` (~24 MB)
- License: MIT

### 2. sherpa-onnx (UPDATE existing package)

Update from v1.11.3 → v1.12.28:
- Point to musl-built onnxruntime via `SHERPA_ONNXRUNTIME_LIB_DIR` / `SHERPA_ONNXRUNTIME_INCLUDE_DIR`
- Disable auto-download of prebuilt onnxruntime
- Output: `libsherpa-onnx-c-api.so`, `libpiper_phonemize.so`, `libespeak-ng.so`
- CMake: existing flags are correct (TTS=ON, C_API=ON, GPU=OFF, etc.)

### 3. piper-voices (NEW — data package)

Download and install Piper voice model to target:
- Model: `en_US-amy-low` (smallest good English voice)
- Files: `.onnx` + `tokens.txt` + `espeak-ng-data/`
- Install to: `/data/models/tts/` (DATA partition, not squashfs)
- Download URL: `https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-en_US-amy-low.tar.bz2`
- Size: ~81 MB total

## Code Changes

### voice.cpp — Already Done
The sherpa-onnx init and generation code is already written behind `#ifdef HAVE_SHERPA_ONNX` (lines 274-327, 436-467). No code changes needed.

### CMakeLists.txt — Add sherpa-onnx linking
```cmake
if(SHERPA_ONNX_FOUND)
  target_compile_definitions(llamaste PRIVATE HAVE_SHERPA_ONNX)
  target_link_libraries(llamaste sherpa-onnx-c-api)
endif()
```

### post_build.sh — Install TTS model
Add model download and install step (or make it a separate data package).

### audio.download_model tool — Extend for TTS
Add TTS model download option alongside existing whisper model downloads.

## RAM Tier Behavior

| RAM | TTS Engine | Quality | Latency (5s utterance) |
|-----|-----------|---------|----------------------|
| <1 GB | none | - | - |
| 1-3 GB | espeak-ng | robotic, 22kHz | instant |
| 3+ GB | sherpa-onnx (Piper) | natural, 22kHz | ~1-2.5s |

## Disk Budget

| Component | Size |
|-----------|------|
| libonnxruntime.so | ~24 MB |
| libsherpa-onnx-c-api.so | ~5 MB |
| libpiper_phonemize.so | ~2 MB |
| libespeak-ng.so | ~0.5 MB |
| **Squashfs delta** | **~31.5 MB** |
| en_US-amy-low.onnx | ~63 MB |
| espeak-ng-data/ | ~18 MB |
| **DATA partition** | **~81 MB** |

Total: ~112.5 MB. SYS-A partition grows from ~150MB to ~182MB. DATA partition adds 81MB for the voice model.

## License Impact

- onnxruntime: MIT ✓
- sherpa-onnx: Apache-2.0 ✓
- piper-phonemize: MIT ✓
- espeak-ng: GPL-3.0 ⚠️

**espeak-ng is GPL-3.0**. Since it's loaded as a shared library (`.so`), the linking is dynamic, which provides a stronger argument for separation. However, this is a gray area — consult legal if selling commercial licenses. For an open-source project, GPL-3.0 is fine.

## Implementation Steps

1. Create `br2-external/package/onnxruntime/` — Buildroot package that builds ORT from source with musl
2. Update `br2-external/package/sherpa-onnx/` — Point to musl ORT, update version
3. Update `br2-external/configs/llamaste_x86_64_defconfig` — Enable `BR2_PACKAGE_SHERPA_ONNX=y`
4. Update `src/llamaste/CMakeLists.txt` — Find and link sherpa-onnx
5. Build and test in WSL2 Buildroot environment
6. Create model download tool/script for first-boot TTS model setup
7. Deploy to VDI, verify neural TTS via `audio.speak` tool
8. Benchmark RTF and memory usage

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| ORT musl build fails | Fallback to ncnn backend (no deps, trivial musl build) |
| ORT musl build takes >2 days | Time-box, then try ncnn |
| Model too large for small systems | Already gated by 3GB RAM check |
| espeak-ng GPL contaminates binary | Dynamic linking (.so) provides separation |
| Build time too long | ORT build is slow (~30-60 min); one-time cost |

## Success Criteria

- [ ] `audio.speak` produces natural-sounding speech on 4GB VM
- [ ] RTF < 1.0 (faster than real-time) on 2 CPU cores
- [ ] Fallback to espeak-ng works when sherpa-onnx unavailable
- [ ] Image size increase < 50MB (squashfs)
- [ ] No regression in boot time or LLM performance
