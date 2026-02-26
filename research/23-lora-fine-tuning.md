# LoRA Adapters & Fine-Tuning for Llamaste

## Executive Summary

LoRA (Low-Rank Adaptation) adapters are small files (10-500 MB) that modify a base model's behavior without changing its weights. For Llamaste, this enables "skill packs" -- downloadable specializations for medical, legal, coding, or company-specific domains. llama.cpp has mature LoRA support with per-request adapter selection, runtime hot-swapping, and multi-adapter serving. On-device fine-tuning is technically possible but impractical for most hardware; the recommended path is external training with adapter loading on Llamaste.

**Key finding**: llama.cpp already supports everything needed for LoRA adapter loading and management. The implementation cost is modest -- mostly file management, web UI integration, and a conversion pipeline. This should be Phase 2 (adapter loading) and Phase 3 (skill pack marketplace).

---

## 1. How LoRA Works

### The Core Idea

Instead of updating all parameters in a large weight matrix W (billions of parameters), LoRA freezes W and learns two small matrices A and B such that the adapted weight is W + BA. The matrices A (rank x hidden) and B (hidden x rank) are dramatically smaller than W.

- **Rank (r)**: Controls adapter capacity. Low rank = fewer parameters, smaller file. Typical values: 4, 8, 16, 32, 64.
- **Alpha**: Scaling factor, usually set to 2x rank. Controls how strongly the adapter modifies the base model.
- **Target modules**: Which layers get adapted. Usually attention projections (q_proj, k_proj, v_proj, o_proj), sometimes all linear layers.

### Parameter Reduction

LoRA reduces trainable parameters by up to 10,000x. For a 7B model:
- Full fine-tuning: ~14 GB of parameter updates
- LoRA rank 16 (attention only): ~10-50 MB of parameter updates
- LoRA rank 64 (all linear layers): ~100-500 MB

### Inference Mechanics

At inference time, the LoRA matrices are applied as an additive modification to the base model's attention weights. For each forward pass through an adapted layer:

```
output = x @ W + scale * (x @ A @ B)
```

The `scale` factor (alpha/rank) controls adaptation strength. This is a simple matrix multiplication addition -- no architectural changes needed.

---

## 2. llama.cpp LoRA Implementation

### Command-Line Support

llama.cpp has full LoRA support via command-line flags:

```bash
# Single adapter
llama-server -m base-model.gguf --lora adapter.gguf

# Multiple adapters
llama-server -m base-model.gguf --lora adapter1.gguf --lora adapter2.gguf

# Scaled adapter (control strength)
llama-server -m base-model.gguf --lora-scaled adapter.gguf 0.5

# Load adapters but start with scale 0 (disabled until requested)
llama-server -m base-model.gguf --lora adapter.gguf --lora-init-without-apply
```

### GGUF LoRA Format

llama.cpp uses its own GGUF format for LoRA adapters. Conversion from HuggingFace PEFT format:

```bash
python convert_lora_to_gguf.py \
  --base /path/to/base-model/ \
  --outtype q8_0 \
  /path/to/peft-adapter/
```

The script expects a directory containing:
- `adapter_config.json` (PEFT config with rank, alpha, target modules)
- `adapter_model.safetensors` (the actual weights)

Output: a single `.gguf` file containing the adapter weights, ready for `--lora`.

There is also a web-based converter on HuggingFace: "GGUF-my-LoRA" (HF Space) for users who do not want to run the script locally.

### Adapter Quantization

GGUF adapters can be quantized (e.g., `--outtype q8_0`) to reduce size further. Quantization levels:
- **f16**: Full precision, largest files, best quality
- **q8_0**: 8-bit quantized, ~50% size reduction, negligible quality loss
- **q4_0**: 4-bit quantized, ~75% size reduction, some quality loss

**Recommendation for Llamaste**: Default to q8_0 for a good balance of size and quality.

### Merging vs. Runtime Loading

Two approaches exist:
1. **Merge into base model** (`llama-export-lora`): Permanently bakes the adapter into a new GGUF file. Larger file, no runtime overhead, but inflexible.
2. **Runtime loading** (`--lora`): Keeps adapter separate. Small overhead at inference, but can swap/combine adapters dynamically.

**Recommendation for Llamaste**: Always use runtime loading. Merging defeats the purpose of modular skill packs.

---

## 3. llama-server LoRA API

This is the most important section for Llamaste. llama-server already exposes a full LoRA management API.

### GET /lora-adapters

Returns currently loaded adapters with their IDs, paths, and scale values:

