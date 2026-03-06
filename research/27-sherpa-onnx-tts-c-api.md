# Research 27: sherpa-onnx C API for Offline TTS

**Date**: 2026-03-05
**Purpose**: Evaluate sherpa-onnx as a Flite replacement for high-quality TTS in Llamaste
**Version researched**: sherpa-onnx 1.12.28

## 1. C API Function Signatures

### Configuration Structs

```c
// For Piper VITS models
typedef struct SherpaOnnxOfflineTtsVitsModelConfig {
  const char *model;         // Path to .onnx model file
  const char *lexicon;       // Path to lexicon file (optional for Piper)
  const char *tokens;        // Path to tokens.txt
  const char *data_dir;      // Path to espeak-ng-data/ directory
  float noise_scale;         // Noise scale (default ~0.667)
  float noise_scale_w;       // Noise scale W (default ~0.8)
  float length_scale;        // Speed: <1.0 = faster, >1.0 = slower (default 1.0)
  const char *dict_dir;      // Optional dictionary directory
} SherpaOnnxOfflineTtsVitsModelConfig;

typedef struct SherpaOnnxOfflineTtsModelConfig {
  SherpaOnnxOfflineTtsVitsModelConfig vits;  // VITS/Piper config
  int32_t num_threads;       // Number of inference threads
  int32_t debug;             // Debug output (0=off)
  const char *provider;      // "cpu" for CPU inference
  // Also has: matcha, kokoro, kitten, zipvoice, pocket configs
} SherpaOnnxOfflineTtsModelConfig;

typedef struct SherpaOnnxOfflineTtsConfig {
  SherpaOnnxOfflineTtsModelConfig model;
  const char *rule_fsts;     // Optional FST rules for text normalization
  int32_t max_num_sentences; // Batch size for long text
  const char *rule_fars;     // Optional FAR rules
  float silence_scale;       // Scale silence duration between sentences
} SherpaOnnxOfflineTtsConfig;
```

### Audio Output Struct

```c
typedef struct SherpaOnnxGeneratedAudio {
  const float *samples;      // Audio samples (float32, -1.0 to 1.0)
  int32_t n;                 // Number of samples
  int32_t sample_rate;       // Sample rate (e.g., 22050 for Piper)
} SherpaOnnxGeneratedAudio;
```

### Core TTS Functions

```c
// Create TTS engine from config
const SherpaOnnxOfflineTts *SherpaOnnxCreateOfflineTts(
    const SherpaOnnxOfflineTtsConfig *config);

// Destroy TTS engine
void SherpaOnnxDestroyOfflineTts(const SherpaOnnxOfflineTts *tts);

// Query properties
int32_t SherpaOnnxOfflineTtsSampleRate(const SherpaOnnxOfflineTts *tts);
int32_t SherpaOnnxOfflineTtsNumSpeakers(const SherpaOnnxOfflineTts *tts);

// Generate audio (blocking, returns full audio)
const SherpaOnnxGeneratedAudio *SherpaOnnxOfflineTtsGenerate(
    const SherpaOnnxOfflineTts *tts,
    const char *text,          // Input text
    int32_t sid,               // Speaker ID (0 for single-speaker models)
    float speed);              // Speed factor (1.0 = normal)

// Generate with streaming callback (for real-time playback)
typedef int32_t (*SherpaOnnxGeneratedAudioCallbackWithArg)(
    const float *samples, int32_t n, void *arg);

const SherpaOnnxGeneratedAudio *
SherpaOnnxOfflineTtsGenerateWithCallbackWithArg(
    const SherpaOnnxOfflineTts *tts,
    const char *text, int32_t sid, float speed,
    SherpaOnnxGeneratedAudioCallbackWithArg callback, void *arg);

// Generate with progress callback
typedef int32_t (*SherpaOnnxGeneratedAudioProgressCallbackWithArg)(
    const float *samples, int32_t n, float progress, void *arg);

const SherpaOnnxGeneratedAudio *
SherpaOnnxOfflineTtsGenerateWithProgressCallbackWithArg(
    const SherpaOnnxOfflineTts *tts,
    const char *text, int32_t sid, float speed,
    SherpaOnnxGeneratedAudioProgressCallbackWithArg callback, void *arg);

// Free generated audio
void SherpaOnnxDestroyOfflineTtsGeneratedAudio(
    const SherpaOnnxGeneratedAudio *p);
```

### Audio File Utilities

