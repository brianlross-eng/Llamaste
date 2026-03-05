# Llamaste Project -- Session Status

**Last updated**: 2026-03-05 (Voice I/O Phase 3a-2, ISO rebuild)

---

## Where We Are

### Phase 1: COMPLETE
All 12 tasks + ISO/installer done. 5/5 QEMU E2E tests. EFI boot verified.

### Phase 2: COMPLETE
All sub-phases done: Web UI, scheduler, desktop mode, inference, model download, auth, network config.

### Phase 3a: Voice I/O — IN PROGRESS

| Sub-phase | Status |
|-----------|--------|
| 3a-1: STT Foundation | DONE — whisper.cpp package, voice.h/cpp, WAV parser, HTTP endpoints, mic button |
| 3a-2: Always-Listening | DONE — ALSA capture thread, energy VAD, wake phrase detection, agent loop wiring |
| 3a-3: Piper TTS | NOT STARTED — ONNX Runtime musl build (high risk), espeak-ng, GPL isolation |
| 3a-4: Web UI Voice | PARTIAL — mic button exists, needs TTS audio playback |

**Key design decision**: Voice pipeline only initializes in desktop mode (saves ~200MB RAM on headless server).

---

## Latest Session (2026-03-05)

### Features Added
1. **Always-listening voice pipeline** (voice.cpp):
   - ALSA capture: 480 frames (30ms) at 16kHz mono S16_LE
   - Energy-based VAD: RMS threshold (0.01), speech onset/offset detection
   - 300ms silence = end of utterance, min 0.5s speech, max 30s recording
   - Wake phrase: case-insensitive "llamaste" match in whisper transcription
   - Command extraction: text after wake phrase routed to agent loop
   - ALSA error recovery (overrun handling)

2. **Agent loop integration** (child_main.cpp):
   - Voice commands go through same `agent_turn()` as HTTP chat
   - TODO: Phase 3a-3 will pipe agent response to Piper TTS

3. **Enhanced audio status** (tools_audio.cpp):
   - New fields: `always_listening`, `alsa_device`, `silence_ms`, `last_error`

4. **Desktop-only voice** (child_main.cpp):
   - Voice pipeline gated on `g_boot_mode == "desktop"`
   - Server mode logs "voice pipeline disabled" and skips whisper model loading

### Build Fixes
- **ALSA PCM headers missing**: alsa-lib needed rebuild after PCM config enabled (pcm.h not in staging)
- **alsa-utils removed**: musl cross-compile issues, only alsa-lib (C API) needed
- **Config.in selects**: Added `select BR2_PACKAGE_ALSA_LIB` and `select BR2_PACKAGE_WHISPER_CPP`

### ISO Rebuilt
- 964 MB ISO with all fixes: voice I/O, CA certs, DNS, --jinja, network config

---

## Known Issues

### WSL2 localhost access
- VirtualBox port forwarding doesn't work from WSL2 `localhost`
- Use `172.18.208.1:8080` instead

### Voice pipeline untested end-to-end
- Whisper model download + ALSA capture not yet tested with real microphone
- VirtualBox AC97 audio enabled but whisper model needs to be downloaded first

---

## Next Steps

### Immediate
1. Test voice pipeline end-to-end (download whisper model, test with VBox mic)
2. Phase 3a-3: Piper TTS (ONNX Runtime musl build — high risk)
3. Phase 3a-4: Web UI TTS audio playback

### Future
4. Inference speed optimization (try 0.5B model, context tuning)
5. Phase 4: A/B update mechanism, Ed25519 signing, USB sideload

---

## Build & Boot Summary

| Artifact | Size | Details |
|----------|------|---------|
| llamaste binary | 4.9 MB | Dynamic ELF, x86-64, musl + whisper.cpp + ALSA |
| bzImage kernel | 7.5 MB | Built-in DRM/GPU/audio drivers, no modules |
| rootfs.squashfs | 78 MB | llamaste + WPEWebKit + Mesa + Wayland + whisper + alsa-lib |
| llamaste.img | 611 MB | 5-partition GPT disk image |
| llamaste.iso | 964 MB | Live ISO with installer |
| Boot time | ~2 seconds | Kernel → HTTP server ready |
| Model load | ~6 seconds | Qwen2.5-1.5B-Instruct Q4_K_M |

---

## Source Summary

~9,000 LOC original C++ + ~45KB web UI:
- main.cpp, supervisor.cpp, init.cpp, hwdetect.cpp, child_main.cpp
- agent.cpp, prompt_builder.cpp
- tools.cpp + 11 tool files (fs, process, network, system, config, model, model_download, install, schedule, auth, audio)
- voice.h, voice.cpp
- bcrypt.cpp, auth.cpp
- net_mdns.cpp, scheduler.cpp
- Web UI: index.html, login.html, setup.html, chat.js, dashboard.js, files.js, system.js, notifications.js, install.js, style.css

## Test Suites
10 suites, ~138 tests: hwdetect, tools, agent, integration, http, mdns, auth, inference, model_download, audio

## VirtualBox VM
- **VM Name**: "Llamaste", Location: `D:\Llamaste\vm\Llamaste\`
- 4 GB RAM, 2 CPUs, EFI64, VMSVGA, AC97 audio, NAT (host 8080 -> guest 80)
- SATA port 0: `llamaste-disk.vdi` (16 GB)
- Access from Windows: `http://localhost:8080`
- Access from WSL2: `http://172.18.208.1:8080`

## Build Commands (WSL2)
```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin && export FORCE_UNSAFE_CONFIGURE=1 && cd /root/llamaste-build/output && make llamaste-dirclean && make llamaste && make"
```