```json
[
  {"id": 0, "path": "/data/adapters/medical-v1.gguf", "scale": 1.0},
  {"id": 1, "path": "/data/adapters/coding-v1.gguf", "scale": 0.0}
]
```

Adapters with scale 0.0 are loaded but inactive.

### POST /lora-adapters

Set global adapter scales (overridden by per-request settings):

```json
[
  {"id": 0, "scale": 1.0},
  {"id": 1, "scale": 0.5}
]
```

### Per-Request Adapter Selection

The key feature: each inference request can specify which adapters to use:

```json
{
  "prompt": "Diagnose the following symptoms...",
  "lora": [
    {"id": 0, "scale": 1.0}
  ]
}
```

Adapters not listed default to scale 0.0 for that request. This means different users or sessions can use different adapters on the same server.

### Performance Note

Requests with different LoRA configurations will NOT be batched together. This is a latency consideration for multi-user scenarios -- but for Llamaste's typical 1-3 concurrent users on home hardware, this is not a significant issue.

### Startup Initialization Strategy

Using `--lora-init-without-apply` loads all adapters into memory at startup but sets their scales to 0. The LLM or web UI can then activate specific adapters per-request. This is ideal for Llamaste:

1. Boot: load base model + all adapters from `/data/adapters/`
2. Default: no adapters active (base model behavior)
3. User selects "Medical Knowledge" skill pack via chat or web UI
4. Subsequent requests include `"lora": [{"id": 0, "scale": 1.0}]`

---

## 4. LoRA Rank Trade-offs

### Rank Comparison Table

| Rank | Parameters (7B model, attn) | Adapter Size (q8_0) | Quality | Use Case |
|------|----------------------------|---------------------|---------|----------|
| 4    | ~2M                        | ~2-5 MB             | Style/formatting only | Personality, tone |
| 8    | ~4M                        | ~5-10 MB            | Good for simple tasks | Language, basic domain |
| 16   | ~8M                        | ~10-25 MB           | Solid general purpose | Most domain adaptation |
| 32   | ~16M                       | ~25-50 MB           | High quality | Complex domains |
| 64   | ~32M                       | ~50-150 MB          | Near full fine-tune | Deep specialization |
| 128  | ~64M                       | ~150-400 MB         | Diminishing returns | Large dataset only |

### Recommendations by Use Case

- **Personality/style adaptation** (brand voice, formality level): rank 4-8
- **Language improvement** (better non-English): rank 8-16
- **Domain vocabulary** (medical/legal terms): rank 16-32
- **Deep domain knowledge** (company procedures, technical manuals): rank 32-64
- **Tool-use improvement** (better function calling): rank 8-16

### Alpha Setting

The standard practice is alpha = 2 * rank. This has been empirically shown to provide the best generalization. For rank 16, use alpha 32. The scaling factor applied during inference is alpha/rank.

---

## 5. Performance Impact of LoRA at Inference

### Memory Overhead

Each loaded adapter consumes additional RAM proportional to its size:
- Rank 16 adapter for 7B model: ~10-25 MB in memory
- Rank 64 adapter for 7B model: ~50-150 MB in memory
- 10 rank-16 adapters loaded simultaneously: ~100-250 MB total

For a Llamaste system with 16 GB RAM running a 7B Q4 model (~4 GB), loading 10 adapters adds only ~200 MB -- negligible.

### Latency Overhead

LoRA adds extra matrix multiplications per adapted layer per token. The overhead is:
- **Prompt processing**: Minimal impact (already compute-bound)
- **Token generation**: Measurable but modest, roughly 5-15% slowdown depending on rank and number of adapted layers
- **Memory bandwidth**: The additional A and B matrices must be read for each token, adding to bandwidth pressure on CPU-only systems

Since llama.cpp inference is memory-bandwidth-bound on CPU, the impact scales with adapter size relative to model size. A rank-16 adapter on a 7B model adds < 0.5% to total memory reads -- essentially negligible.

### Switching Latency

Switching adapters between requests does NOT require reloading the base model. With `--lora-init-without-apply`, all adapters are pre-loaded. Switching is just changing the scale factors -- effectively zero-cost.

---

## 6. LoRA Adapter Formats and Sources

### The Conversion Pipeline

```
HuggingFace PEFT adapter (safetensors)
  --> convert_lora_to_gguf.py (requires base model for reference)
    --> GGUF adapter file (.gguf)
      --> llama-server --lora adapter.gguf
```

### Where to Find Pre-Trained Adapters

