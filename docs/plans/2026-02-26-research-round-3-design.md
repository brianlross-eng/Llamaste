# Research Round 3: Gap Analysis & Coverage Plan

**Created**: 2026-02-26
**Status**: COMPLETE (all 8 topics researched)
**Approach**: Sequential (Approach C) — one topic at a time, highest impact first

---

## Background

After completing 15 research documents covering technical infrastructure (Buildroot, llama.cpp, boot process, CPU optimization, networking, API, MCP, minimal OS, syscalls, LLM-as-OS paradigm, speed optimization), a gap analysis revealed 8 topics that need research before Phase 1 implementation begins.

Two topics were found to already have sufficient coverage:
- First-run / onboarding UX (covered in docs 03, 06)
- Storage performance / model loading (covered in docs 04, 08, 09)

---

## Research Queue (Priority Order)

Each topic gets a full-depth research document (200-500 lines) saved to `D:\Llamaste\research\`.

| # | Priority | File | Topic | Status | Blocks |
|---|----------|------|-------|--------|--------|
| 1 | CRITICAL | `16-licensing-analysis.md` | Full stack license analysis | COMPLETE (637 lines) | Distribution |
| 2 | CRITICAL | `17-failure-modes-recovery.md` | Failure analysis, graceful degradation | COMPLETE (470 lines) | PID 1 reliability |
| 3 | HIGH | `18-multi-user-auth.md` | Multi-user, sessions, authentication | COMPLETE (450 lines) | API + Web UI design |
| 4 | HIGH | `19-privacy-data-security.md` | Encryption, retention, compliance | COMPLETE (480 lines) | Data partition design |
| 5 | MEDIUM | `20-update-mechanisms.md` | Offline updates, delta, signing | COMPLETE (470 lines) | Partition layout |
| 6 | MEDIUM | `21-power-management.md` | Laptops, suspend/resume, thermal | COMPLETE (1038 lines) | Laptop support |
| 7 | LOW | `22-competitor-ux-analysis.md` | Jan.ai, LM Studio, Ollama, LocalAI | COMPLETE (450 lines) | Web UI UX design |
| 8 | LOW | `23-lora-fine-tuning.md` | LoRA runtime loading, adapters | COMPLETE (500 lines) | Model customization |

---

## Research Scope Per Topic

### 1. Licensing Audit (16-licensing-audit.md)
**Key questions:**
- What licenses apply to each component in the stack? (kernel, musl, llama.cpp, ggml, httplib, Qwen models)
- Can we statically link GPL code (kernel headers) into a single binary?
- What are the distribution obligations? (source code, license notices, COPYING file)
- Does shipping GGUF model files in the image create license issues?
- What license should Llamaste's own code use?
- Are there any patent concerns with SIMD optimizations or quantization methods?

### 2. Failure Modes & Recovery (17-failure-modes-recovery.md)
**Key questions:**
- What are all the ways the llamaste binary can fail? (crash, OOM, model corruption, disk full, network down)
- How does a PID 1 process recover from crashes? (kernel panic vs respawn)
- What happens when the LLM hallucinates a destructive tool call?
- How to detect and handle corrupted model files?
- Graceful degradation: what works if the model can't load? (static web UI? reduced functionality?)
- Watchdog integration for auto-recovery
- State persistence: conversation recovery after crash
- Filesystem integrity (fsck on boot, journal recovery)

### 3. Multi-User & Authentication (18-multi-user-auth.md)
**Key questions:**
- How do existing local LLM servers handle multiple users? (Ollama, LM Studio, text-generation-webui)
- Session isolation: separate conversation histories per user/client?
- Authentication options: API keys, JWT tokens, HTTP basic auth, OAuth proxy?
- Authorization: can some users access destructive tools but not others?
- Concurrent access: multiple users chatting simultaneously — how does llama.cpp handle this?
- Web UI session management (cookies, localStorage, server-side sessions)
- Rate limiting per user

### 4. Privacy & Data Security (19-privacy-data-security.md)
**Key questions:**
- Should the data partition be encrypted? (LUKS, dm-crypt, performance impact on model loading)
- Conversation history: where stored, how long retained, how purged?
- Audit log: what gets logged, rotation policy, access control
- Tool call results may contain sensitive data (file contents, process lists, network info)
- GDPR/CCPA relevance for a local-only device?
- Network traffic: TLS for web UI? Self-signed certs? Let's Encrypt on LAN?
- What NOT to log (passwords, secrets found in files)

### 5. Update Mechanisms (20-update-mechanisms.md)
**Key questions:**
- RAUC vs SWUpdate vs Mender vs custom: which fits our partition layout?
- A/B partition switching: how does GRUB know which partition to boot?
- Offline updates: USB stick with update bundle, how to trigger?
- Delta updates: binary diff of squashfs images?
- Signature verification: what signing scheme? Ed25519?
- Model updates vs system updates (different cadence, different mechanism)
- Rollback trigger: boot counter? Health check? Watchdog?
- Update from web UI: upload .img via browser?

### 6. Power Management (21-power-management.md)
**Key questions:**
- ACPI support without systemd: what handles suspend/resume?
- Battery detection and reporting (sysfs, /sys/class/power_supply)
- Thermal throttling: how to reduce inference load when CPU is hot?
- Lid close behavior on laptops (suspend? keep serving?)
- CPU frequency scaling: performance governor vs powersave vs dynamic?
- Fan control awareness
- Wake-on-LAN for headless servers?
- UPS detection for graceful shutdown?

### 7. Competitor UX Analysis (22-competitor-ux-analysis.md)
**Key questions:**
- Jan.ai: install flow, model download UX, chat interface, settings, API
- LM Studio: same analysis
- Ollama: CLI UX, model management, API design, community
- LocalAI: API compatibility, Docker deployment, configuration
- What do users praise? What do they complain about?
- Feature comparison matrix
- What can Llamaste learn from each?
- What's our unique differentiator beyond "boots from USB"?

### 8. LoRA & Fine-Tuning (23-lora-fine-tuning.md)
**Key questions:**
- llama.cpp LoRA support: --lora flag, --lora-scaled, multiple adapters
- Performance impact of LoRA at inference time
- Hot-swapping adapters without reloading the base model
- Where to store adapters (data partition, same dir as models)
- Fine-tuning workflows: what tools exist? (Unsloth, Axolotl, PEFT)
- Can fine-tuning happen ON the Llamaste device or only externally?
- Use cases: domain-specific terminology, company knowledge, personality customization
- LoRA as a "skill pack" concept for the community repository (Phase 4)

---

## Completion Tracking

When each topic is completed, update its status in this table and in SESSION-STATUS.md.

**If session is interrupted**: Resume from the first NOT STARTED topic in the queue. All completed research docs are self-contained and saved to disk.

---

## After Research Completes

1. Update `LLAMASTE-IMPLEMENTATION-PLAN.md` with findings that affect architecture
2. Update `SESSION-STATUS.md` with new research count
3. Update `CLAUDE.md` and `MEMORY.md`
4. Begin Phase 1 implementation
