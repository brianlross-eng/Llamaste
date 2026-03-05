# Research 27: Voice I/O Components for Llamaste

**Date**: 2026-03-05
**Context**: Phase 3 planning — voice input/output for minimal embedded Linux (musl libc, no systemd, Buildroot, x86_64 CPU-only)

---

## 1. whisper.cpp — Speech-to-Text

**Repository**: [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp)
**License**: MIT

### Model Sizes (GGML format)

| Model | Full Size | Quantized (Q5_1) | Parameters | RAM Required | WER (clean/other) |
|-------|-----------|-------------------|------------|--------------|-------------------|
| tiny.en | 75 MB | 31 MB | 39M | ~201 MB | 5.6% / 14.9% |
| base.en | 142 MB | 57 MB | 74M | ~215 MB | 4.3% / 12.8% |
| small.en | 466 MB | 182 MB | 244M | ~600 MB | 3.4% / 8.7% |

**Recommendation**: Start with **tiny.en quantized (Q5_1)** at 31 MB. Only 39M params but sufficient for well-recorded English speech. Can upgrade to base.en (57 MB Q5_1) if accuracy is insufficient.

### Dependencies

- **Zero external dependencies** — pure C/C++ (ggml library is vendored)
- SDL2 is needed ONLY for the `whisper-stream` real-time mic example
- For our use: we can use ALSA directly and pipe raw PCM via the `whisper-stream-pcm` binary or the C API

### Audio Format Requirements

- **Input**: 16-bit PCM, 16 kHz, mono
- Accepts WAV files (whisper-cli) or raw PCM via stdin (whisper-stream-pcm)
- Raw PCM formats: `f32le` (32-bit float) or `s16le` (16-bit signed int), 16 kHz mono
- No need for ffmpeg at runtime — we can capture at 16kHz directly from ALSA

### Streaming / Real-Time Transcription

Three approaches available:

1. **whisper-stream** — Uses SDL2, samples every 500ms, runs transcription continuously
2. **whisper-stream-pcm** — Reads raw PCM from stdin, no SDL2 required. Supports Silero VAD
3. **C/C++ API** — `whisper_full()` accepts pcmf32 float arrays directly in memory

For Llamaste, approach **#3 (C/C++ API)** is ideal — we can capture ALSA audio in our binary and call whisper directly, no subprocess needed.

### VAD (Voice Activity Detection)

whisper.cpp has built-in Silero VAD support:
- Silero VAD model is only **1.8 MB**
- Processes 30ms audio chunks in ~1ms on CPU
- CPU usage: **~0.4%** when running continuously in real-time
- Only passes speech segments to Whisper, dramatically reducing CPU load during silence
- Configurable: threshold (default 0.5), min speech duration (0.1s), min silence (0.5s)

### Approximate Binary Size

- whisper.cpp static build (CLI tools): estimated **2-5 MB** (pure C/C++, no heavy deps)
- The entire codebase is just whisper.h/whisper.cpp + ggml library
- Total on disk: binary (~3 MB) + model (31-75 MB) + silero VAD (1.8 MB) = **~36-80 MB**

### Buildroot Package Approach

No official Buildroot recipe exists. Create a custom `br2-external/package/whisper-cpp/`:

```makefile
# whisper-cpp.mk
WHISPER_CPP_VERSION = v1.7.5  # or latest stable tag
WHISPER_CPP_SITE = https://github.com/ggml-org/whisper.cpp
WHISPER_CPP_SITE_METHOD = git
WHISPER_CPP_LICENSE = MIT
WHISPER_CPP_INSTALL_STAGING = YES

WHISPER_CPP_CONF_OPTS = \
    -DBUILD_SHARED_LIBS=OFF \
    -DWHISPER_SDL2=OFF \
    -DWHISPER_BUILD_EXAMPLES=OFF \
    -DWHISPER_BUILD_TESTS=OFF \
    -DGGML_STATIC=ON

$(eval $(cmake-package))
```

### Gotchas for musl/no-systemd

- Pure C/C++ — no glibc-specific APIs, should build cleanly with musl
- No pthread issues expected (ggml uses pthreads which musl supports)
- No systemd dependency
- The streaming examples use SDL2 but we can bypass this entirely by using the C API
- Quantized models reduce both disk space AND inference time

