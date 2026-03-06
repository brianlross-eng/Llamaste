# Research 28: Piper TTS for Embedded Linux (Buildroot + musl)

**Date**: 2026-03-05
**Purpose**: Evaluate Piper TTS as a Flite replacement for Llamaste
**Current TTS**: Flite with cmu_us_kal voice (8kHz, robotic, ~4MB overhead)

---

## 1. Project Status

**IMPORTANT**: The original `rhasspy/piper` repository was **archived on October 6, 2025** and is now read-only. Development has moved to:

- **New repo**: https://github.com/OHF-Voice/piper1-gpl
- **License change**: MIT -> **GPL-3.0** (the new fork embeds espeak-ng which is GPL)
- **Latest version**: 1.4.1 (February 2026)
- **Status**: "Looking for maintainers" (contact voice@openhomefoundation.org)

The GPL-3.0 license is a significant consideration for Llamaste. The original MIT-licensed code at `rhasspy/piper` (release 2023.11.14-2) is still usable but frozen.

---

## 2. Dependencies

Piper has a substantial dependency chain:

### Direct Dependencies
| Dependency | Version | Purpose | License |
|---|---|---|---|
| **onnxruntime** | ~1.17.x | Neural network inference (VITS model) | MIT |
| **espeak-ng** | 1.51+ | Text-to-phoneme conversion (grapheme-to-phoneme) | GPL-3.0 |
| **piper-phonemize** | latest | C++ wrapper around espeak-ng + onnxruntime phonemization | MIT |
| **fmt** | 10.0.0 | String formatting | MIT |
| **spdlog** | 1.12.0 | Logging | MIT |
| **pthread** | system | Threading (Linux) | -- |

### Transitive Dependencies (via onnxruntime)
- protobuf (serialization)
- Eigen or MLAS (math kernels)
- re2 (regex)
- abseil-cpp
- flatbuffers
- Various ONNX graph optimization libs

### espeak-ng Data
- Full espeak-ng-data directory: ~2MB (all 100+ languages)
- Can be stripped to single language: under 500KB
- libespeak-ng.so: 1.56MB (unstripped), ~512KB (stripped)