1. **HuggingFace Hub**: Thousands of LoRA adapters. Search by base model (e.g., "Qwen2.5 LoRA") and task. Most are in PEFT safetensors format, requiring conversion.
2. **Community collections**: Curated collections like DevQuasar's "LoRA Adapters" on HuggingFace.
3. **Research papers**: Academic LoRA adapters often published with papers.
4. **Ollama library**: Some models include LoRA adapters already in GGUF format.

### Compatibility Requirements

An adapter MUST match the base model architecture:
- Same model family (e.g., Qwen2.5, not Qwen2.5 adapter on Llama)
- Same model size (e.g., 7B adapter on 7B model)
- Same vocabulary (tokenizer must match)
- Target modules must exist in the base model

Mismatched adapters will fail to load or produce garbage output.

### Metadata for Llamaste Skill Packs

Each adapter should include a metadata file:

```json
{
  "name": "Medical Terminology v1",
  "description": "Improves medical term recognition and clinical reasoning",
  "version": "1.0.0",
  "author": "CommunityUser",
  "license": "Apache-2.0",
  "base_model": "Qwen2.5-7B-Instruct",
  "base_model_family": "qwen2",
  "rank": 16,
  "alpha": 32,
  "target_modules": ["q_proj", "k_proj", "v_proj", "o_proj"],
  "training_data_description": "PubMed abstracts, clinical notes, medical QA",
  "training_examples": 50000,
  "sha256": "abc123...",
  "created_at": "2026-01-15",
  "tags": ["medical", "clinical", "health"]
}
```

---

## 7. On-Device Fine-Tuning

### llama.cpp Built-in Finetune

llama.cpp includes a `finetune` binary that supports LoRA training on CPU with quantized GGUF models. This is the only option that works natively within the Llamaste ecosystem (no Python required at runtime).

**Capabilities**:
- Works with quantized GGUF models directly
- CPU-only (no GPU required)
- Produces LoRA adapter files
- Configurable rank, learning rate, context length, iterations

**Limitations**:
- Very slow on CPU (hours to days for small models, days to weeks for 7B+)
- Limited feature set compared to Python frameworks
- No QLoRA optimization (uses its own quantized training approach)
- The finetune binary has been described as suitable mainly for very small models

### CPU Training Benchmarks

Based on community benchmarks (i7-12700H, 64 GB RAM):

| Model Size | Context | RAM Needed | Training Time (1000 examples) |
|------------|---------|------------|-------------------------------|
| 1B Q4      | 512     | ~8 GB      | 2-6 hours                     |
| 3B Q4      | 512     | ~12 GB     | 8-24 hours                    |
| 7B Q4      | 512     | ~20 GB     | 2-5 days                      |
| 7B Q4      | 2048    | ~32 GB     | 5-14 days                     |
| 13B Q4     | 2048    | ~48 GB     | 2-4 weeks                     |

These are rough estimates. Actual times depend on CPU speed, RAM bandwidth, rank, and number of iterations.

### RAM Requirements for Training

Training requires significantly more RAM than inference because gradients and optimizer states must be stored:

| Available RAM | Max Trainable Model | Context Limit |
|---------------|--------------------|----|
| 16 GB         | 3B (possibly 7B with swap) | 512-1024 |
| 32 GB         | 7B comfortably     | 2048 |
| 64 GB         | 13B comfortably    | 2048 |

### QLoRA on CPU (Intel Extension)

Intel has developed CPU-optimized QLoRA via their Extension for Transformers. This requires Python and the Intel extension, making it unsuitable for Llamaste's static binary approach. Mentioned for completeness only.

### Practical Assessment for Llamaste

On-device fine-tuning via llama.cpp's finetune tool is **technically possible but impractical** for most Llamaste users:

- A 3B model with 1000 training examples takes 8-24 hours on a decent CPU
- A 7B model (the recommended Llamaste default) takes days
- The quality of CPU-trained adapters is comparable to GPU-trained ones
- Users would need to prepare training data in the correct format
- The process is not interactive -- no progress feedback, no early stopping via UI

**Recommendation**: Include on-device fine-tuning as an advanced/experimental feature in Phase 4. Focus on loading pre-trained adapters for Phase 2-3.

---

## 8. External Fine-Tuning Workflow

The recommended path: users fine-tune on external hardware (cloud GPU, local GPU machine), export the adapter, and load it on Llamaste.

### Recommended Tools