```c
// Write samples to WAV file
int32_t SherpaOnnxWriteWave(const float *samples, int32_t n,
                            int32_t sample_rate, const char *filename);

// Get required buffer size for WAV
int64_t SherpaOnnxWaveFileSize(int32_t n_samples);

// Write WAV to pre-allocated buffer (no filesystem needed)
void SherpaOnnxWriteWaveToBuffer(const float *samples, int32_t n,
                                 int32_t sample_rate, char *buffer);
```

## 2. Piper VITS Model Configuration

### Minimal Init Code

```c
SherpaOnnxOfflineTtsConfig config;
memset(&config, 0, sizeof(config));

// Piper VITS model
config.model.vits.model = "/data/models/en_US-lessac-medium.onnx";
config.model.vits.tokens = "/data/models/tokens.txt";
config.model.vits.data_dir = "/data/models/espeak-ng-data";
config.model.vits.noise_scale = 0.667;
config.model.vits.noise_scale_w = 0.8;
config.model.vits.length_scale = 1.0;  // 1.0 = normal speed

config.model.num_threads = 2;
config.model.debug = 0;
config.model.provider = "cpu";
config.max_num_sentences = 1;

const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&config);

// Generate
const SherpaOnnxGeneratedAudio *audio =
    SherpaOnnxOfflineTtsGenerate(tts, "Hello world", 0, 1.0);
// audio->samples = float32 PCM
// audio->n = sample count
// audio->sample_rate = 22050

// Cleanup
SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
SherpaOnnxDestroyOfflineTts(tts);
```

### Key Config Notes
- `data_dir` = path to `espeak-ng-data/` directory (required for phonemization)
- `lexicon` = not needed for Piper models (espeak-ng handles it)
- `tokens` = maps token IDs to phonemes (model-specific, ~921 bytes)
- `noise_scale` and `noise_scale_w` control voice variability
- `length_scale` controls speaking speed

## 3. CMake Build Flags for Minimal TTS-Only Build

```bash
cmake -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DSHERPA_ONNX_ENABLE_TTS=ON \
  -DSHERPA_ONNX_ENABLE_C_API=ON \
  -DSHERPA_ONNX_ENABLE_BINARY=OFF \
  -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
  -DSHERPA_ONNX_ENABLE_TESTS=OFF \
  -DSHERPA_ONNX_ENABLE_CHECK=OFF \
  -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
  -DSHERPA_ONNX_ENABLE_JNI=OFF \
  -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
  -DSHERPA_ONNX_ENABLE_GPU=OFF \
  -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
  -DSHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY=ON \
  ..
```

### What Gets Built
- `libsherpa-onnx-c-api.a` -- C API wrapper
- `libsherpa-onnx-core.a` -- Core engine (includes ASR code too, no way to exclude)
- `libpiper_phonemize.a` -- Text-to-phoneme conversion
- `libespeak-ng.a` -- Phonemizer backend (built from source, bundled)
- `lib*.a` (onnxruntime static libs) -- ONNX inference engine
- `libkaldi-*.a` -- Kaldi decoder libs (linked even for TTS-only)

### Important: No ASR Exclusion Flag
There is **no** `SHERPA_ONNX_ENABLE_ASR=OFF` flag. The core library (`sherpa-onnx-core`) always compiles ASR code. You can only disable TTS (`SHERPA_ONNX_ENABLE_TTS=OFF`), not ASR. The ASR code links in but doesn't increase runtime memory if you never call ASR functions.

### Link Order for Static Build
```
-lsherpa-onnx-c-api
-lsherpa-onnx-core
-lpiper_phonemize
-lespeak-ng
-lkaldi-decoder-core
-lsherpa-onnx-kaldifst-core
-lsherpa-onnx-fstfar
-lsherpa-onnx-fst
-lkaldi-native-fbank-core
-lonnxruntime  (glob of all lib*.a from onnxruntime)
-lpthread -ldl -lm
```

## 4. ONNX Runtime Bundling

**sherpa-onnx bundles its own onnxruntime. No separate build needed.**

### How It Works
1. CMake checks for pre-installed onnxruntime via `SHERPA_ONNXRUNTIME_INCLUDE_DIR` / `SHERPA_ONNXRUNTIME_LIB_DIR` env vars
2. If not found, **downloads pre-built static libs** from GitHub/HuggingFace
3. For Linux x86_64 static: `onnxruntime-linux-x64-static_lib-1.23.2-glibc2_17.zip`
4. Contains multiple `.a` files in `lib/` directory, collected via glob

### Version
- ONNX Runtime **1.23.2** (as of sherpa-onnx 1.12.28)
- Built for **glibc 2.17** (CentOS 7 era)

