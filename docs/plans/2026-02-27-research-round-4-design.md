# Research Round 4: Speed, ISOs, and Distro Building

**Created**: 2026-02-27
**Status**: COMPLETE
**Approach**: Sequential execution (Topic 1 → 2 → 3)
**Sources**: Web research + YouTube video summaries

---

## Motivation

Research round 3 (23 docs) covered architecture, licensing, failure modes, auth, privacy,
updates, power, competitors, and LoRA. This round fills three gaps identified before
Phase 1 implementation begins:

1. Measured speed benchmarks and advanced optimization (beyond theory)
2. ISO image construction and installation UX
3. How real distros are built (practical Buildroot workflow, driver selection, CI)

---

## Topic 1: LLM Speed Benchmarks & Advanced Optimization

**File**: `research/24-llm-speed-benchmarks.md`
**Status**: COMPLETE (302 lines)
**Blocks**: Speed tuning in Phase 1 (thread config, KV cache settings, context size)

### Key Questions
- Real tokens/sec for Qwen2.5 7B/14B Q4_K_M on 8-16 GB x86?
- Quantized KV cache (`-ctk q8_0 -ctv q8_0`) — RAM savings, speed impact?
- `--ubatch-size` and prompt chunking — TTFT for ~2000 token system prompt?
- Vulkan iGPU offload — does it help or hurt on integrated graphics?
- Latest llama.cpp speed improvements (2025-2026) not in our existing docs?
- YouTube: llama.cpp optimization talks, benchmark walkthroughs

### Sources to Check
- llama.cpp GitHub releases/discussions (2025-2026 commits)
- HuggingFace model cards with benchmark tables
- r/LocalLLaMA benchmark megathreads
- YouTube: "llama.cpp speed", "llama cpp optimization 2025", "fastest llama inference"

---

## Topic 2: ISO Image Construction & Installation UX

**File**: `research/25-iso-image-construction.md`
**Status**: COMPLETE (845 lines)
**Blocks**: Task 11 (genimage), Task 10 (GRUB), Task 12 (E2E testing)

### Key Questions
- Does PID 1 (`init=/opt/llamaste`) need an initramfs?
- Live-from-USB vs install-to-disk — should Llamaste support both?
- What makes an ISO work with Ventoy/Rufus/Etcher?
- Model delivery: baked into image vs separate download vs USB sideload?
- Exact QEMU commands for BIOS and UEFI boot testing?
- grub-mkrescue vs xorriso for hybrid BIOS+UEFI ISOs?
- YouTube: "building custom linux iso", "grub mkrescue tutorial"

### Sources to Check
- Arch Linux archiso scripts
- Debian live-build documentation
- grub-mkrescue documentation
- Ventoy compatibility requirements
- YouTube: custom ISO build tutorials

---

## Topic 3: Custom Linux Distro Building Patterns

**File**: `research/26-custom-distro-build-patterns.md`
**Status**: COMPLETE (1162 lines)
**Blocks**: Task 1 (Buildroot setup), ongoing developer velocity

### Key Questions
- Buildroot cross-toolchain config for musl + static linking?
- Fast iteration: `make package/X-rebuild` workflow and gotchas?
- Kernel driver selection for "universal x86-64" — what do Alpine/Debian include?
- Reproducible builds with Docker for CI?
- Upstream update/security patch workflow?
- Alternative tools: mkosi, debos, live-build, lorax?
- YouTube: "building linux distro from scratch", "buildroot tutorial", "custom linux"

### Sources to Check
- Buildroot manual (chapter on developer workflow)
- Alpine Linux aports repository
- Debian kernel config fragments
- NixOS reproducibility approach
- YouTube: distro building conference talks

---

## Success Criteria

Each document:
- 400-800 lines
- Concrete answers (commands, numbers, configs — not just "it's possible")
- YouTube video summaries with timestamps and key takeaways
- Applicability notes for Llamaste Phase 1
- Updated recommendations that extend earlier research