| Tool | Speed | Ease of Use | GPU Required | Output Format |
|------|-------|-------------|-------------|---------------|
| **Unsloth** | 2-10x faster | High (notebooks) | Yes (NVIDIA) | PEFT safetensors |
| **Axolotl** | Standard | Medium (YAML config) | Yes | PEFT safetensors |
| **PEFT/HuggingFace** | Standard | High (Python API) | Yes | PEFT safetensors |
| **torchtune** | Standard | Medium | Yes | PyTorch |
| **LLaMA-Factory** | Standard | High (web UI) | Optional (slow on CPU) | PEFT safetensors |

### Unsloth (Recommended for Speed)

Unsloth is the fastest option, offering 2-10x speed improvements and 70-80% VRAM reduction via custom Triton kernels. Supports QLoRA (4-bit training) which fits a 7B model in ~6 GB VRAM.

Training a Qwen2.5-7B adapter with 1000 examples on an RTX 3090 (24 GB): ~30-60 minutes with Unsloth.

Unsloth is GPU-only for training. CPU support exists only for inference offloading.

### Training Data Formats

Common formats accepted by most tools:

**Alpaca format** (simplest):
```json
[
  {
    "instruction": "Explain hypertension",
    "input": "",
    "output": "Hypertension is a condition where blood pressure..."
  }
]
```

**ShareGPT format** (conversational):
```json
[
  {
    "conversations": [
      {"from": "human", "value": "What is hypertension?"},
      {"from": "gpt", "value": "Hypertension is..."}
    ]
  }
]
```

**JSONL format** (one example per line):
```jsonl
{"messages": [{"role": "user", "content": "..."}, {"role": "assistant", "content": "..."}]}
```

### End-to-End Workflow for Non-Technical Users

1. **Prepare data**: Create a JSONL file with question-answer pairs (Llamaste could provide a tool to help format documents into training data)
2. **Upload to cloud**: Google Colab (free GPU), RunPod, Lambda Labs, or local GPU
3. **Run training notebook**: Pre-configured Unsloth notebook (Llamaste could provide this)
4. **Download adapter**: The PEFT adapter directory (~10-200 MB)
5. **Convert**: Run `convert_lora_to_gguf.py` (or use GGUF-my-LoRA HF Space)
6. **Install on Llamaste**: Copy GGUF file to `/data/adapters/` and create metadata JSON
7. **Activate**: Select via web UI or chat command

### Simplification Opportunities

Llamaste could ship:
- A pre-configured Google Colab notebook for training
- A `model.convert_adapter()` tool that runs the conversion (requires Python at build time, or a lightweight converter compiled into the binary)
- A web UI page for uploading adapters and auto-generating metadata
- Documentation with step-by-step guides for each training platform

---

## 9. Multi-LoRA Serving

### llama.cpp Native Multi-LoRA

llama-server already supports loading multiple adapters at startup and selecting per-request. This is the simplest approach and sufficient for Llamaste's scale (1-5 concurrent users).

**Memory model**: All loaded adapters reside in RAM simultaneously. With typical adapter sizes of 10-50 MB each, loading 10-20 adapters adds only 100-500 MB.

**Batching limitation**: Requests using different adapter configurations cannot be batched together. For single-user scenarios this is irrelevant. For multi-user, it means each user's request is processed independently.

### S-LoRA (Research Context)

S-LoRA is a research system for serving thousands of concurrent LoRA adapters. Key innovations:
- **Unified paging**: Shared memory pool for KV cache and adapter weights
- **Custom CUDA kernels**: Efficient batched inference across different adapters
- **Adapter prefetching**: Predict next-needed adapters and pre-load them

S-LoRA can serve 2,000 adapters simultaneously with minimal overhead. However, it requires GPU and is designed for cloud-scale serving. Not directly applicable to Llamaste, but the concepts (prefetching, unified memory management) could inform future optimization.

### Practical Multi-LoRA for Llamaste

Given Llamaste's constraints (CPU-only, 1-5 users, home/small business):
- Pre-load all installed adapters at boot (with `--lora-init-without-apply`)
- Let users activate adapters via web UI or chat
- Adapter switching is effectively instant (just change scale factors)
- No need for S-LoRA-level optimization

**Memory budget**: With 16 GB RAM and a 7B Q4 model (~4 GB), there is ~8 GB for KV cache and other data. Loading 20 rank-16 adapters uses ~200-500 MB -- well within budget.

---

## 10. Combining Multiple Adapters

### Multi-Adapter Composition

llama.cpp supports loading multiple adapters simultaneously with individual scaling factors. This enables composing capabilities:

```json
{
  "lora": [
    {"id": 0, "scale": 1.0},
    {"id": 1, "scale": 0.7}
  ]
}
```