---

## 2. Piper TTS — Text-to-Speech

**Repository**: [rhasspy/piper](https://github.com/rhasspy/piper) (now at [OHF-Voice/piper1-gpl](https://github.com/OHF-Voice/piper1-gpl))
**License**: **GPL** (due to espeak-ng dependency; originally labeled MIT but effectively GPL)

### Dependencies

1. **ONNX Runtime** — neural network inference engine (~5-15 MB shared lib with minimal build)
2. **espeak-ng** — text-to-phoneme conversion (GPL licensed)
3. **piper-phonemize** — wrapper library around espeak-ng

### Smallest English Voice Model

| Voice | Quality | Sample Rate | ONNX Size | Notes |
|-------|---------|-------------|-----------|-------|
| en_US-amy-low | low | 16 kHz | ~63 MB | Smallest available en_US |
| en_US-amy-medium | medium | 22.05 kHz | ~63 MB | Same model size, better audio |
| en_US-lessac-medium | medium | 22.05 kHz | ~63 MB | Good quality, commonly used |

Note: There are **no x_low en_US voices** in the official repo. The developer said the performance difference wasn't significant enough. Low and medium models are the same architecture size (~63 MB ONNX), differing only in training sample rate.

### Output Format

- `--output-raw` streams **16-bit mono PCM** to stdout at the model's native sample rate
- `--output_file audio.wav` saves a WAV file
- Can pipe directly to `aplay` for real-time playback:
  ```bash
  echo "Hello" | piper --model voice.onnx --output-raw | aplay -r 22050 -f S16_LE -t raw -
  ```

### Binary + Model Size Estimate

| Component | Size |
|-----------|------|
| piper binary (dynamically linked) | ~2-3 MB |
| ONNX Runtime shared lib (minimal) | ~10-15 MB |
| espeak-ng library + data | ~3-5 MB |
| piper-phonemize library | ~1-2 MB |
| Voice model (en_US-amy-low.onnx) | ~63 MB |
| Voice config (.onnx.json) | ~1 KB |
| **Total** | **~80-90 MB** |

### License Concern

**This is the biggest gotcha.** Piper depends on espeak-ng which is GPL. The project has acknowledged this by moving to `piper1-gpl`. Since Llamaste uses Apache 2.0 compatible licensing throughout, integrating Piper as a **separate process** (not linked into the main binary) is the safest approach. espeak-ng as a separate binary + Piper as a separate binary, communicating via pipes/stdin/stdout, avoids GPL infection of the main Llamaste binary.

Alternative: There is discussion about creating ONNX models that replicate espeak-ng's phonemization, which would eliminate the GPL dependency, but this is not yet available.

### Buildroot Package Approach

Complex due to multiple dependencies:

1. Package `onnxruntime` (CMake, ~15 MB minimal shared lib)
2. Package `espeak-ng` (autotools, ~3 MB)
3. Package `piper-phonemize` (CMake, depends on espeak-ng + onnxruntime)
4. Package `piper` (CMake, depends on piper-phonemize)

### Gotchas for musl/no-systemd

- **ONNX Runtime on musl is the hardest part** — official builds target glibc 2.31+
- Will need to cross-compile ONNX Runtime from source with musl toolchain
- ONNX Runtime is a large, complex C++ project with many optional features
- Use `--minimal_build --config MinSizeRel --disable_ml_ops --disable_exceptions` to minimize
- espeak-ng builds fine with musl (commonly used on Alpine Linux)
- No systemd dependency in any component

---

## 3. Wake Word Detection

### openWakeWord (Python-only)

**Repository**: [dscripka/openWakeWord](https://github.com/dscripka/openWakeWord)
**License**: Apache 2.0

- **Python-only** — requires Python + numpy + onnxruntime
- NOT suitable for direct integration into Llamaste (no Python runtime available)
- Models are ONNX format, so C++ inference is possible with the right wrapper

### C++ Alternatives

#### LOWWI (Lightweight OpenWakeWord Implementation)
**Repository**: [CLFML/lowwi](https://github.com/CLFML/lowwi)
**License**: Open source

- **Pure C++ implementation** compatible with openWakeWord ONNX models
- Single dependency: **ONNX Runtime**
- CMake build system with FetchContent support
- Can use any pre-trained openWakeWord model
- If we already have ONNX Runtime for Piper, LOWWI adds minimal overhead

#### Openwakeword.Cpp (Rhasspy)
**Repository**: [rhasspy/openWakeWord-cpp](https://github.com/rhasspy/openWakeWord-cpp)
- Another C++ port, also depends on ONNX Runtime
- Less actively maintained than LOWWI

#### Porcupine (Picovoice)
- Commercial (free tier available for dev)
- ANSI C precompiled library
- Very accurate (11x better than PocketSphinx)
- **NOT open source** — binary blobs, requires AccessKey
- Not suitable for Llamaste's open-source philosophy

### Size Estimates for LOWWI

| Component | Size |
|-----------|------|
| LOWWI library code | ~100 KB |
| ONNX Runtime (shared with Piper) | 0 MB (already present) |
| Wake word model (ONNX) | ~1-5 MB per wake word |
| **Total incremental** | **~1-5 MB** |

### Wake Word Model Training

openWakeWord models are trained on synthetic data. Pre-trained models include:
- "hey jarvis", "alexa", "hey mycroft", "ok google"
- Custom wake words can be trained using the openWakeWord training pipeline (Python/Colab)
- For Llamaste: train a "hey llamaste" or "namaste" wake word model

---

## 4. ALSA — Audio Capture/Playback

### Kernel Configuration Required

```
CONFIG_SOUND=y                    # Primary sound support
CONFIG_SND=m                      # ALSA sound system
CONFIG_SND_PCM=m                  # PCM support (capture + playback)
CONFIG_SND_TIMER=m                # Timer (PCM dependency)
CONFIG_SND_INTEL8X0=m             # Intel ICH AC'97 (VirtualBox AC97)
CONFIG_SND_HDA_INTEL=m            # Intel HD Audio (VirtualBox HDA)
CONFIG_SND_HDA_GENERIC=m          # Generic HD Audio codec parser
CONFIG_SND_OSSEMUL=y              # OSS API emulation (optional)
CONFIG_SND_MIXER_OSS=m            # OSS mixer (optional)
CONFIG_SND_PCM_OSS=m              # OSS PCM (optional)
```

### Userspace Packages (Buildroot)

```
BR2_PACKAGE_ALSA_LIB=y           # libasound.so — the ALSA API library
BR2_PACKAGE_ALSA_LIB_PCM=y       # PCM support in alsa-lib
BR2_PACKAGE_ALSA_LIB_MIXER=y     # Mixer support
BR2_PACKAGE_ALSA_UTILS=y         # arecord, aplay, amixer, alsamixer (for testing)
```

- **No PulseAudio needed** — alsa-lib provides direct hardware access
- alsa-lib is ~500 KB shared library
- alsa-utils is ~1 MB (optional, only needed for testing/debugging)

### Direct ALSA Capture in C/C++

Yes, we can capture audio with just alsa-lib. Minimal code:

```c
#include <alsa/asoundlib.h>

snd_pcm_t *capture;
snd_pcm_open(&capture, "default", SND_PCM_STREAM_CAPTURE, 0);
snd_pcm_set_params(capture, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                   1, 16000, 1, 100000);  // mono, 16kHz, 100ms latency
snd_pcm_readi(capture, buffer, frames);
```

This gives us raw 16-bit 16kHz mono PCM — exactly what whisper.cpp needs.

### VirtualBox Audio

- **Works** with AC'97 controller (`CONFIG_SND_INTEL8X0`)
- Intel HDA controller has known mic input bugs with PulseAudio (not relevant — we use ALSA directly)
- VirtualBox emulates audio hardware; no PCI passthrough
- Must enable "Audio Input" in VM settings for mic capture
- For testing: AC'97 is most reliable for mic input
- For production: either AC'97 or Intel HDA works for output (playback)
- Test: `arecord -d 5 -f S16_LE -r 16000 -c 1 test.wav && aplay test.wav`

---

## 5. Can We Skip openWakeWord? (Whisper-as-Continuous-Listener)

### The Idea

Run whisper.cpp tiny model continuously, transcribe everything, and check the output text for a wake phrase like "llamaste" or "hey llamaste".

### CPU Cost Analysis

**With Silero VAD (recommended approach):**

| Phase | CPU Usage | Active When |
|-------|-----------|-------------|
| ALSA capture (16kHz mono) | ~0.1% | Always |
| Silero VAD processing | ~0.4% | Always (30ms chunks, 1ms each) |
| Whisper tiny.en inference | 50-100% of 1 core | Only during detected speech |
| **Idle (no speech)** | **~0.5% total** | |
| **Active speech** | **~50-100% of 1 core** | |

**Without VAD (pure Whisper continuous):**

| Phase | CPU Usage | Notes |
|-------|-----------|-------|
| Whisper tiny.en inference | 50-100% of 1 core | Continuously, even during silence |
| Power draw | Significant | Not suitable for always-on |

### Verdict: VAD + Whisper is Viable, but LOWWI is Better

**Option A: Silero VAD + Whisper tiny (no separate wake word)**
- Pros: Simpler architecture, fewer components, no extra model
- Cons: Whisper runs on ALL speech (not just wake-word speech), higher CPU during conversations near the device, potential false-wake from similar-sounding words in conversation
- CPU idle: ~0.5%
- CPU when someone nearby is talking: 50-100% of 1 core (transcribing everything)

**Option B: LOWWI wake word + Whisper (proper wake word)**
- Pros: Only runs Whisper AFTER wake word detected, much lower CPU during ambient speech
- Cons: Extra ONNX dependency (but shared with Piper), need to train a wake word model
- CPU idle: ~0.5% (VAD-like monitoring)
- CPU when someone nearby is talking: ~1-2% (wake word detection only)
- CPU after wake word: 50-100% of 1 core (Whisper runs for command)

**Recommendation**: Use **Option A (VAD + Whisper)** for Phase 3 simplicity. We can always add LOWWI wake word detection later if CPU usage during ambient speech is a problem. The VAD ensures Whisper only runs when speech is detected, and the tiny model is fast enough for real-time on modern x86_64.

---

## 6. Overall Architecture Recommendation

### Minimal Viable Voice I/O Pipeline

```
[Microphone] → [ALSA capture 16kHz] → [Silero VAD] → [whisper.cpp tiny.en] → text
                                                                                ↓
                                                                        [agent loop]
                                                                                ↓
[Speaker] ← [ALSA playback] ← [raw PCM] ← [Piper TTS] ← text response
```

### Component Summary

| Component | Size (disk) | RAM | License | Build Difficulty |
|-----------|-------------|-----|---------|------------------|
| whisper.cpp (library) | ~3 MB | ~201 MB | MIT | Easy (pure C++) |
| whisper tiny.en Q5_1 | 31 MB | (included above) | MIT | Download only |
| Silero VAD model | 1.8 MB | ~5 MB | MIT | Download only |
| Piper binary | ~2 MB | ~80 MB | GPL | Medium |
| ONNX Runtime | ~10-15 MB | (included above) | MIT | Hard (large C++ project) |
| espeak-ng + data | ~3-5 MB | ~10 MB | GPL | Easy (autotools) |
| Voice model (low) | ~63 MB | (included above) | CC-BY-SA/MIT | Download only |
| alsa-lib | ~0.5 MB | ~1 MB | LGPL-2.1 | Easy (Buildroot has it) |
| **Total** | **~115-120 MB** | **~300 MB** | Mixed | |

### Disk Budget

- Current squashfs rootfs: 73 MB
- Voice I/O additions: ~120 MB (binary + models)
- New squashfs estimate: ~190 MB (models compress well)
- sys-a partition: 256 MB — **fits comfortably**

### RAM Budget (4 GB system)

- Linux kernel + init: ~50 MB
- Llamaste binary + web UI: ~30 MB
- LLM model (Qwen2.5-1.5B): ~1.5 GB
- Whisper tiny.en: ~200 MB
- Piper + ONNX Runtime: ~80 MB
- Voice model (loaded on demand): ~63 MB
- **Total**: ~1.9 GB — leaves ~2 GB for KV cache and system

On a 2 GB system, voice I/O would not fit alongside the LLM. Consider voice I/O as a 4 GB+ feature.

### Phased Implementation

**Phase 3a**: whisper.cpp integration (STT only)
- Add ALSA kernel config + alsa-lib to Buildroot
- Create whisper-cpp Buildroot package (easy, pure C++)
- Integrate whisper C API into llamaste binary for audio capture + transcription
- Add Silero VAD for efficient speech detection
- Ship tiny.en Q5_1 model (31 MB)

**Phase 3b**: Piper TTS integration (TTS)
- Build ONNX Runtime for Buildroot (hardest part)
- Build espeak-ng for Buildroot
- Build piper-phonemize + piper for Buildroot
- Run Piper as separate process (GPL isolation), pipe text in, PCM out
- Ship en_US-amy-low voice model (63 MB)

**Phase 3c**: Wake word (optional)
- Integrate LOWWI for dedicated wake word detection
- Train custom "llamaste" wake word model
- ONNX Runtime already available from Phase 3b

---

## 7. Key Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| ONNX Runtime won't build with musl | Blocks Piper + LOWWI | Try sherpa-onnx (designed for embedded), or use espeak-ng standalone + custom VITS inference |
| GPL license infection from Piper | Legal risk | Run Piper as separate process, not linked into main binary |
| Whisper CPU usage too high on 2 cores | Poor UX | Use quantized tiny model + VAD, or offload to background thread |
| VirtualBox mic input broken | Can't test | Use AC'97 controller, test with arecord first |
| 2 GB RAM systems can't run voice + LLM | Feature gap | Make voice I/O optional, only enable on 4 GB+ systems |
| Audio latency too high | Poor UX | Tune ALSA buffer sizes, use low-latency PCM settings |

---

## Sources

- [whisper.cpp GitHub](https://github.com/ggml-org/whisper.cpp)
- [whisper.cpp models README](https://github.com/ggml-org/whisper.cpp/blob/master/models/README.md)
- [whisper.cpp streaming README](https://github.com/ggml-org/whisper.cpp/blob/master/examples/stream/README.md)
- [whisper-stream-pcm PR](https://github.com/ggml-org/whisper.cpp/pull/3653)
- [whisper.cpp Silero VAD issue](https://github.com/ggml-org/whisper.cpp/issues/3003)
- [Piper TTS GitHub](https://github.com/rhasspy/piper)
- [Piper1-GPL (new home)](https://github.com/OHF-Voice/piper1-gpl)
- [Piper license issue](https://github.com/rhasspy/piper/issues/93)
- [Piper voice samples](https://rhasspy.github.io/piper-samples/)
- [Piper voices on HuggingFace](https://huggingface.co/rhasspy/piper-voices)
- [LOWWI (C++ openWakeWord)](https://github.com/CLFML/lowwi)
- [openWakeWord GitHub](https://github.com/dscripka/openWakeWord)
- [Openwakeword.Cpp](https://github.com/Hoog-V/Openwakeword.Cpp)
- [ONNX Runtime build docs](https://onnxruntime.ai/docs/build/inferencing.html)
- [ONNX Runtime custom build](https://onnxruntime.ai/docs/build/custom.html)
- [ONNX Runtime size issue](https://github.com/microsoft/onnxruntime/issues/6160)
- [ALSA ArchWiki](https://wiki.archlinux.org/title/Advanced_Linux_Sound_Architecture)
- [VirtualBox audio settings](https://docs.oracle.com/en/virtualization/virtualbox/6.0/user/settings-audio.html)
- [VirtualBox HDA mic bug](https://www.virtualbox.org/ticket/22347)
- [Whisper model sizes explained](https://openwhispr.com/blog/whisper-model-sizes-explained)
- [Silero VAD overview](https://medium.com/axinc-ai/silerovad-machine-learning-model-to-detect-speech-segments-e99722c0dd41)
- [Piper core TTS engine](https://deepwiki.com/rhasspy/piper/2-core-tts-engine)
- [Piper available voices](https://deepwiki.com/rhasspy/piper/3.1-available-voices)
