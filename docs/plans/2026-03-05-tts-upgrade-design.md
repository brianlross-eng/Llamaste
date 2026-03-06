# TTS Upgrade Design: sherpa-onnx + Piper VITS

**Date**: 2026-03-05
**Status**: Approved
**Replaces**: Flite cmu_us_kal (8kHz diphone, Phase 3a-3)

## Problem

Flite's cmu_us_kal voice produces 8kHz robotic speech via diphone concatenation. Users expect natural-sounding voice output. Neural TTS (VITS architecture) produces dramatically better quality at 16-22kHz.

## Solution

Replace Flite with **sherpa-onnx** running **Piper VITS** voice models. sherpa-onnx provides a clean C API, bundles onnxruntime, and is Apache-2.0 licensed. Piper models produce natural speech at 16-22kHz.

## Architecture

### What Changes

**voice.cpp** synthesis backend swap:

```
Before: flite_text_to_wave(text, voice) → int16 PCM at 8kHz
After:  SherpaOnnxOfflineTtsGenerate(tts, text, sid, speed) → float32 PCM at 16-22kHz
```

Everything else stays the same: ALSA playback, `/audio/tts` endpoint, web UI speaker button, auto-speak logic, always-listening pipeline.

### C API Integration

```c
// Init
SherpaOnnxOfflineTtsConfig config = {};
config.model.vits.model = "/data/models/tts/en_US-amy-low.onnx";
config.model.vits.tokens = "/data/models/tts/tokens.txt";
config.model.vits.data_dir = "/data/models/tts/espeak-ng-data";
config.model.vits.length_scale = 1.0;
config.model.num_threads = 2;
config.model.provider = "cpu";
const SherpaOnnxOfflineTts* tts = SherpaOnnxCreateOfflineTts(&config);

// Synthesize
const SherpaOnnxGeneratedAudio* audio =
    SherpaOnnxOfflineTtsGenerate(tts, text, /*sid=*/0, /*speed=*/1.0f);
// audio->samples is float32 PCM, audio->sample_rate is 16000 or 22050

// Cleanup
SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
SherpaOnnxDestroyOfflineTts(tts);
```

Output is float32 samples (-1.0 to 1.0). Convert to int16 for ALSA playback:

```cpp
std::vector<int16_t> pcm(audio->n);
for (int i = 0; i < audio->n; i++)
    pcm[i] = static_cast<int16_t>(std::clamp(audio->samples[i], -1.0f, 1.0f) * 32767);
```

### Build Integration

New Buildroot package `sherpa-onnx` at `br2-external/package/sherpa-onnx/`:

- Builds from source (CMake)
- FetchContent pulls onnxruntime + espeak-ng automatically
- **Dynamic linking** (shared lib, same pattern as libcurl)
- Ships `libsherpa-onnx-c-api.so` + transitive deps in `/usr/lib`

CMake flags:
```
BUILD_SHARED_LIBS=ON
SHERPA_ONNX_ENABLE_TTS=ON
SHERPA_ONNX_ENABLE_C_API=ON
SHERPA_ONNX_ENABLE_BINARY=OFF
SHERPA_ONNX_ENABLE_PYTHON=OFF
SHERPA_ONNX_ENABLE_TESTS=OFF
SHERPA_ONNX_ENABLE_CHECK=OFF
SHERPA_ONNX_ENABLE_PORTAUDIO=OFF
SHERPA_ONNX_ENABLE_JNI=OFF
SHERPA_ONNX_ENABLE_WEBSOCKET=OFF
SHERPA_ONNX_ENABLE_GPU=OFF
SHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF
```

Llamaste CMakeLists.txt:
```cmake
find_library(SHERPA_ONNX_LIB NAMES sherpa-onnx-c-api)
if(SHERPA_ONNX_LIB)
    target_compile_definitions(llamaste PRIVATE HAVE_SHERPA_ONNX)
    target_link_libraries(llamaste PRIVATE ${SHERPA_ONNX_LIB})
endif()
```

### Voice Model Strategy