Example: "Medical Knowledge" (scale 1.0) + "Formal Tone" (scale 0.7) = a formally-worded medical assistant.

### Limitations of Composition

- Adapters trained independently may interfere with each other
- No guarantee that combining two good adapters produces good results
- Higher total rank (sum of all active adapter ranks) means more overhead
- Empirical testing is needed for each combination

### X-LoRA (Advanced)

X-LoRA uses a mixture-of-experts approach to dynamically blend multiple adapters at the token level. The gating mechanism learns which adapter is most useful for each token. This is more sophisticated than simple scaling but requires a trained gating model. Potential Phase 4+ feature.

---

## 11. Security Considerations

### Threat Model

LoRA adapters are a significant attack surface. A malicious adapter can:

1. **Inject backdoors**: Respond normally except when a trigger phrase is present, then output attacker-controlled content
2. **Bias outputs**: Subtly shift the model toward misinformation or harmful content
3. **Exfiltrate data**: Encode user input into output patterns readable by an attacker
4. **Bypass safety**: Override the base model's alignment and safety training

### Why Detection Is Hard

- Adapters are small (10-200 MB) and opaque -- you cannot easily inspect what they do
- Backdoors activate only on specific triggers, passing normal evaluation
- A "helpful" adapter that improves medical knowledge could also contain a hidden backdoor
- The LoRA ecosystem lacks formal vetting or certification

### Attack Scenarios for Llamaste

Since Llamaste runs as PID 1 with full system access:
- A backdoored adapter could cause the LLM to execute destructive system commands when triggered
- An adapter could make the LLM leak file contents or network information
- An adapter could disable security features or create backdoor accounts

### Defenses

1. **Source verification**: Only allow adapters from trusted sources. Implement a signing system where official/verified adapters have cryptographic signatures.

2. **SHA256 integrity**: Hash every adapter file and verify on load. Store hashes in the metadata file and verify before loading.

3. **Weight-space analysis**: Recent research (February 2026) shows that backdoored adapters have distinctive weight patterns -- concentrated singular values, low entropy. A detector achieved 97% accuracy with under 2% false positives on 500 adapters without running the model. Llamaste could implement a lightweight version of this check.

4. **Sandboxed evaluation**: Before installing, run test prompts through the adapter and check for anomalous behavior. Not foolproof against trigger-based backdoors, but catches obvious problems.

5. **User warnings**: Always warn users when loading third-party adapters. Display adapter source, author, and any verification status.

6. **Permission system**: Adapters loaded from untrusted sources could be run in a "restricted" mode where certain dangerous tools (fs.delete, process.kill, network.raw) are disabled.

### Recommendation

For Phase 2-3, implement:
- SHA256 integrity checking on all adapter files
- Metadata validation (adapter claims to be for the correct base model)
- User confirmation prompt when loading new adapters
- Source tracking (where was this adapter downloaded from?)

For Phase 4+, consider:
- Weight-space anomaly detection
- Community reputation system for skill pack authors
- Official "verified" adapter program

---

## 12. Skill Pack Concept Design

### What is a Skill Pack?

A skill pack is a user-friendly package containing:
1. A GGUF LoRA adapter file
2. A metadata JSON file (name, description, base model, author, license)
3. Optional: example prompts, usage instructions, icon

### File Structure on Disk

```
/data/adapters/
  medical-terminology-v1/
    adapter.gguf              (10-200 MB, the actual weights)
    metadata.json             (adapter info, compatibility, SHA256)
    README.txt                (optional usage instructions)
  legal-documents-v1/
    adapter.gguf
    metadata.json
  company-acme-knowledge/
    adapter.gguf
    metadata.json
```

### Installation Flow

1. **Download**: User downloads `.skillpack` file (a tar.gz containing adapter.gguf + metadata.json)
2. **Verify**: Llamaste checks SHA256, validates metadata, confirms base model compatibility
3. **Install**: Extract to `/data/adapters/<pack-name>/`
4. **Load**: On next boot (or via hot-reload API), adapter is loaded with scale 0
5. **Activate**: User enables via web UI or chat: "Enable medical terminology"

### Web UI Integration

The web UI should include:
- **Skill Packs page**: List all installed adapters with name, description, status (active/inactive)
- **Toggle switch**: Enable/disable each adapter
- **Strength slider**: Adjust scale factor (0.0 to 2.0)
- **Install button**: Upload a .skillpack file
- **Compatibility check**: Show which adapters are compatible with the currently loaded base model

