# Phase 3a: Voice I/O — Always-Listening Appliance

**Date**: 2026-03-05
**Status**: Approved
**Phase**: 3a (first Phase 3 feature)

## Context

Llamaste is a bootable LLM-OS where the LLM is the operating system. Phase 2 is complete: web UI, inference, model download, auth, network config, install flow all working. The next step is Voice I/O — turning Llamaste into an always-listening appliance you can talk to. This is the highest-impact Phase 3 feature.

**User choices**: Always-listening appliance, Piper TTS, openWakeWord (but research shows VAD+whisper is simpler for Phase 3a).

---

## Architecture

### Audio Pipeline

```
Microphone -> ALSA capture (16kHz mono PCM)
    -> Silero VAD (0.4% CPU, always running)
    -> Speech detected? -> whisper.cpp tiny.en (STT)
    -> Contains wake phrase? -> Feed to LLM agent
    -> LLM response -> Piper TTS -> ALSA playback -> Speaker
```

### Component Summary

| Component | Purpose | Size | License | Build |
|-----------|---------|------|---------|-------|
| whisper.cpp | Speech-to-text | 2-5MB bin + 31MB model | MIT | Link into llamaste binary (C API) |
| Silero VAD | Voice activity detection | 1.8MB model | MIT | Bundled with whisper.cpp |
| Piper TTS | Text-to-speech | 2MB bin + 63MB model | GPL (espeak-ng) | **Separate process** (GPL isolation) |
| ONNX Runtime | Neural net inference (Piper) | 12MB lib | MIT | Buildroot package |
| espeak-ng | Phonemizer (Piper dep) | 4MB | GPL | Buildroot package |
| alsa-lib | Audio capture/playback | 500KB | LGPL | Buildroot package |

**Total disk**: ~115MB additional (binaries + models). SYS-A partition is 256MB, current squashfs is 77MB. Fits.

**Total RAM**: ~280MB additional (whisper ~200MB + piper/onnx ~80MB). With 4GB system: kernel(50) + llamaste(30) + LLM(1500) + voice(280) = 1860MB. Leaves ~2GB for KV cache. Workable. **Voice I/O requires 4GB+ RAM.**

### Key Design Decisions

1. **whisper.cpp linked into llamaste binary** — MIT license, C API, avoids subprocess overhead. Use `whisper_full()` directly with ALSA-captured PCM buffers. Thread runs continuously.

2. **Piper runs as separate process** — GPL (espeak-ng dependency). Communicate via stdin/stdout pipes. Spawn on demand, keep alive for session. Same pattern as llama-server but simpler (no HTTP, just pipes).

3. **VAD-first, not wake-word-first** — Silero VAD is bundled with whisper.cpp (0.4% CPU idle). Only runs whisper inference when speech detected. Check transcription for wake phrase ("Hey Llamaste" or "Llamaste"). Simpler than adding LOWWI/openWakeWord initially.

4. **ALSA only, no PulseAudio** — Direct hardware access via alsa-lib. Minimal, no daemon. VirtualBox AC'97 controller for dev/test.

5. **Web UI gets voice too** — Push-to-talk button in chat.js using WebAudio API + MediaRecorder. Uploads WAV to `/llamaste/audio/transcribe`. TTS responses streamed as audio via `/llamaste/audio/speak`. This works even without hardware mic (browser mic).

---

## New Files

### C++ Source
- `src/llamaste/voice.h` — Voice pipeline state, thread management
- `src/llamaste/voice.cpp` — ALSA capture, VAD, whisper inference, wake detection, TTS pipe
- `src/llamaste/tools_audio.cpp` — `audio.transcribe`, `audio.speak`, `audio.status` tools

### Buildroot Packages
- `br2-external/package/whisper-cpp/` — whisper.cpp static library
- `br2-external/package/piper-tts/` — Piper binary + espeak-ng + onnxruntime
- Enable: `BR2_PACKAGE_ALSA_LIB=y`, `BR2_PACKAGE_ALSA_UTILS=y` (for dev testing)

### Kernel Config
- `CONFIG_SOUND=y`, `CONFIG_SND=y` (or =m)
- `CONFIG_SND_INTEL8X0=y` — VirtualBox AC'97
- `CONFIG_SND_HDA_INTEL=y` — Real hardware Intel HDA
- `CONFIG_SND_USB_AUDIO=y` — USB microphones

### Web UI
- `chat.js` — Add microphone button, WebAudio recording, audio playback
- `style.css` — Mic button styling

