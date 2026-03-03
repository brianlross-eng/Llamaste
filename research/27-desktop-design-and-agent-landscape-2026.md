# Research 27: Desktop Design & AI Agent Landscape (March 2026)

## Desktop UI — What's Popular

### The Dominant Pattern: Chat-First
~90% of successful AI interfaces use ChatGPT-style layout (input box, scrolling conversation, SSE streaming). This is the proven paradigm. Llamaste already has this.

### What Users Love
- **Privacy/offline operation** (top priority for local LLM community)
- Fast first-token response (<3 seconds)
- Clean, minimal UI (ChatGPT simplicity)
- Streaming token output (word-by-word)
- RAG / "talk to your files"
- One-click model downloads / easy setup
- Dark theme (universal preference)
- Keyboard shortcuts / global hotkeys
- System status display (model, RAM, tokens/sec)
- Collapsible tool output in chat

### What Users Hate
- Electron bloat / memory overhead
- Rate limits / subscription paywalls
- Telemetry / data collection
- Slow cold starts
- Complex setup / CLI-only
- Context window limits / forgetting context
- Lack of offline capability

### Top Open-Source Local LLM UIs (ranked by community sentiment)
1. **Open WebUI** — self-hosted, RAG, MCP support, multi-tenant, SSE streaming
2. **LM Studio** — beginner favorite, Electron+React, one-click model downloads
3. **Jan.ai** — offline-first ChatGPT replacement, Cortex.cpp (C++) engine
4. **AnythingLLM** — MIT, best for RAG/document workflows
5. **LobeChat** — most polished UI, MCP, chain-of-thought visualization
6. **LibreChat** — best multi-provider support

### AI Hardware/OS Failures
- **Rabbit R1**: 100K pre-orders, 95% abandonment in 5 months. 1.5/5 rating.
- **Humane AI Pin**: $700+$24/mo, called "worst product ever reviewed." Bricked Feb 2025.
- **Lesson**: Standalone AI hardware fails. Software on existing screens wins.

### Kiosk Desktop Stack for Llamaste
```
Linux kernel
  -> llamaste (PID 1, HTTP server on :80)
  -> Cage (Wayland kiosk compositor)
    -> Chromium --kiosk --ozone-platform=wayland http://localhost
      -> Llamaste web UI (vanilla JS, SSE streaming, dark theme)
```
- Cage: purpose-built kiosk, single maximized app, no escape, minimal footprint
- Alternative: Cog (WebKit WPE) — smaller than Chromium, purpose-built for kiosk

---

## OpenClaw — The Biggest Open-Source AI Agent

### What It Is
Free, open-source, self-hosted personal AI agent. MIT license.
- **GitHub**: github.com/openclaw/openclaw — **~251,000 stars** (fastest-growing in history)
- Created by Peter Steinberger (PSPDFKit founder, now at OpenAI)
- Language: TypeScript/Node.js (~430,000 LOC)
- Originally "Clawdbot" (Nov 2025) → "Moltbot" (Jan 27, 2026, Anthropic trademark) → "OpenClaw" (Jan 30, 2026)

### Capabilities
- Autonomous agent via messaging platforms (WhatsApp, Telegram, Slack, Discord, Signal, iMessage, IRC, Teams, Matrix)
- Also: CLI, WebChat, companion apps (macOS menu bar, iOS, Android)
- Browser automation via CDP, file management, shell commands, calendar, email
- Proactive behavior (morning briefings, reminders, alerts)
- 5,400+ community skills on ClawHub
- Model-agnostic: Claude, GPT-4, DeepSeek, or local via Ollama