### Chat Integration

LLM tools for adapter management:

```
model.list_adapters()     -> List installed adapters with status
model.enable_adapter(name, scale=1.0)  -> Activate an adapter
model.disable_adapter(name)            -> Deactivate an adapter
model.adapter_info(name)               -> Show adapter metadata
```

### Storage Budget

On a typical Llamaste data partition (remaining space after base model):

| Adapter Rank | Approx Size (q8_0) | Adapters in 1 GB | Adapters in 5 GB |
|-------------|--------------------|----|---|
| 8           | ~5-10 MB           | 100-200 | 500-1000 |
| 16          | ~10-25 MB          | 40-100 | 200-500 |
| 32          | ~25-50 MB          | 20-40 | 100-200 |
| 64          | ~50-150 MB         | 7-20 | 33-100 |

Even with generous adapter sizes, 1-5 GB of storage supports dozens to hundreds of adapters.

---

## 13. Use Cases for Llamaste

### Domain Adaptation

| Domain | Training Data | Expected Rank | Benefit |
|--------|--------------|---------------|---------|
| Medical | PubMed, clinical notes, drug databases | 16-32 | Accurate medical terminology, clinical reasoning |
| Legal | Case law, contracts, regulations | 16-32 | Legal terminology, citation formats, clause analysis |
| Finance | Financial reports, regulations, market data | 16-32 | Financial terms, risk assessment, compliance |
| Coding | Code repos, documentation, Stack Overflow | 16-32 | Better code generation, debugging, API knowledge |
| Customer service | FAQ, support tickets, product manuals | 8-16 | Company-specific answers, product knowledge |

### Company Knowledge Base

A small business can fine-tune on:
- Internal procedures and policies
- Product documentation
- Customer FAQ
- Employee handbook
- Technical manuals

This makes the Llamaste system a company-specific knowledge assistant.

### Personality and Style

- **Brand voice**: Match the tone of company communications
- **Formality levels**: Casual, professional, technical
- **Character personas**: Custom AI personality for different interfaces
- **Language focus**: Improve performance in specific non-English languages

### Tool-Use Improvement

Fine-tune on examples of correct tool calls for Llamaste's built-in tools:
- Better accuracy in choosing the right tool
- More precise parameter generation
- Fewer hallucinated tool calls
- Better error recovery

---

## 14. Phased Implementation Plan

### Phase 1: No LoRA (Current)
- Focus on getting the base system working
- No adapter support needed
- Design the data partition layout to reserve space for future adapters

### Phase 2: Static Adapter Loading
**Scope**: Load pre-trained adapters at startup

**Implementation**:
1. Add `--lora` and `--lora-init-without-apply` to the llamaste server startup
2. Scan `/data/adapters/*/metadata.json` at boot
3. Validate each adapter (base model match, SHA256)
4. Load all valid adapters with scale 0
5. Read user preferences for default adapter configuration
6. Add LLM tools: `model.list_adapters()`, `model.enable_adapter()`, `model.disable_adapter()`
7. Add web UI: adapter list with toggles

**Estimated effort**: 2-3 days of implementation
- Mostly file scanning, JSON parsing, and wiring up existing llama.cpp flags
- Web UI: one new page with adapter cards

**What goes in the binary**:
- Adapter directory scanner (C++)
- Metadata JSON parser (C++)
- Adapter validation (SHA256, base model check)
- LLM tool handlers for adapter management
- Web UI page (embedded HTML/JS)

**What is just file management**:
- `/data/adapters/` directory structure
- Metadata JSON files
- User copies GGUF files to the data partition

### Phase 3: Runtime Management and Skill Packs
**Scope**: Hot-swap adapters, skill pack packaging, community sharing

**Implementation**:
1. `.skillpack` file format (tar.gz of adapter.gguf + metadata.json)
2. Web UI: upload/install/remove skill packs
3. Per-session adapter selection (different users can use different adapters)
4. Adapter combination UI (select multiple adapters with individual scales)
5. Conversion tool integration: accept HuggingFace PEFT adapters and convert in-place
6. Community repository browser (curated list of compatible skill packs)

**Estimated effort**: 1-2 weeks
- Conversion requires bundling `convert_lora_to_gguf.py` or reimplementing in C++
- Community repository requires a simple catalog format (JSON index file hosted on a URL)

### Phase 4: On-Device Fine-Tuning (Experimental)
**Scope**: Basic fine-tuning on the device itself