### CRITICAL: musl Compatibility Issue
The pre-built onnxruntime static libraries are built against **glibc 2.17**. They will NOT work with musl libc (used by Llamaste's Buildroot system).

**Options for Llamaste**:
1. **Build onnxruntime from source with musl** -- Complex but possible. Community script at `RapidAI/OnnxruntimeBuilder/build-onnxruntime-musl.sh` exists
2. **Link sherpa-onnx as a shared library** -- Build sherpa-onnx with glibc on the host, ship as .so, rely on dynamic linker
3. **Switch Buildroot to glibc** -- Would require `BR2_TOOLCHAIN_BUILDROOT_GLIBC=y` instead of musl
4. **Use Alpine Linux chroot** -- Alpine has onnxruntime in its edge repo

**Recommendation for Llamaste**: Since we already use dynamic linking for libcurl (LLAMASTE_STATIC=OFF), we could build sherpa-onnx as a shared library (.so) with glibc and ship it alongside. Alternatively, building onnxruntime from source in Buildroot with musl is the cleanest but most complex option.

## 5. Voice Model Files

### Recommended: en_US-lessac-medium (Single Speaker, English)

**HuggingFace**: `https://huggingface.co/csukuangfj/vits-piper-en_US-lessac-medium`

| File | Size | Required |
|------|------|----------|
| `en_US-lessac-medium.onnx` | 63.2 MB | YES |
| `en_US-lessac-medium.onnx.json` | 4.89 KB | YES |
| `tokens.txt` | 921 B | YES |
| `espeak-ng-data/` | ~18 MB | YES |
| **Total** | **~81 MB** | |

### Alternative: en_US-libritts_r-medium (904 Speakers)
- Model: 75 MB
- Multi-speaker (can choose voices via `sid` parameter)
- Same espeak-ng-data requirement

### Alternative (Smaller): MATCHA-TTS
- Model: <10 MB
- Requires separate vocoder model
- Apache 2.0 license
- Lower quality than Piper VITS

### Download URL Pattern (tar.bz2, all files bundled)
```
https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-en_US-lessac-medium.tar.bz2
```

### espeak-ng-data Contents
- ~50+ language dictionary files (en_dict = 167 KB)
- `lang/` subdirectory with language configs
- `voices/` subdirectory with voice configs
- `intonations` file (2 KB)
- Total: ~18 MB (all languages; could strip to English-only ~200 KB)

## 6. Memory Usage at Runtime

### Measured Benchmarks (from community/docs)
| Config | RAM Usage |
|--------|-----------|
| Piper VITS TTS on Raspberry Pi 4 | ~180 MB |
| Piper standalone (documented minimum) | <256 MB |
| Piper INT8 quantized (Android) | ~65% less than FP32 |
| Full stack (Kokoro/Matcha TTS + STT) | ~1.5 GB |

### Breakdown Estimate for Llamaste
- ONNX Runtime engine overhead: ~30-50 MB
- Model in memory (lessac-medium): ~63 MB (model weights)
- espeak-ng phonemizer: ~5 MB
- Inference buffers: ~10-20 MB per utterance
- **Total estimated**: ~120-180 MB for TTS only

### Comparison with Current Flite
- Flite (cmu_us_kal): ~13 MB total overhead, 8 kHz output
- sherpa-onnx + Piper: ~150 MB total, 22050 Hz output, vastly better quality

### Impact on Llamaste
- 4 GB RAM tier: 150 MB for TTS leaves plenty for 1.5B LLM (~1.5 GB GGUF)
- 2 GB RAM tier: Tight. Would need MATCHA-TTS (<10 MB model) or no TTS
- 1 GB RAM tier: No room for neural TTS

## 7. RTF Benchmarks (Real-Time Factor)

### en_US-lessac-medium on Raspberry Pi 4
| Threads | RTF |
|---------|-----|
| 1 | 0.774 |
| 2 | 0.482 |
| 3 | 0.390 |
| 4 | 0.357 |

RTF < 1.0 means faster than real-time. On x86_64 with 2+ cores, expect RTF ~0.1-0.2 (much faster).

## 8. Static Build Example (x86_64 Linux, glibc)

```bash
# Clone
git clone https://github.com/k2-fsa/sherpa-onnx
cd sherpa-onnx
mkdir build && cd build

# Configure for static TTS-only
cmake -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DSHERPA_ONNX_ENABLE_TTS=ON \
  -DSHERPA_ONNX_ENABLE_C_API=ON \
  -DSHERPA_ONNX_ENABLE_BINARY=ON \
  -DSHERPA_ONNX_ENABLE_PYTHON=OFF \
  -DSHERPA_ONNX_ENABLE_TESTS=OFF \
  -DSHERPA_ONNX_ENABLE_CHECK=OFF \
  -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
  -DSHERPA_ONNX_ENABLE_JNI=OFF \
  -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
  -DSHERPA_ONNX_ENABLE_GPU=OFF \
  -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
  -DSHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY=ON \
  ..

# Build
make -j$(nproc)

# Result: bin/sherpa-onnx-offline-tts (static binary, ~30-40 MB)
# Libraries: lib/libsherpa-onnx-c-api.a, lib/libsherpa-onnx-core.a, etc.

# Test
./bin/sherpa-onnx-offline-tts \
  --vits-model=/path/to/en_US-lessac-medium.onnx \
  --vits-tokens=/path/to/tokens.txt \
  --vits-data-dir=/path/to/espeak-ng-data \
  --output-filename=test.wav \
  "Hello, this is a test of sherpa onnx text to speech."
```

### Note on GCC version
Requires GCC >= 11 for static linking. GCC <= 10 needs `BUILD_SHARED_LIBS=ON` due to onnxruntime static lib compatibility issues.

## 9. Integration Strategy for Llamaste

### Option A: Shared Library (Recommended for speed)
1. Add sherpa-onnx as a Buildroot package (like llama-server)
2. Build with glibc toolchain as shared libs
3. Ship libsherpa-onnx-c-api.so + onnxruntime.so
4. Link dynamically from llamaste binary
5. **Problem**: Llamaste uses musl, not glibc

### Option B: Build onnxruntime from source with musl
1. Create Buildroot package for onnxruntime (build from source with musl)
2. Create Buildroot package for sherpa-onnx that uses the musl-built onnxruntime
3. Static link everything
4. **Cleanest** but most complex (~days of build debugging)

### Option C: Dynamic linking with glibc compat
1. Build sherpa-onnx with host glibc as a single .so
2. Ship glibc + libstdc++ alongside (like we do for libcurl already)
3. Runtime: dlopen() the .so when TTS is needed
4. **Pragmatic** -- similar to current libcurl approach

### Option D: Switch Buildroot to glibc
1. Change `BR2_TOOLCHAIN_BUILDROOT_GLIBC=y`
2. Rebuild everything
3. Larger rootfs (~5-10 MB more)
4. **Simplest** but touches entire build system

### Recommendation
**Option D** (switch to glibc) is simplest if the rootfs size increase is acceptable. We already dynamically link libcurl/liblzma/ICU, so musl purity is already broken. glibc would solve sherpa-onnx, onnxruntime, AND any future library compatibility issues.

If staying on musl, **Option B** is the right approach but requires building onnxruntime from source in Buildroot (possible with the RapidAI musl build script as reference).

## 10. Dependencies Built from Source by sherpa-onnx

| Dependency | How Obtained | License |
|---|---|---|
| onnxruntime 1.23.2 | Pre-built download (static .a) | MIT |
| piper-phonemize | FetchContent (built from source) | MIT |
| espeak-ng | FetchContent (built from source, fork) | GPL-3.0 |
| kaldi-native-fbank | FetchContent | Apache 2.0 |
| kaldi-decoder | FetchContent | Apache 2.0 |
| cargs | FetchContent (for CLI examples) | MIT |

### License Note
**espeak-ng is GPL-3.0**. Since Llamaste links it statically, the resulting binary would be GPL-3.0 unless espeak-ng is loaded as a shared library. This is the same situation as with whisper.cpp (MIT) and llama.cpp (MIT). Need to verify if Piper models can work without espeak-ng (they cannot -- espeak-ng is required for phonemization).

**Mitigation**: Ship espeak-ng as a separate shared library (.so) and dlopen() it. Or accept GPL-3.0 for the TTS component.

## Sources
- sherpa-onnx GitHub: https://github.com/k2-fsa/sherpa-onnx
- C API header: https://github.com/k2-fsa/sherpa-onnx/blob/master/sherpa-onnx/c-api/c-api.h
- Linux build docs: https://k2-fsa.github.io/sherpa/onnx/install/linux.html
- VITS models: https://k2-fsa.github.io/sherpa/onnx/tts/pretrained_models/vits.html
- Piper model (HuggingFace): https://huggingface.co/csukuangfj/vits-piper-en_US-lessac-medium
- onnxruntime musl build: https://github.com/RapidAI/OnnxruntimeBuilder
- Alpine onnxruntime package: https://pkgs.alpinelinux.org/package/edge/community/aarch64/onnxruntime