- **Bundled**: `en_US-amy-low` (~40MB, 16kHz) in squashfs for out-of-box quality
- `espeak-ng-data/` trimmed to English-only (~1MB)
- Users download higher-quality models via existing `model.download` tools
- TTS models stored in `/data/models/tts/`
- Model files: `.onnx` model + `.onnx.json` config + `tokens.txt` + `espeak-ng-data/`

### RAM Gating

- **>= 3GB RAM**: Load sherpa-onnx neural TTS
- **< 3GB RAM**: Keep Flite (robotic but ~13MB total)
- Detection reuses existing `hwdetect.cpp` RAM tier logic
- Gate check in `VoicePipeline::init()` before loading model

### Fallback

Keep Flite compiled in. If sherpa-onnx init fails (missing model, OOM, library not found), fall back to Flite with a log warning. Priority order:

1. `HAVE_SHERPA_ONNX` + model exists + RAM >= 3GB → sherpa-onnx
2. `HAVE_FLITE` → Flite cmu_us_kal
3. Neither → TTS disabled

### State Machine

No changes. `SPEAKING` state already covers synthesis + playback. The speak() method signature stays the same: `std::vector<int16_t> speak(const std::string& text, int* out_sample_rate)`.

### HTTP/Tool Changes

- `audio.status` → `tts_engine` changes from `"flite"` to `"sherpa-onnx"` (or `"flite"` if fallback)
- `audio.config` → add `tts_voice` field showing current model name
- `/llamaste/audio/tts` → no change (already handles variable sample rates)
- WAV encoding → sample rate from speak() output, already dynamic

### Web UI Changes

None. The speaker button, auto-speak, voice indicator all work unchanged.

## Size Impact

| Component | Size |
|-----------|------|
| libsherpa-onnx-c-api.so + deps | ~15-25 MB |
| espeak-ng-data (English-only) | ~1 MB |
| en_US-amy-low.onnx model | ~40 MB |
| **Total squashfs increase** | **~55-65 MB** |
| Current squashfs | 91 MB |
| **New squashfs estimate** | ~150-155 MB |

Disk image impact: ~55-65 MB increase (squashfs compressed).

## RAM Impact

~120-150 MB additional at runtime:
- onnxruntime engine: ~30-50 MB
- Model weights: ~40 MB (low quality)
- espeak-ng phonemizer: ~5 MB
- Inference buffers: ~10-20 MB

On 4GB system with 1.5GB LLM: ~2GB free. Comfortable.

## Licensing

- **sherpa-onnx**: Apache-2.0
- **onnxruntime**: MIT
- **espeak-ng**: GPL-3.0 (statically linked inside sherpa-onnx)
- **Piper models**: MIT (rhasspy/piper-voices)

espeak-ng GPL-3.0 is contained inside `libsherpa-onnx-c-api.so`. Llamaste binary dynamically links to it — standard Linux distribution practice. Source available via sherpa-onnx's GitHub repository (satisfies GPL source availability requirement).

## Files Modified

- `src/llamaste/voice.h` — add sherpa-onnx types to Impl forward decl
- `src/llamaste/voice.cpp` — sherpa-onnx init/speak/cleanup, float32→int16 conversion
- `src/llamaste/tools_audio.cpp` — engine name reporting
- `src/llamaste/CMakeLists.txt` — sherpa-onnx library detection and linking
- `br2-external/package/sherpa-onnx/` — new Buildroot package (Config.in, sherpa-onnx.mk, sherpa-onnx.hash)
- `br2-external/package/llamaste/Config.in` — select sherpa-onnx
- `br2-external/package/llamaste/llamaste.mk` — add dependency
- `br2-external/configs/llamaste_x86_64_defconfig` — enable sherpa-onnx package

## What Doesn't Change

- Web UI (same endpoints, same buttons, same playback)
- ALSA playback code (sample rate already parameterized)
- WAV encoding for HTTP (already handles variable sample rates)
- Always-listening pipeline (STT side untouched)
- Agent auto-speak logic
- Voice state machine
- speak() method signature