**Implementation**:
1. Integrate llama.cpp's finetune functionality into the binary
2. Web UI: training data upload (JSONL), parameter configuration, training progress
3. LLM tool: `model.train_adapter(data_path, rank, epochs)`
4. Background training with progress reporting
5. Auto-convert and install the resulting adapter

**Estimated effort**: 2-4 weeks
- Significant C++ work to integrate the finetune code path
- UI for training progress, data preparation
- Must handle the system remaining responsive during training (background thread, lower priority)

**Realistic expectations**: Only practical for 1B-3B models on typical hardware. 7B+ requires powerful hardware and multiple days. Position this as "experimental" / "advanced users only."

### Phase 5: Advanced Features (Future)
- Weight-space security analysis for untrusted adapters
- Adapter marketplace with ratings and reviews
- X-LoRA dynamic adapter mixing
- Adapter versioning and rollback
- Training data synthesis (generate training examples from documents)
- One-click cloud fine-tuning integration (upload data, receive adapter)

---

## 15. Implementation Details for the Binary

### Adapter Scanner (Phase 2)

```cpp
// At boot, scan /data/adapters/ for installed skill packs
struct AdapterInfo {
    std::string name;
    std::string path;          // path to .gguf file
    std::string description;
    std::string base_model;
    int rank;
    float alpha;
    std::string sha256;
    bool compatible;           // matches current base model?
    int lora_id;               // assigned by llama.cpp at load time
    float current_scale;       // 0.0 = inactive
};

std::vector<AdapterInfo> scan_adapters(const std::string& adapter_dir);
```

### LLM Tools

```
Tool: model.list_adapters
Description: List all installed LoRA skill packs
Output: JSON array of adapter info (name, description, active, scale)

Tool: model.enable_adapter
Parameters: name (string), scale (float, default 1.0)
Description: Activate a skill pack for the current session

Tool: model.disable_adapter
Parameters: name (string)
Description: Deactivate a skill pack

Tool: model.adapter_info
Parameters: name (string)
Description: Show detailed information about a skill pack
```

### Web UI Components

- `/adapters` page: grid of adapter cards
- Each card: name, description, author, tags, active toggle, scale slider
- Upload button: install new .skillpack file
- Delete button: remove adapter (with confirmation)
- Compatibility badge: green check if adapter matches base model

---

## 16. Key Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Incompatible adapter loaded | Garbage output, potential crash | Validate base model match in metadata before loading |
| Malicious adapter | System compromise via LLM tool calls | SHA256 verification, source tracking, weight-space analysis (Phase 4) |
| Memory exhaustion from too many adapters | System instability | Cap max loaded adapters based on available RAM |
| Slow inference with many active adapters | Poor user experience | Warn when > 3 adapters active; recommend disabling unused ones |
| Adapter quality varies wildly | User frustration | Community ratings, verified/tested adapters, clear descriptions |
| User expects fine-tuning to work quickly | Disappointment with CPU training | Set clear expectations in UI; recommend external GPU training |

---

## 17. Storage and Directory Layout

### Recommended Data Partition Layout

```
/data/
  model/                     # Base model GGUF file(s)
    qwen2.5-7b-instruct-q4_k_m.gguf
  adapters/                  # Skill pack directory
    medical-terminology-v1/
      adapter.gguf
      metadata.json
    coding-assistant-v1/
      adapter.gguf
      metadata.json
    company-acme/
      adapter.gguf
      metadata.json
  training/                  # Training data for on-device fine-tuning (Phase 4)
    datasets/
      my-training-data.jsonl
    output/
      my-custom-adapter/
        adapter.gguf
        metadata.json
  config/
    adapters.json            # User preferences: which adapters are enabled by default
```

### adapters.json (User Preferences)

```json
{
  "default_adapters": [
    {"name": "medical-terminology-v1", "scale": 1.0},
    {"name": "formal-tone", "scale": 0.5}
  ],
  "max_loaded_adapters": 20,
  "auto_load_new": true
}
```

---

## 18. Comparison: llama.cpp vs. vLLM LoRA Support

| Feature | llama.cpp | vLLM |
|---------|-----------|------|
| Per-request adapter selection | Yes (lora field in request) | Yes (model field in request) |
| Multiple simultaneous adapters | Yes (--lora multiple times) | Yes (--lora-modules) |
| Runtime hot-swap | Yes (POST /lora-adapters) | Yes (v1/load_lora_adapter) |
| Adapter scaling | Yes (--lora-scaled, per-request scale) | Limited |
| CPU support | Full | No (GPU only) |
| Multi-adapter batching | No (different configs not batched) | Yes (optimized batching) |
| Max simultaneous adapters | Limited by RAM | Hundreds (GPU memory) |
| S-LoRA integration | No | Partial |
| GGUF adapter format | Native | No (uses safetensors) |
| Startup load without apply | Yes (--lora-init-without-apply) | N/A |

