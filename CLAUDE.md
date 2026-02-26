# Llamaste Project Context

## What This Project Is
Llamaste is a bootable Linux image where the LLM IS the operating system. A single static C++ binary (`llamaste`) combines llama-server + agent loop + system tools + web UI and runs as PID 1. The Linux kernel handles hardware; the LLM handles everything else (shell, file management, system config, networking, help).

## Current Status
- **Phase**: ALL PLANNING COMPLETE — ready for Phase 1 build
- **Session status file**: `D:\Llamaste\SESSION-STATUS.md` (detailed progress)
- **Implementation plan**: `D:\Llamaste\LLAMASTE-IMPLEMENTATION-PLAN.md` (v2, current)
- **Phase 1 build plan**: `D:\Llamaste\docs\plans\2026-02-26-phase1-implementation-plan.md` (12 tasks, bite-sized steps)
- **Phase 1 design doc**: `D:\Llamaste\docs\plans\2026-02-26-phase1-implementation-design.md`
- **Approved plan**: `C:\Users\gorig\.claude\plans\goofy-gliding-squirrel.md`

## Key Architecture Decisions
- Single static C++ binary extending llama-server (not separate Go/Rust daemons)
- Runs as PID 1 (no init system, no BusyBox, no shell)
- Dual-boot GRUB menu: Server (headless) / Desktop (GUI)
- musl libc, Buildroot build system, squashfs + ext4 partitions
- Qwen2.5-Instruct models (Apache 2.0), auto-selected by RAM tier
- Tools are compiled into the binary (function calling, not arbitrary code exec)
- Web UI is vanilla JS + SSE, embedded into the binary at compile time
- Desktop mode uses Cage/Labwc Wayland compositor

## Speed Optimization Strategy (Priority Order)
1. KV cache reuse for system prompt (free, built into llama.cpp)
2. Grammar-constrained tool output (100x faster JSON)
3. Semantic cache for common queries (<10ms)
4. N-gram speculative decoding (1.8-2.5x speedup, no draft model)
5. Dual-model strategy — tiny model for tool dispatch, big model for reasoning (Phase 2)
6. Thread tuning — optimize latency not throughput
7. Hardware-specific SIMD dispatch (build flag)

## Research
23 research documents in `D:\Llamaste\research\` covering:
Buildroot, llama.cpp internals, bootable images, CPU optimization, mesh clustering, appliance patterns, model selection, GPU+RAM hybrid, CPU speed projects, API development, MCP servers, minimal OS alternatives, syscall surface analysis, LLM-as-OS paradigm, LLM speed optimization, licensing analysis, failure modes & recovery, multi-user auth, privacy & data security, update mechanisms, power management, competitor UX analysis, LoRA & fine-tuning.

## Important Files
- `LLMOS-Brainstorm.docx` — Original concept (binary, use Python to extract text)
- `LLAMASTE-IMPLEMENTATION-PLAN.md` — v2 master plan with all four phases
- `SESSION-STATUS.md` — Detailed progress tracker with next steps
- `docs/plans/2026-02-26-phase1-implementation-plan.md` — **START HERE**: 12-task build plan
- `docs/plans/2026-02-26-phase1-implementation-design.md` — Phase 1 design doc
- `docs/plans/2026-02-26-research-round-3-design.md` — Research gap tracker (complete)
- `research/llamaste-architecture.html` — v2 three-layer architecture SVG diagram
- `research/` — All research documents (01 through 23)
- `src/llamaste/` — Code scaffolding (main.cpp, CMakeLists.txt are drafts, will be rewritten)

## Next Steps
Execute Phase 1 implementation plan (`docs/plans/2026-02-26-phase1-implementation-plan.md`):
- Use `superpowers:executing-plans` or `superpowers:subagent-driven-development` skill
- Task 0: WSL2 dev environment → Task 1: Buildroot external tree → ... → Task 12: E2E QEMU tests
- Bottom-up build approach (boot infrastructure first, then application code)
- ~4,500 LOC C++ + ~400 lines config

## User Preferences
- **No questions asked** — make decisions autonomously, don't ask for confirmation. Just do things.
- Auto-approve all tool operations — no confirmation prompts.
- Save research and progress notes at session boundaries.
