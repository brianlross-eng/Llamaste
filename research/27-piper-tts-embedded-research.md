# Piper TTS for Embedded musl Linux — Research

**Date**: 2026-03-05
**Purpose**: Evaluate Piper TTS and alternatives for embedding in Llamaste (musl libc, no glibc, x86_64, PID 1, minimal Linux)

---

## 1. Piper Pre-Built Binaries

### Latest Release: 2023.11.14-2

**NOTE: The original rhasspy/piper repository was archived on 2025-10-06. Development moved to [OHF-Voice/piper1-gpl](https://github.com/OHF-Voice/piper1-gpl) (GPL-3.0 license, was MIT).**

Pre-built release assets (from GitHub releases):

| Asset | Size |
|-------|------|
| `piper_linux_x86_64.tar.gz` | 25.2 MB |
| `piper_linux_aarch64.tar.gz` | 24.8 MB |
| `piper_linux_armv7l.tar.gz` | 24.3 MB |
| `piper_macos_aarch64.tar.gz` | 18.3 MB |
| `piper_windows_amd64.zip` | 21.4 MB |

Downloads: 84,356 for linux_x86_64 (very popular).

### Linking: glibc, NOT static

The pre-built binaries are built on **Debian Bullseye** (glibc-based). They are **dynamically linked** against glibc with `RPATH=$ORIGIN` so shared libs are found relative to the binary.

The tarball bundles these shared libraries alongside the `piper` binary:
- `libespeak-ng.so` (phonemization)
- `libpiper_phonemize.so` (Piper's phoneme processing layer)
- `libonnxruntime.so` (ONNX Runtime — the neural network inference engine)
- `libfmt.so` (formatting)
- `libspdlog.so` (logging)
- `libstdc++.so` / `libgcc_s.so` (C++ runtime)

**These binaries WILL NOT work on musl-based systems.** The glibc and musl dynamic linkers are incompatible — a glibc-linked binary fails with "No such file or directory" on musl because `/lib64/ld-linux-x86-64.so.2` doesn't exist.

---

## 2. Runtime Dependencies

Piper's dependency chain:

```
piper binary
├── libonnxruntime.so (~15-17 MB) — ONNX neural network inference
├── libpiper_phonemize.so — text-to-phoneme conversion
│   └── libespeak-ng.so (~512K stripped) — phoneme generation
├── espeak-ng-data/ (~2 MB) — language data files for espeak-ng
├── libspdlog.so — structured logging
├── libfmt.so — string formatting
├── libstdc++.so / libgcc_s.so — C++ runtime
└── voice model (.onnx) — 60-130 MB per voice
```

The critical heavyweight dependency is **ONNX Runtime** (~15-17 MB shared lib). This is what makes Piper sound natural — it runs a VITS neural network for speech synthesis, far superior to formant synthesis.

---

## 3. CLI Interface

Piper reads text from **stdin** (one line at a time) and produces audio output. Four output modes:

### File output (default)
```bash
echo "Hello world" | piper -m model.onnx -f output.wav
```

### Stdout WAV output (pipe to player)
```bash
echo "Hello world" | piper -m model.onnx -f - | aplay
```

### Raw PCM output (streaming, lowest latency)
```bash
echo "Hello world" | piper -m model.onnx --output-raw | aplay -r 22050 -f S16_LE -c 1
```

### Directory output (auto-named files)
```bash
echo "Hello world" | piper -m model.onnx -d /tmp/audio/
# Creates /tmp/audio/<timestamp>.wav
```

### JSON input mode
```bash
echo '{"text": "Hello", "speaker_id": 0}' | piper -m model.onnx --json-input -f -
```

### Key CLI flags:
- `-m FILE` / `--model FILE` — path to .onnx model
- `-c FILE` / `--config FILE` — model config JSON (default: model.onnx.json)
- `-f FILE` / `--output_file FILE` — output WAV ('-' for stdout)
- `--output_raw` — raw 16-bit PCM to stdout (streaming)
- `-s NUM` / `--speaker NUM` — speaker ID for multi-speaker voices
- `--length_scale NUM` — speed control (1.0 = normal, <1 = faster)
- `--noise_scale NUM` — audio quality variance (default 0.667)
- `--espeak_data DIR` — path to espeak-ng-data/
- `--json-input` — JSON lines input mode
- `-q` / `--quiet` — disable logging

---

## 4. Audio Output Format

| Property | Value |
|----------|-------|
| Encoding | PCM (Pulse Code Modulation) |
| Bit depth | 16-bit signed integer (int16) |
| Channels | 1 (Mono) |
| Sample rate | Model-dependent (see below) |
| File format | WAV (standard RIFF) or raw PCM |

### Sample rates by quality level:
- **low quality**: 16,000 Hz (16 kHz)
- **medium quality**: 22,050 Hz (22 kHz)
- **high quality**: 22,050 Hz (22 kHz)

The sample rate is defined in the model's `.onnx.json` config file under `audio.sample_rate`.

---

## 5. Voice Models

All models are ONNX format. Available at: https://huggingface.co/rhasspy/piper-voices

### English voices sorted by model size:

| Voice | Quality | Size | Speakers |
|-------|---------|------|----------|
| en_US-sam-medium | medium | 60.0 MB | 1 |
| en_GB-alan-low | low | 60.2 MB | 1 |
| en_US-amy-low | low | 60.2 MB | 1 |
| en_US-lessac-low | low | 60.3 MB | 1 |
| en_US-lessac-medium | medium | 60.3 MB | 1 |
| en_US-amy-medium | medium | 60.3 MB | 1 |
| en_US-ryan-low | low | 60.2 MB | 1 |
| en_US-ryan-medium | medium | 60.3 MB | 1 |
| en_US-hfc_female-medium | medium | 60.3 MB | 1 |
| en_US-hfc_male-medium | medium | 60.3 MB | 1 |
| en_GB-vctk-medium | medium | 73.4 MB | 109 |
| en_US-libritts_r-medium | medium | 74.9 MB | 904 |
| en_US-lessac-high | high | 108.6 MB | 1 |
| en_US-ryan-high | high | 115.2 MB | 1 |
| en_US-libritts-high | high | 130.3 MB | 904 |

**Smallest useful models**: ~60 MB. There is very little size difference between low and medium quality — the model architecture is the same, only the training data/sample rate differs. Low = 16kHz output, medium = 22kHz.

Each model needs two files:
1. `model.onnx` (60-130 MB) — the neural network
2. `model.onnx.json` (~5 KB) — config with phoneme map, sample rate, etc.

Plus shared data:
3. `espeak-ng-data/` directory (~2 MB) — language/phoneme rules

**Total minimum footprint for Piper**: ~60 MB model + ~25 MB engine + ~2 MB espeak data = **~87 MB**

---

## 6. musl Compatibility Options

### Option A: Build Piper from source with musl (HARD)

Building Piper with musl requires:
1. Building ONNX Runtime with musl (~15-17 MB) — **no official support**, glibc assumptions in ORT source, community patches needed
2. Building espeak-ng with musl (straightforward, it's C99)
3. Building piper-phonemize with musl (C++, depends on espeak-ng + ORT)
4. Building Piper itself (C++17, depends on all above)

**Verdict**: Very painful. ONNX Runtime is the bottleneck — large C++ project with glibc-specific code paths.

### Option B: Use sherpa-onnx instead of Piper directly (RECOMMENDED)

[sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) (v1.12.28, active development) is a unified speech toolkit that:
- **Runs Piper VITS models directly** — same voice quality, same models
- **Bundles ONNX Runtime internally** — no separate ORT dependency
- **Supports static linking by default on Linux** (CMake `BUILD_SHARED_LIBS=OFF`)
- **Supports embedded Linux** (ARM, RISC-V, x86_64)
- **Has espeak-ng built in** for phonemization
- **Active project** with regular releases (unlike archived Piper)

Pre-built release sizes (v1.12.28):

| Variant | Size (compressed) |
|---------|-------------------|
| linux-x64-shared (with TTS) | 28.2 MB |
| linux-x64-shared (no TTS) | 23.8 MB |
| linux-x64-static (with TTS) | 321.4 MB |
| linux-x64-static (no TTS) | 282.7 MB |

**WARNING**: The static builds are 321 MB compressed because they statically link ALL of ONNX Runtime + debug info. The shared builds are 28 MB because `libonnxruntime.so` is ~17 MB dynamically linked.

**Key concern**: These pre-built static binaries are built with glibc. For musl, you must build from source. But sherpa-onnx's build system is better set up for this than raw Piper.

CMake options for minimal TTS-only build:
```bash
cmake -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=OFF \
      -DSHERPA_ONNX_ENABLE_TTS=ON \
      -DSHERPA_ONNX_ENABLE_BINARY=ON \
      -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
      -DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
      -DSHERPA_ONNX_LINK_LIBSTDCPP_STATICALLY=ON \
      ..
```

CLI usage with Piper models:
```bash
sherpa-onnx-offline-tts \
  --vits-model=./en_US-amy-low.onnx \
  --vits-tokens=./tokens.txt \
  --vits-data-dir=./espeak-ng-data \
  --output-filename=./output.wav \
  "Hello, I am Llamaste."
```

### Option C: Use Piper as external process with bundled libs (PRAGMATIC)

Since Piper bundles all its `.so` files alongside the binary with `RPATH=$ORIGIN`, we could:
1. Include the entire `piper_linux_x86_64.tar.gz` contents (25 MB) on the DATA partition
2. Spawn Piper as a subprocess via `fork/exec`
3. Use `LD_LIBRARY_PATH` to point to its bundled libs
4. Pipe text to stdin, capture WAV/PCM from stdout

**Problem**: This only works if the kernel has glibc's dynamic linker available. On a musl-only system (like Llamaste), this WILL NOT work unless we also include `ld-linux-x86-64.so.2` and glibc itself — which defeats the purpose.

**Verdict**: Not viable for a pure musl system.

---

## 7. Simpler Alternatives (No ONNX Runtime)

### espeak-ng (Formant Synthesis)

- **Size**: ~2 MB total (binary + all language data), ~512 KB stripped library
- **Quality**: Robotic/synthetic — formant-based, NOT neural
- **musl**: Native support, Alpine Linux packages exist, pure C99
- **Static build**: Trivial with `-static` flag
- **Output**: 22050 Hz, 16-bit mono PCM/WAV
- **License**: GPL-3.0
- **Build**: autotools-based, cross-compilation supported
- **Verdict**: Tiny and trivial to embed, but sounds robotic. Good for system alerts, bad for reading LLM responses aloud.

### CMU Flite (Concatenative/Unit Selection)

- **Size**: ~2-3 MB total (core 60K + US English 100K + lexicon 600K + diphone voice 1.8M)
- **Quality**: Better than espeak-ng, still noticeably synthetic. Not natural.
- **musl**: Pure ANSI C, no external dependencies, static linking is the default build mode
- **Static build**: Default behavior, compiles voices into the binary as const data
- **Output**: 8000 or 16000 Hz, 16-bit mono
- **License**: BSD-style (very permissive)
- **Performance**: 70x real-time on desktop, ~10x on embedded ARM
- **Runtime memory**: Under 1 MB
- **Verdict**: Even smaller than espeak-ng, completely self-contained, but lower quality. Good fallback option.

### Quality Comparison (subjective)

```
Piper (VITS neural) ████████████████████ (excellent, near-human)
espeak-ng (formant)  ████████             (robotic, functional)
Flite (diphone)      ██████               (slightly better clarity than espeak, still synthetic)
```

---

## 8. Recommended Strategy for Llamaste

### Tier 1: Immediate / Phase 3 (Minimum Viable Voice)

**Use espeak-ng** as the initial TTS engine:
- Trivial to add to Buildroot (`BR2_PACKAGE_ESPEAK=y` or custom package)
- ~2 MB total footprint
- Works perfectly with musl static build
- Pure C, no C++ dependencies
- Good enough for system notifications, error messages, short responses
- Can be used as phonemizer for future Piper integration

### Tier 2: Phase 3+ (Natural Voice)

**Build sherpa-onnx from source** in Buildroot with musl:
- Use TTS-only build flags to minimize size
- Statically link everything
- Run Piper VITS models for natural speech
- Expected binary: ~15-30 MB stripped (estimate for TTS-only static musl build)
- Model: 60 MB additional (en_US-amy-low or en_US-lessac-low)
- espeak-ng-data: 2 MB additional (can share with Tier 1)
- **Total: ~77-92 MB for natural speech**

### Architecture for Llamaste

```
llamaste binary (PID 1)
  ├── espeak-ng (compiled in or forked process) — fast, robotic fallback
  └── sherpa-onnx-offline-tts (forked process) — natural voice, optional
       ├── loads en_US-amy-low.onnx from /data/models/tts/
       ├── reads espeak-ng-data from /data/models/tts/espeak-ng-data/
       └── outputs 16-bit mono PCM at 16kHz to stdout
```

Integration pattern:
1. LLM generates response text
2. llamaste pipes text to TTS process
3. TTS outputs raw PCM audio
4. llamaste plays via ALSA (`/dev/snd/` or `/dev/dsp`)
5. Web UI can also request audio via `/llamaste/tts?text=...` endpoint

### Build Integration Notes

For Buildroot:
- espeak-ng: Create `br2-external/package/espeak-ng/` package, autotools build
- sherpa-onnx: Create `br2-external/package/sherpa-onnx/` package, CMake build
  - Must add ONNX Runtime as subproject (sherpa-onnx handles this)
  - Set `SHERPA_ONNX_ENABLE_TTS=ON`, disable everything else
  - Kernel needs ALSA support for audio playback

### Audio Playback Requirements

Kernel config additions needed:
```
CONFIG_SOUND=y
CONFIG_SND=y
CONFIG_SND_PCM=y
CONFIG_SND_INTEL_DSP_CONFIG=y  # for Intel HDA
CONFIG_SND_HDA_INTEL=y         # common on x86
CONFIG_SND_HDA_CODEC_REALTEK=y # common codec
CONFIG_SND_USB_AUDIO=y          # USB audio
CONFIG_SND_VIRTIO=y             # VirtualBox/QEMU
```

---

## 9. License Summary

| Component | License | Compatible with Llamaste? |
|-----------|---------|---------------------------|
| Piper (original) | MIT | Yes |
| Piper1-gpl (new fork) | GPL-3.0 | Depends on distribution model |
| Piper voice models | MIT | Yes |
| ONNX Runtime | MIT | Yes |
| sherpa-onnx | Apache-2.0 | Yes |
| espeak-ng | GPL-3.0 | Yes (separate process OK) |
| Flite | BSD | Yes |

**Note**: espeak-ng is GPL-3.0. If linking statically into the Llamaste binary, the entire binary would need to be GPL-3.0. Running espeak-ng as a separate forked process avoids this (process-level separation is not "linking" under GPL).

sherpa-onnx bundles espeak-ng internally for its phonemizer, so a statically-linked sherpa-onnx binary would carry GPL-3.0 obligations from espeak-ng.

---

## 10. Key Decisions Needed

1. **espeak-ng first, Piper later** vs **Piper from day one** — espeak-ng is 100x simpler to integrate but sounds robotic
2. **sherpa-onnx vs raw Piper build** — sherpa-onnx has better build system, active maintenance, and runs same models
3. **Static binary vs forked process** — forking a separate TTS process is simpler and avoids GPL contamination
4. **Voice model download** — 60 MB voice models should go on DATA partition, downloadable like LLM models
5. **GPL implications** — if TTS runs as separate process, main Llamaste binary stays Apache-2.0 compatible

---

## Sources

- [Piper GitHub (archived)](https://github.com/rhasspy/piper)
- [Piper1-GPL (active fork)](https://github.com/OHF-Voice/piper1-gpl)
- [Piper voice models](https://huggingface.co/rhasspy/piper-voices)
- [sherpa-onnx GitHub](https://github.com/k2-fsa/sherpa-onnx)
- [sherpa-onnx Piper integration docs](https://k2-fsa.github.io/sherpa/onnx/tts/piper.html)
- [espeak-ng GitHub](https://github.com/espeak-ng/espeak-ng)
- [CMU Flite GitHub](https://github.com/festvox/flite)
- [ONNX Runtime build docs](https://onnxruntime.ai/docs/build/inferencing.html)
- [Alpine Linux piper-tts package](https://pkgs.alpinelinux.org/package/edge/testing/armv7/piper-tts)
- [Alpine Linux espeak-ng package](https://pkgs.alpinelinux.org/package/v3.21/community/x86/espeak-ng)