### Architecture
- Gateway WebSocket control plane (ws://127.0.0.1:18789)
- Execution in Docker containers
- ~17,000 token system prompt (needs 32K+ context, 65K+ recommended)
- Minimum: 2 cores, 2GB RAM, 2GB storage (cloud API mode)

### CRITICAL Security Issues
- **Multiple CVEs**: RCE (CVE-2026-25253), command injection (CVE-2026-24763), prompt injection (CVE-2026-22708), and more
- 17% of community skills flagged as malicious
- API keys stored in plaintext
- 42,665 exposed instances on Shodan, 93.4% with auth bypasses
- Andrej Karpathy: "a 400K lines of vibe-coded monster"
- Meta AI security researcher: agent "ran amok" on inbox, deleting everything

### Integration with Llamaste — NOT Recommended
| Aspect | Llamaste | OpenClaw |
|--------|----------|----------|
| Scope | LLM IS the OS (PID 1) | LLM agent ON TOP of an OS |
| Language | C++ (static binary) | Node.js (~430K LOC) |
| Runtime | Embedded llama.cpp | External LLM (API or Ollama) |
| Dependencies | None (static binary) | Node.js, npm, Docker |
| Security model | Compiled tools, sandboxed | Community skills, plaintext creds |

Node.js runtime + Docker dependency = fundamentally incompatible with Llamaste's no-shell single-binary architecture.

---

## Better Alternatives to OpenClaw

### For General Use
1. **ZeroClaw** (~18,900 stars) — Rust, <5MB RAM, <10ms cold start, 8.8MB single binary, Apache 2.0. The architectural anti-OpenClaw. Harvard/MIT students.
2. **Nanobot** (~21-27K stars) — Python, ~4,000 LOC (vs OpenClaw's 430K). Telegram, web search, MCP support.
3. **Open Interpreter** (~50K stars) — Python, natural language → code execution, local model profiles, AGPL-3.0
4. **OpenHands** (~67,900 stars) — Python, AI software development, $18.8M funded, MIT license

### For Llamaste Specifically
**None of these are suitable for direct integration.** They all require Python or Node.js runtimes. Llamaste's single-binary C++ architecture is unique and correct for its use case.

### What Llamaste Should Borrow (Design Patterns Only)
1. **From OpenClaw**: Skill/plugin architecture concept
2. **From ZeroClaw**: Trait-driven swap-anything-via-config pattern
3. **From AIOS**: OS-level LLM abstractions (scheduling, context switching)
4. **From Open Interpreter**: Model profiles (pre-configured settings per model)
5. **From CrewAI**: Hierarchical agent pattern (manager + worker models)
6. **From OpenClaw's failures**: Security anti-patterns (do the opposite)

### Best Models for Tool Calling (2026)
| RAM Tier | Model | Notes |
|----------|-------|-------|
| 2-4 GB | Qwen3-8B-Q4_K_M | Best tool-calling at small size, Apache 2.0 |
| 4-8 GB | Qwen3-30B-A3B-Q4_K_M (MoE) | 3B active params, 32B quality |
| 8-16 GB | Qwen3-Coder-30B or Qwen3-32B | Frontier tool calling |
| 16+ GB | DeepSeek-V3.2 (37B active) or Llama 3.3 70B | Maximum capability |

Qwen3 dominates 2026 tool-calling benchmarks, Apache 2.0 — natural upgrade from Llamaste's current Qwen2.5.

---

## AIOS — The Closest Conceptual Peer
- **GitHub**: github.com/agiresearch/AIOS — 5,100 stars
- Academic project (COLM 2025 paper) that embeds LLM into OS kernel abstractions
- Scheduling, context switching, memory management for LLM resources
- Natural-language terminal/shell
- Supports Ollama (llama.cpp backend)
- 2.1x faster agent execution via OS-level scheduling
- **BUT**: Python-based, academic prototype, not a bootable system
- **Value**: Study their OS-kernel abstraction ideas for Llamaste

---

## Summary: Agent Landscape State

| Category | Leader | Stars | Status |
|----------|--------|-------|--------|
| Messaging Agent | OpenClaw | 251K | Active but security disaster |
| Lightweight Agent | ZeroClaw | 19K | Active, architecturally sound |
| Dev Agent | OpenHands | 68K | Active, well-funded |
| Multi-Agent | CrewAI | 44K | Active, enterprise |
| Code Execution | Open Interpreter | 50K | Active |
| AI OS (academic) | AIOS | 5K | Active |
| Infrastructure | Ollama | 162K | Very active |
| Dead | BabyAGI, AgentGPT | — | Archived/stale |

The 2026 landscape is massive but architecturally homogeneous (Python/Node.js, cloud-first). Llamaste occupies a genuinely novel niche.