### Key Insight
The **espeak-ng dependency is GPL-3.0**, which is why the OHF-Voice fork changed to GPL-3.0. Even the original MIT-licensed Piper code becomes effectively GPL when linked with espeak-ng. The only way to avoid GPL would be the proposed modular phonemizer (piper-phonemize issue #17) which would make espeak-ng optional, but this was never implemented.

---

## 3. musl libc Compatibility

### Piper itself
- **No known musl-specific issues** in the Piper C++ code itself
- Alpine Linux (musl-based) has a `piper-tts` package in edge/testing for armv7
- Alpine package dependencies: `libc.musl-armv7.so.1`, `libespeak-ng.so.1`, `libfmt.so.11`, `libonnxruntime.so.1`, `libpiper_phonemize.so.1`, `libstdc++.so.6`
- **All dynamically linked** -- no static musl build evidence found

### onnxruntime + musl
- **Recent fix (August 2025)**: PR #25726 and #25721 added `__GLIBC__` guards around `execinfo.h` (backtrace functions are glibc-specific)
- Before these patches, onnxruntime would **fail to compile** on musl due to missing `execinfo.h`
- The fix is in onnxruntime mainline now
- **Custom/minimal builds recommended**: `--minimal_build` + `--disable_ml_ops` + operator config file

### espeak-ng + musl
- espeak-ng builds cleanly on musl (Alpine ships it)
- Pure C code with minimal libc dependencies
- No glibc-specific APIs used

### piper-phonemize + musl
- Archive status -- no longer maintained
- Alpine ships `piper-phonemize` package for musl
- The library expects shared (.so) linking; static builds may hit duplicate symbol errors

### Verdict
**musl builds are possible but require care**, especially around onnxruntime. The Alpine Linux packages prove it works for dynamic linking. Static linking is more challenging due to onnxruntime's complexity.

---

## 4. Binary and Library Sizes

### Pre-built Release Sizes (v2023.11.14-2)
| Artifact | Size |
|---|---|
| piper_linux_x86_64.tar.gz | **26.5 MB** |
| piper_linux_aarch64.tar.gz | **26.0 MB** |
| piper_linux_armv7l.tar.gz | **25.4 MB** |
| piper_windows_amd64.zip | 22.5 MB |
| piper_macos_x64.tar.gz | 19.1 MB |

These tarballs include: piper binary + libonnxruntime.so + libpiper_phonemize.so + libespeak-ng.so + espeak-ng-data + libtashkeel_model.onnx.

### Component Breakdown (approximate, x86_64)
| Component | Size (stripped) |
|---|---|
| libonnxruntime.so (full) | ~24.5 MB |
| libonnxruntime.so (minimal/reduced) | ~13 MB |
| libonnxruntime.so (MinSizeRel + op stripping) | ~7-10 MB |
| libespeak-ng.so (stripped) | ~512 KB |
| libpiper_phonemize.so | ~200 KB |
| piper binary | ~500 KB |
| espeak-ng-data (all langs) | ~2 MB |
| espeak-ng-data (English only) | ~400 KB |
| fmt + spdlog (static) | ~200 KB |

### Comparison with Current Flite
| | Flite (current) | Piper (estimated) |
|---|---|---|
| Binary overhead | ~4 MB | ~15-30 MB |
| Voice data | ~4 MB (cmu_us_kal) | ~15-75 MB (ONNX model) |
| Total footprint | ~8 MB | ~30-100 MB |
| Audio quality | 8kHz robotic | 16-22kHz neural |

### Size Reduction Strategies for onnxruntime
1. `MinSizeRel` CMake build type
2. `--minimal_build` flag
3. `--include_ops_by_config` with only VITS-required operators
4. `--disable_ml_ops` (not needed for TTS)
5. `--disable_exceptions` and `--disable_rtti`
6. Strip debug symbols
7. Use shared libc++ instead of static

---

## 5. Voice Models

### Quality Tiers
| Quality | Sample Rate | Parameters | ONNX Size (typical) |
|---|---|---|---|
| x_low | 16 kHz | 5-7M | ~15-20 MB |
| low | 16 kHz | 15-20M | ~40-50 MB |
| medium | 22.05 kHz | 15-20M | ~63 MB |
| high | 22.05 kHz | 28-32M | ~75-90 MB |

### Specific Model Sizes (en_US)
| Model | Quality | Size | Speakers |
|---|---|---|---|
| en_US-amy-medium | medium | 63.2 MB | 1 |
| en_US-lessac-medium | medium | ~63 MB | 1 |
| en_US-libritts_r-medium | medium | ~75 MB | 904 |
| en_US-ryan-high | high | ~90 MB | 1 |

### Quantized Models
- int8 quantization reduces ONNX model to ~22 MB
- Quality impact is minimal for medium-quality voices
- Significant win for embedded: 63MB -> 22MB

### Model Downloads
- Hosted on HuggingFace: `rhasspy/piper-voices`
- Two files per voice: `.onnx` (model) + `.onnx.json` (config)
- 100+ pre-trained voices across 40+ languages
- Download URL pattern: `https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/{voice}/{quality}/{name}.onnx`

### Recommendation for Llamaste
- **x_low** (16kHz, ~15-20MB) for RAM-constrained tiers
- **medium** (22.05kHz, ~63MB) for 4GB+ systems
- int8 quantized medium (~22MB) as best balance

---

## 6. C/C++ API

Piper provides a clean C++ API in `piper.hpp` / `piper.cpp`:

### Key Types
```cpp
namespace piper {

struct PiperConfig {
    std::string eSpeakDataPath;
    bool useESpeak = true;
};

struct SynthesisConfig {
    float noiseScale = 0.667f;
    float lengthScale = 1.0f;    // speed control
    float noiseW = 0.8f;
    int sampleRate = 22050;       // output sample rate
    int sampleWidth = 2;          // 16-bit
    int channels = 1;             // mono
    std::optional<SpeakerId> speakerId;
    float sentenceSilenceSeconds = 0.2f;
};

struct SynthesisResult {
    double inferSeconds;
    double audioSeconds;
    double realTimeFactor;
};

struct Voice {
    PhonemizeConfig phonemizeConfig;
    SynthesisConfig synthesisConfig;
    ModelConfig modelConfig;
    ModelSession session;         // Ort::Session wrapper
};
```

### Key Functions
```cpp
void initialize(PiperConfig &config);
void terminate(PiperConfig &config);

void loadVoice(PiperConfig &config, std::string modelPath,
               std::string modelConfigPath, Voice &voice,
               std::optional<SpeakerId> &speakerId, bool useCuda);

// Synthesize to int16_t buffer (primary API for embedded use)
void textToAudio(PiperConfig &config, Voice &voice, std::string text,
                 std::vector<int16_t> &audioBuffer, SynthesisResult &result,
                 const std::function<void()> &audioCallback);

// Synthesize directly to WAV file
void textToWavFile(PiperConfig &config, Voice &voice, std::string text,
                   std::ostream &audioFile, SynthesisResult &result);
```

### Integration Notes
- `textToAudio()` returns raw `int16_t` PCM samples -- can feed directly to ALSA
- `audioCallback` fires per-sentence for streaming playback
- No dependency on command-line interface; library-only usage is clean
- `SynthesisResult.realTimeFactor` useful for performance monitoring
- CUDA support optional (not relevant for Llamaste's CPU-only target)

### Comparison with Current Flite Integration
| | Flite (current) | Piper |
|---|---|---|
| API style | C function calls | C++ namespace + structs |
| Output | float samples | int16_t samples |
| Streaming | No (full buffer) | Per-sentence callback |
| Sample rate | 8 kHz | 16-22.05 kHz |
| Speed control | No | lengthScale parameter |
| Multi-voice | Limited | Multiple models/speakers |

---

## 7. Audio Output Format

| Property | Value |
|---|---|
| Sample rate | 16,000 Hz (x_low/low) or 22,050 Hz (medium/high) |
| Bit depth | 16-bit signed PCM (int16_t) |
| Channels | 1 (mono) |
| Format constant | `MAX_WAV_VALUE = 32767.0f` |
| WAV encoding | Standard RIFF WAV header + raw PCM |

### ALSA Integration
Current Llamaste ALSA playback uses `SND_PCM_FORMAT_S16_LE` mono, which matches Piper's output format. The only change needed is the sample rate:
- Current (Flite): 8000 Hz
- Piper x_low/low: 16000 Hz
- Piper medium/high: 22050 Hz

The `snd_pcm_hw_params_set_rate()` call just needs the new rate value.

---

## 8. Static Linking and Cross-Compilation Issues

### Static Linking Challenges
1. **onnxruntime**: The biggest challenge. Full static build possible but complex:
   - Omit `--build_shared_lib` flag
   - Use `MinSizeRel` for size
   - Many internal dependencies (protobuf, re2, abseil) must also be static
   - Static lib can be 50-100MB before stripping

2. **espeak-ng**: Clean static build, no issues on musl

3. **piper-phonemize**: Duplicate symbol errors reported when mixing static/shared
   - Build system expects shared libs
   - Workaround: build espeak-ng and onnxruntime as static, link everything together

4. **Recommended approach**: Dynamic linking (matching current Llamaste pattern)
   - Llamaste already uses dynamic linking for libcurl
   - Ship libonnxruntime.so + libespeak-ng.so + libpiper_phonemize.so
   - This is what Alpine Linux does successfully on musl

### Cross-Compilation
- Piper's CMakeLists.txt uses standard CMake cross-compilation
- Linux-specific: `-static-libgcc -static-libstdc++` + RPATH `$ORIGIN`
- espeak-ng data files are architecture-independent (copy to target)
- onnxruntime cross-compilation is well-documented for ARM/aarch64
- Buildroot can drive the cross-compilation toolchain

### Buildroot Packaging Effort
No existing Buildroot package for Piper. Would need to create:
1. `br2-external/package/espeak-ng/` (or use upstream if available)
2. `br2-external/package/onnxruntime/` (most complex -- CMake ExternalProject)
3. `br2-external/package/piper-phonemize/`
4. `br2-external/package/piper/`

Each package needs `.mk` + `Config.in` + hash files. Estimated effort: 2-3 days for a working build.

---

## 9. sherpa-onnx as Alternative

### Overview
sherpa-onnx (https://github.com/k2-fsa/sherpa-onnx) is a unified speech processing library from the Next-gen Kaldi project (k2-fsa). It bundles STT + TTS + VAD + speaker diarization into a single library.

### Key Advantages Over Standalone Piper
| Feature | Piper (standalone) | sherpa-onnx |
|---|---|---|
| **Piper model support** | Native | Yes (converted models) |
| **Additional TTS models** | None | MATCHA, Kokoro, Kitten, Zipvoice |
| **espeak-ng dependency** | Required (GPL) | Bundles own espeak-ng-data |
| **piper-phonemize** | Required | Not needed |
| **STT included** | No | Yes (could replace whisper.cpp) |
| **VAD included** | No | Yes |
| **C API** | No (C++ only) | Yes (clean C API) |
| **C++ API** | Yes | Yes |
| **Static linking** | Difficult | Supported (`BUILD_SHARED_LIBS=OFF`) |
| **Cross-compilation** | Manual CMake | Pre-made build scripts |
| **Embedded Linux docs** | None | Extensive (aarch64, RISC-V) |
| **Maintenance** | Archived/seeking maintainers | Actively maintained (v1.12.28, Feb 2026) |
| **License** | MIT/GPL-3.0 | Apache-2.0 |

### sherpa-onnx C API for TTS
```c
// Configuration
SherpaOnnxOfflineTtsConfig config;
config.model.vits.model = "model.onnx";
config.model.vits.tokens = "tokens.txt";
config.model.vits.data_dir = "espeak-ng-data";

// Create engine
SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&config);
int sample_rate = SherpaOnnxOfflineTtsSampleRate(tts);
int num_speakers = SherpaOnnxOfflineTtsNumSpeakers(tts);

// Generate audio
const SherpaOnnxGeneratedAudio *audio =
    SherpaOnnxOfflineTtsGenerate(tts, "Hello world", /*sid=*/0, /*speed=*/1.0);

// With streaming callback
SherpaOnnxOfflineTtsGenerateWithCallback(tts, text, sid, speed, callback);

// Cleanup
SherpaOnnxDestroyOfflineTts(tts);
```

### sherpa-onnx Binary Sizes (aarch64, shared)
| Binary | Size |
|---|---|
| sherpa-onnx | 187 KB |
| sherpa-onnx-alsa | 191 KB |
| libsherpa-onnx-core.so | ~15-20 MB (estimated) |

### sherpa-onnx on musl
- No official musl documentation found
- Uses GNU toolchains in all examples
- onnxruntime musl patches (PRs #25721, #25726) would apply here too
- Alpine Linux does not ship sherpa-onnx (unlike Piper)
- Would need testing/patching for musl compatibility

### sherpa-onnx Performance (Raspberry Pi 4)
- VITS TTS (Dutch): RTF 0.61, 80 MB RAM
- Real-time capable on embedded hardware

### Llamaste Synergy: sherpa-onnx Could Replace BOTH whisper.cpp AND Flite
Currently Llamaste uses:
- whisper.cpp for STT (libwhisper.a + 3 libggml*.a)
- Flite for TTS (libflite.so)

sherpa-onnx could provide:
- STT (Whisper models via ONNX, or Zipformer)
- TTS (Piper VITS models, or MATCHA/Kokoro)
- VAD (Silero VAD model)
- All through one library with one inference runtime

This would reduce total dependency count and binary overhead.

---

## 10. Recommendation for Llamaste

### Option A: Standalone Piper (Original, MIT-licensed code)
**Pros**: Direct VITS inference, proven quality, simple C++ API
**Cons**: Archived repo, GPL espeak-ng contamination, 26MB+ binary, complex onnxruntime build, no musl static linking evidence, no Buildroot package
**Effort**: 3-4 days
**Risk**: Medium-high (onnxruntime + musl + Buildroot = many failure points)

### Option B: sherpa-onnx (Recommended)
**Pros**: Apache-2.0 license, actively maintained, C API, supports Piper models + more, static linking supported, cross-compilation scripts ready, could replace whisper.cpp too, extensive embedded platform testing
**Cons**: Larger scope (bundles STT+TTS+VAD), onnxruntime musl compatibility needs verification, no Buildroot package exists, no Alpine package exists
**Effort**: 3-4 days (TTS only), 5-7 days (replace whisper.cpp + Flite)
**Risk**: Medium (onnxruntime musl is the main risk, but actively maintained)

### Option C: Piper via OHF-Voice/piper1-gpl (New fork)
**Pros**: Actively maintained, espeak-ng embedded directly
**Cons**: **GPL-3.0 license** (incompatible with Llamaste's MIT-aspirational approach), same onnxruntime challenges
**Effort**: 3-4 days
**Risk**: Medium-high (license + build complexity)

### Recommended Path: Option B (sherpa-onnx)
1. Build sherpa-onnx with Buildroot musl toolchain (test onnxruntime musl patches)
2. Use VITS-Piper model (en_US, medium quality, or x_low for small systems)
3. If successful, consider replacing whisper.cpp with sherpa-onnx STT too
4. Apache-2.0 license is clean for Llamaste

### Fallback: Stay with Flite, upgrade voice
If onnxruntime proves too complex for musl:
- Flite with a better voice (e.g., `cmu_us_slt` at 16kHz) is marginal improvement
- Or use espeak-ng directly (GPL, but 512KB, formant synthesis, better than Flite)

---

## 11. Build Integration Sketch (sherpa-onnx for Buildroot)

```makefile
# br2-external/package/sherpa-onnx/sherpa-onnx.mk
SHERPA_ONNX_VERSION = 1.12.28
SHERPA_ONNX_SITE = https://github.com/k2-fsa/sherpa-onnx/archive/refs/tags/v$(SHERPA_ONNX_VERSION).tar.gz
SHERPA_ONNX_LICENSE = Apache-2.0
SHERPA_ONNX_INSTALL_STAGING = YES

SHERPA_ONNX_CONF_OPTS = \
    -DSHERPA_ONNX_ENABLE_TTS=ON \
    -DSHERPA_ONNX_ENABLE_BINARY=OFF \
    -DBUILD_SHARED_LIBS=ON \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DSHERPA_ONNX_ENABLE_CHECK=OFF \
    -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
    -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF

$(eval $(cmake-package))
```

### Integration with Llamaste Binary
```cpp
// In tools_audio.cpp or voice.cpp
#include "sherpa-onnx/c-api/c-api.h"

// Initialize
SherpaOnnxOfflineTtsConfig config = {};
config.model.vits.model = "/data/models/en_US-amy-medium.onnx";
config.model.vits.tokens = "/data/models/tokens.txt";
config.model.vits.data_dir = "/usr/share/espeak-ng-data";
config.model.num_threads = 2;

SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&config);

// Synthesize
const SherpaOnnxGeneratedAudio *audio =
    SherpaOnnxOfflineTtsGenerate(tts, text.c_str(), 0, 1.0);

// Feed to ALSA (audio->samples is float*, audio->n is count)
// Convert float -> int16_t, play at SherpaOnnxOfflineTtsSampleRate(tts)
```

---

## Sources
- https://github.com/rhasspy/piper (archived)
- https://github.com/OHF-Voice/piper1-gpl (active fork, GPL-3.0)
- https://github.com/rhasspy/piper-phonemize (archived)
- https://pkgs.alpinelinux.org/package/edge/testing/armv7/piper-tts
- https://github.com/microsoft/onnxruntime/pull/25726 (musl fix)
- https://github.com/k2-fsa/sherpa-onnx
- https://k2-fsa.github.io/sherpa/onnx/tts/piper.html
- https://k2-fsa.github.io/sherpa/onnx/install/aarch64-embedded-linux.html
- https://huggingface.co/rhasspy/piper-voices
- https://sourceforge.net/projects/piper-tts.mirror/files/2023.11.14-2/
- https://onnxruntime.ai/docs/build/custom.html
- https://github.com/espeak-ng/espeak-ng/issues/918
