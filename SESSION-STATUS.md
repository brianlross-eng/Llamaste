# Llamaste Project -- Session Status

**Last updated**: 2026-03-05 (end-of-shift wrap-up: skills, docs, CLAUDE.md, GitHub push)

---

## Where We Are

### Phase 1: COMPLETE
All 12 tasks + ISO/installer done. 5/5 QEMU E2E tests. EFI boot verified.

### Phase 2: COMPLETE
All sub-phases done: Web UI, scheduler, desktop mode, inference, model download, auth, network config.

### Phase 3a: Voice I/O — COMPLETE

### Phase 3: MCP Server — COMPLETE (0d418fc)

| Sub-phase | Status |
|-----------|--------|
| 3a-1: STT Foundation | DONE — whisper.cpp package, voice.h/cpp, WAV parser, HTTP endpoints, mic button |
| 3a-2: Always-Listening | DONE — ALSA capture thread, energy VAD, wake phrase detection, agent loop wiring |
| 3a-3: Flite TTS | DONE — Flite (BSD) linked into binary, cmu_us_kal voice, ALSA playback, agent auto-speak |
| 3a-4: Web UI Voice | DONE — /audio/tts WAV endpoint, TTS toggle button, voice status indicator, error handler fix |

**Key design decision**: Voice pipeline only initializes in desktop mode (saves ~200MB RAM on headless server).

---

## Latest Session (2026-03-05) — Phase 3a Complete

### Phase 3a-3: Flite TTS
- **Flite** (BSD-4-Clause) linked directly into binary — no GPL isolation needed
- `cmu_us_kal` voice (8kHz, robotic but functional)
- `VoicePipeline::speak()` calls `flite_text_to_wave()`, plays via ALSA `snd_pcm_open(PLAYBACK)`
- `audio.speak` tool writes WAV to `/data/tmp/tts_*.wav`
- Agent command callback speaks LLM response after each voice command
- Squashfs: 91MB (up from 78MB, flite voice data) | Binary: 4.1MB (flite linked dynamically)
- Commits: `123e66c`

### Phase 3a-4: Web UI Voice Integration
- **`/llamaste/audio/tts`** — new POST endpoint, returns `audio/wav` binary for browser playback
- WAV encoding: in-memory (no disk write needed for browser path), uses `voice.speak()` + encode_wav_for_http()
- **`#tts-btn`** — speaker icon toggle button in chat input area (between mic and send)
- **Auto-speak**: after stream finishes, if TTS enabled, POSTs response text to /audio/tts and plays via `new Audio()`
- **Markdown stripping**: plain text extraction before synthesis (removes code blocks, bold, italic, headers)
- **500 char truncation** to avoid very long synthesis
- **`#voice-indicator`** — colored dot in status bar, polls `/audio/status` every 5s
  - Green dot = listening, Red pulsing = recording, Blue = speaking, hidden = disabled
- **Error handler fix**: `set_error_handler` now preserves custom response bodies (was overwriting all non-200)
- **voice.speak() API**: added `int* out_sample_rate` parameter (backward compatible)
- **VDI deployment lesson**: dynamic VDIs need qemu-nbd for partition writes (not raw dd with data offset)
- Commits: `70561ad`

### Key Commits This Session
- `123e66c` — Phase 3a-3 Flite TTS
- `70561ad` — Phase 3a-4 Web UI voice integration

### VDI Recovery Needed
- Dynamic VDI was briefly corrupted by naive dd (wrong data offset assumption)
- Recovery: VBoxManage convertfromraw + resize, then qemu-nbd for future updates
- **Correct update pattern**: `qemu-nbd -c /dev/nbd0 <VDI>` → `dd to /dev/nbd0p3` → `qemu-nbd -d /dev/nbd0`

## Previous Session (Phase 3a-1 + 3a-2)

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

## Latest Session — MCP API Key (e8680ce)

### MCP API Key Implementation
- **Bearer token auth**: `Authorization: Bearer <64-hex-key>` accepted by `/mcp` alongside session cookies
- **Key persistence**: stored at `/data/llamaste/mcp_key.txt`, generated on first use, survives reboots
- **`McpAuthCheck`**: changed auth type from handler-wrapping to `function<bool(Request)>` predicate — cleaner, no circular deps
- **`load_or_create_api_key()`**: reads file, validates 64-hex, generates + persists if missing/invalid
- **`regenerate_api_key()`**: new 64-hex key, atomically replaces in memory + file, old key rejected immediately
- **Key management routes** in child_main.cpp (cookie-auth):
  - `GET /llamaste/mcp/key` → `{"key":"<full>","key_prefix":"<8>...","active":true}`
  - `POST /llamaste/mcp/key/regenerate` → same JSON with new key