### Models (downloaded to /data/models/ at first use)
- `ggml-tiny.en-q5_1.bin` — 31MB whisper model
- `silero_vad.onnx` — 1.8MB VAD model (bundled in squashfs)
- `en_US-amy-low.onnx` + `en_US-amy-low.onnx.json` — 63MB Piper voice

---

## HTTP Endpoints

| Route | Method | Purpose |
|-------|--------|---------|
| `/llamaste/audio/transcribe` | POST | Upload WAV/PCM, return transcription |
| `/llamaste/audio/speak` | POST | Text -> TTS, return audio/wav |
| `/llamaste/audio/status` | GET | Voice pipeline state (listening, wake detected, etc.) |
| `/llamaste/audio/config` | GET/POST | Voice settings (wake phrase, TTS voice, enabled) |

---

## Voice Pipeline Thread (voice.cpp)

```
voice_thread():
  1. Open ALSA capture device (hw:0,0), 16kHz, mono, S16_LE
  2. Load Silero VAD model (1.8MB, runs on CPU)
  3. Load whisper.cpp tiny.en model (31MB)
  4. Loop forever:
     a. Read 30ms audio chunk from ALSA (480 samples)
     b. Feed to Silero VAD
     c. If speech probability > 0.5:
        - Accumulate audio into buffer
        - Continue until silence detected (300ms of no speech)
     d. When speech segment complete:
        - Run whisper_full() on accumulated buffer
        - Get transcription text
        - Check for wake phrase ("llamaste" in text, case-insensitive)
        - If wake phrase found:
          * Extract command (text after wake phrase)
          * Submit to agent loop (same as HTTP /llamaste/chat)
          * Get LLM response text
          * Pipe to Piper TTS -> ALSA playback
        - If no wake phrase: discard (ambient speech)
  5. On shutdown: close ALSA, free models
```

---

## Implementation Phases

### Phase 3a-1: ALSA + whisper.cpp STT (Foundation)
- Add ALSA kernel config + alsa-lib to Buildroot
- Create whisper-cpp Buildroot package (static library)
- Build voice.cpp with ALSA capture + whisper transcription
- HTTP endpoint: POST `/llamaste/audio/transcribe` (upload WAV -> text)
- Test: record audio in VirtualBox, transcribe it

### Phase 3a-2: VAD + Always-Listening Pipeline
- Integrate Silero VAD into voice thread
- Continuous ALSA capture with VAD gating
- Wake phrase detection ("llamaste" in transcription)
- Connect to agent loop for processing commands
- Voice status reporting via `/llamaste/audio/status`

### Phase 3a-3: Piper TTS Output
- Build ONNX Runtime for musl (hardest part)
- Create piper-tts Buildroot package
- Spawn Piper as subprocess, pipe text -> PCM
- ALSA playback of TTS output
- HTTP endpoint: POST `/llamaste/audio/speak` (text -> audio)

### Phase 3a-4: Web UI Voice Integration
- Microphone button in chat.js (WebAudio + MediaRecorder)
- Audio upload to `/llamaste/audio/transcribe`
- TTS audio playback for LLM responses
- Voice status indicator in status bar

---

## Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| ONNX Runtime musl build fails | Blocks Piper TTS | Phase 3a-3 is independent; STT works without TTS. Try pre-built static binary or minimal build. |
| VirtualBox audio unreliable | Can't test locally | Use WAV file upload endpoint for testing. Audio passthrough is a nice-to-have for dev. |
| Whisper CPU cost too high | Drains CPU from LLM | VAD gates inference (only runs on speech). Tiny.en model is fast (~1s for 5s audio). |
| Voice + LLM don't fit in 4GB | OOM crashes | Make voice optional (enabled only if RAM > 4GB). Config toggle. |
| espeak-ng GPL contamination | License issue | Piper runs as separate process via fork/exec + pipes. No linking. |

---

## Verification

1. **ALSA**: `arecord -d 5 -f S16_LE -r 16000 test.wav && aplay test.wav` in VirtualBox
2. **STT**: Upload WAV to `/llamaste/audio/transcribe`, verify JSON response with text
3. **VAD**: Voice thread logs show speech detection with timestamps
4. **Wake word**: Say "Llamaste, what time is it?" -> LLM responds with time
5. **TTS**: POST text to `/llamaste/audio/speak`, verify audio plays through speakers
6. **Web UI**: Click mic button -> speak -> see transcription in chat -> hear TTS response
7. **RAM**: Check `/llamaste/system` shows adequate free memory with voice loaded