**For Llamaste**: llama.cpp is the clear choice. It has everything needed, works on CPU, and supports the GGUF format natively. vLLM's advantages (batching, GPU optimization) are irrelevant for Llamaste's use case.

---

## 19. Summary of Recommendations

1. **Phase 1**: No LoRA. Just reserve `/data/adapters/` in the partition layout.

2. **Phase 2**: Load adapters at startup. Implement adapter scanning, validation, LLM tools, and basic web UI. Use `--lora-init-without-apply` to pre-load all adapters. Estimated effort: 2-3 days.

3. **Phase 3**: Runtime management. Implement .skillpack format, upload/install flow, per-session adapter selection, and optional conversion pipeline. Estimated effort: 1-2 weeks.

4. **Phase 4**: On-device fine-tuning (experimental). Only for small models. Set clear expectations about training time. Estimated effort: 2-4 weeks.

5. **Security**: SHA256 verification from day one. Weight-space analysis in Phase 4. Never auto-load adapters from untrusted sources without user confirmation.

6. **Default rank recommendation**: rank 16, alpha 32, q8_0 quantization. Good balance of quality, size, and performance for most use cases.

7. **Storage budget**: Reserve 1-5 GB for adapters on the data partition. Supports 20-500 adapters depending on rank.

8. **Performance impact**: Negligible for typical use (< 15% inference overhead, < 500 MB RAM for 20 adapters). No concern for Llamaste's target hardware.

9. **User-facing terminology**: Call them "skill packs" not "LoRA adapters." Users do not need to know the technical details.

---

## Sources

- [llama.cpp Server README (LoRA API)](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
- [GGUF-my-LoRA (HuggingFace Blog)](https://huggingface.co/blog/ngxson/gguf-my-lora)
- [convert_lora_to_gguf.py Source](https://github.com/ggml-org/llama.cpp/blob/master/convert_lora_to_gguf.py)
- [Fast Inference with GGUF LoRA Adapters on CPU](https://kaitchup.substack.com/p/fast-inference-with-gguf-lora-adapters)
- [Finetune LoRA on CPU using llama.cpp](https://rentry.co/cpu-lora)
- [llama.cpp CPU LoRA Training Metrics](https://rentry.co/cpu-lora-metrics)
- [S-LoRA: Serving Thousands of Concurrent LoRA Adapters (Paper)](https://arxiv.org/pdf/2311.03285)
- [S-LoRA Blog Post (LMSYS)](https://lmsys.org/blog/2023-11-15-slora/)
- [Activated LoRA Feature Request (llama.cpp #15212)](https://github.com/ggml-org/llama.cpp/issues/15212)
- [LoRA Supply Chain Attacks (DEV Community)](https://dev.to/cyberpath/supply-chain-attacks-on-ai-models-how-attackers-inject-backdoors-through-poisoned-lora-adapters-1eb)
- [Weight Space Detection of Backdoors in LoRA Adapters](https://arxiv.org/html/2602.15195)
- [OWASP LLM Supply Chain Risks](https://genai.owasp.org/llmrisk/llm032025-supply-chain/)
- [LoRA Rank Selection Strategies](https://apxml.com/courses/lora-peft-efficient-llm-training/chapter-2-lora-in-depth/lora-rank-selection)
- [LoRA Rank Trade-offs (ACL 2025)](https://arxiv.org/html/2512.15634v1)
- [Unsloth AI](https://unsloth.ai/)
- [LoRA Hyperparameters Guide (Unsloth)](https://unsloth.ai/docs/get-started/fine-tuning-llms-guide/lora-hyperparameters-guide)
- [vLLM LoRA Adapters Documentation](https://docs.vllm.ai/en/latest/features/lora/)
- [TGI Multi-LoRA Serving (HuggingFace Blog)](https://huggingface.co/blog/multi-lora-serving)
- [EdgeLoRA: Multi-Tenant LLM Serving](https://arxiv.org/pdf/2507.01438)
- [llamafile LoRA PR #786](https://github.com/mozilla-ai/llamafile/pull/786)
- [Fine-Tuning LLMs for Domain-Specific Applications (2025)](https://www.johal.in/fine-tuning-llms-for-domain-specific-applications-using-hugging-face-transformers-and-lora-in-2025-2/)