- **System panel** MCP card: full key shown in blue monospace, Copy button (clipboard + textarea fallback for HTTP), Regen button (confirms → POST → updates display + snippet), config snippet auto-includes `Authorization` header with live key

### Verified (localhost:8080):
- `Bearer <key>` → 200 on ping, initialize, tools/list (44 tools), tools/call
- Cookie auth still works for web UI users
- Regenerate: old key → 401 immediately, new key → 200
- `GET /llamaste/mcp/key` → correct JSON, key persisted in `/data/llamaste/mcp_key.txt`

## Previous Session — Phase 3 MCP Server (0d418fc)

### MCP Server Implementation
- **Transport**: Streamable HTTP (MCP spec 2025-03-26), single endpoint `POST /mcp`
- **Protocol**: JSON-RPC 2.0 with session IDs (32-hex, 30-min idle expiry)
- **Methods**: initialize, ping, tools/list (44 tools), tools/call, resources/list, resources/read, prompts/list, notifications
- **Tool mapping**: `ToolRegistry.to_openai_tools_json()` → reformat `parameters`→`inputSchema` for MCP
- **Auth**: same `require_auth` cookie middleware as all other protected routes
- **System panel**: new MCP card with endpoint URL + pre-filled Claude Desktop config snippet, populated from live device IP
- **CORS**: full CORS headers for browser-based MCP clients
- **New files**: mcp_server.h, mcp_server.cpp (~400 LOC), 6 files modified

## Latest Session — mDNS DNS-SD + Proactive Notifications (916b671, 7c75e09)

### mDNS DNS-SD Advertisement (916b671)
- **`MdnsServiceRecord`** struct + `advertise_service()` method in net_mdns.h/cpp
- PTR/SRV/TXT/A record support — full DNS-SD (RFC 6763) response building
- Proactive announcement sent twice on startup (UDP loss tolerance)
- Query handling: PTR + ANY queries for `_mcp._tcp.local` service
- `child_main.cpp`: `mdns.advertise_service("_mcp._tcp", 80, {"path=/mcp","version=2025-03-26","auth=bearer"})`
- Serial log: `[mdns] Advertising llamaste._mcp._tcp.local on port 80 (txt: 3 entries)`

### Proactive Health Notifications (7c75e09)
- **`push_notification()`**: public method on Scheduler, safe to call from any thread
- **`set_model_check_fn()`**: callback `std::function<bool()>` → model-loaded check without coupling
- **Model-not-loaded alert**: fires 60s after startup, at most every 30 min if no model loaded
  - `type="alert"`, `title="No AI Model Loaded"`, `body="...Dashboard → Download a model..."`
- **Startup toast**: queued in `pending_notifications_` before `svr.listen()` blocks
  - `type="info"`, `title="Llamaste Ready"`, `body="Server running at http://<ip>/ — no model loaded yet"`
- Verified: startup toast drains on first SSE connect; model alert fires at 60s

### VDI Deploy Lesson (this session)
- VDI can't be overwritten from WSL2 while VM is running (file locked by VBoxHeadless)
- **Correct flow**: stop VM → update VDI → fix UUID → restart
- UUID fix: `VBoxManage internalcommands sethduuid <vdi> <uuid>` after replacing VDI file
- `scripts/deploy-to-vdi.sh`: helper for the VDI update workflow
- **NTFS rename from WSL2 fails**: use PowerShell `Copy-Item -Force` + `Remove-Item` instead of `mv`
- Skill saved: `~/.claude/skills/virtualbox-vdi-partition-update/SKILL.md`
- Git credential bridge: `/mnt/c/Program Files/Git/mingw64/bin/git-credential-manager.exe` (config as global credential.helper)

### End-of-Session Docs (6208570, 86655ec)
- **DEVELOPER.md**: updated net_mdns DNS-SD, MCP Bearer auth + DNS-SD discovery, scheduler push_notification() API, full version history
- **INSTALL.md**: What's New rewritten, MCP connection guide, notifications table, tools count 32→44, known limitations refreshed
- **CLAUDE.md**: Phase status → Phase 3a COMPLETE, VDI workflow, NTFS rename gotcha, UUID fix, known bugs cleared, next steps updated
- All 22 commits pushed to GitHub (`86655ec`)

## Next Steps

### Immediate
1. **Phase 3: Mesh clustering** — UDP multicast discovery, llama-rpc-server, coordinator election
2. **Phase 4: A/B updates** — SYS-B partition already reserved, GRUB `llamaste_slot` variable in design
3. Upgrade Flite TTS → higher quality voice (sherpa-onnx or Piper if ONNX builds)

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
