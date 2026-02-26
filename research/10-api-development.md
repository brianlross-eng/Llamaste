# Building APIs on Top of llama.cpp

## Overview

This document covers approaches to building API layers on llama.cpp for Llamaste, including the built-in server endpoints, OpenAI compatibility, Python bindings, authentication, streaming, function calling, multi-model serving, and production architecture patterns.

---

## 1. llama-server Built-in Endpoints

llama.cpp ships with a full HTTP server (`llama-server`) that provides both inference and management endpoints.

### Inference Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/completion` | POST | Legacy completion with full parameter control |
| `/v1/chat/completions` | POST | OpenAI-compatible chat completions |
| `/v1/completions` | POST | OpenAI-compatible text completions |
| `/v1/embeddings` | POST | OpenAI-compatible embeddings |
| `/infill` | POST | Fill-in-the-middle (code completion) |
| `/tokenize` | POST | Convert text to tokens |
| `/detokenize` | POST | Convert tokens to text |

### Management Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/v1/models` | GET | List loaded models (OpenAI-compatible) |
| `/health` | GET | Health check (status: ok/loading/error) |
| `/metrics` | GET | Prometheus-format metrics |
| `/props` | GET | Server properties (context size, model info) |
| `/slots` | GET | Concurrent request slot status |

### Key Server Flags
```bash
./llama-server \
    -m model.gguf \
    --host 0.0.0.0 \
    --port 8080 \
    -c 4096 \              # Context size
    -np 4 \                # Number of parallel slots (concurrent requests)
    -cb \                  # Enable continuous batching
    --api-key "secret" \   # Simple API key authentication
    -t 8 \                 # Generation threads
    -tb 16 \               # Batch processing threads
    --mlock \              # Lock model in RAM
    --metrics              # Enable /metrics endpoint
```

### /completion Request Body
```json
{
    "prompt": "Once upon a time",
    "n_predict": 128,
    "temperature": 0.7,
    "top_k": 40,
    "top_p": 0.95,
    "min_p": 0.05,
    "repeat_penalty": 1.1,
    "stream": true,
    "stop": ["\n\n", "User:"],
    "cache_prompt": true,
    "grammar": "",
    "json_schema": null,
    "seed": -1
}
```

### /v1/chat/completions (OpenAI Format)
```json
{
    "model": "local-model",
    "messages": [
        {"role": "system", "content": "You are a helpful assistant."},
        {"role": "user", "content": "Hello!"}
    ],
    "max_tokens": 512,
    "temperature": 0.7,
    "stream": true,
    "tools": [],
    "response_format": {"type": "json_object"}
}
```

---

## 2. OpenAI API Compatibility

### What's Supported
- Chat completions (`/v1/chat/completions`)
- Text completions (`/v1/completions`)
- Embeddings (`/v1/embeddings`)
- Model listing (`/v1/models`)
- Streaming via SSE (`stream: true`)
- Function calling / tool use
- JSON mode (`response_format: {type: "json_object"}`)
- Temperature, top_p, top_k, frequency_penalty, presence_penalty
- Stop sequences
- Max tokens

### What's NOT Supported
- Batch API (`/v1/batches`)
- Assistants API (threads, runs, files)
- Audio/speech endpoints
- Image generation
- Fine-tuning API
- Files/uploads API
- Moderation endpoint

### Drop-in Replacement Pattern
Any application using the OpenAI Python library can switch to llama-server:
```python
from openai import OpenAI

client = OpenAI(
    base_url="http://localhost:8080/v1",
    api_key="not-needed"  # or your --api-key value
)

response = client.chat.completions.create(
    model="local",  # model name is ignored, uses loaded model
    messages=[{"role": "user", "content": "Hello!"}],
    stream=True
)

for chunk in response:
    if chunk.choices[0].delta.content:
        print(chunk.choices[0].delta.content, end="")
```

---

## 3. Three API Architecture Approaches

### Approach 1: Reverse Proxy (Simplest)
```
Client -> nginx/Caddy -> llama-server
```
- llama-server handles all inference
- nginx adds TLS, rate limiting, auth, caching
- Zero additional code
- Best for: single-model deployment

### Approach 2: Custom C/C++ Server
- Fork or extend llama-server's httplib-based server
- Add custom endpoints alongside inference
- Single process, minimal overhead
- Best for: appliance devices (Llamaste)

### Approach 3: Python Wrapper (Most Flexible)
```
Client -> FastAPI app -> llama-cpp-python (in-process)
                      -> or HTTP to llama-server
```
- Full Python ecosystem (auth, middleware, ORMs, etc.)
- Can embed llama.cpp directly via ctypes
- Best for: feature-rich API with business logic

---

## 4. llama-cpp-python

### What It Is
Python bindings for llama.cpp using ctypes. Provides both low-level C API access and a high-level `Llama` class.

### Installation
```bash
# CPU only
pip install llama-cpp-python

# With GPU (CUDA)
CMAKE_ARGS="-DGGML_CUDA=on" pip install llama-cpp-python
```

### Basic Usage
```python
from llama_cpp import Llama

llm = Llama(
    model_path="model.gguf",
    n_ctx=4096,
    n_threads=8,
    n_gpu_layers=0,  # CPU only
    verbose=False
)

# Chat completion
response = llm.create_chat_completion(
    messages=[
        {"role": "system", "content": "You are helpful."},
        {"role": "user", "content": "What is 2+2?"}
    ],
    max_tokens=256,
    temperature=0.7
)
print(response["choices"][0]["message"]["content"])
```

### Built-in FastAPI Server
```bash
# Start OpenAI-compatible server
python -m llama_cpp.server \
    --model model.gguf \
    --n_ctx 4096 \
    --host 0.0.0.0 \
    --port 8080
```

### Function Calling
```python
from llama_cpp import Llama
from llama_cpp.llama_chat_format import Llava15ChatHandler

llm = Llama(model_path="model.gguf", n_ctx=4096)

tools = [{
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "Get current weather",
        "parameters": {
            "type": "object",
            "properties": {
                "location": {"type": "string"},
                "unit": {"type": "string", "enum": ["celsius", "fahrenheit"]}
            },
            "required": ["location"]
        }
    }
}]

response = llm.create_chat_completion(
    messages=[{"role": "user", "content": "What's the weather in Paris?"}],
    tools=tools,
    tool_choice="auto"
)
# Model outputs structured JSON matching tool schema
```

---

## 5. Ollama API

### Overview
Ollama wraps llama.cpp with a model management layer and simpler API.

### Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/generate` | POST | Text generation |
| `/api/chat` | POST | Chat completion |
| `/api/embed` | POST | Embeddings |
| `/api/tags` | GET | List local models |
| `/api/show` | POST | Model info |
| `/api/pull` | POST | Download model |
| `/api/push` | POST | Upload model |
| `/api/create` | POST | Create model from Modelfile |
| `/api/delete` | DELETE | Remove model |

### Modelfile Format
```
FROM qwen2.5:7b
PARAMETER temperature 0.7
PARAMETER top_p 0.9
SYSTEM "You are a helpful assistant."
TEMPLATE """{{ if .System }}<|im_start|>system
{{ .System }}<|im_end|>
{{ end }}{{ if .Prompt }}<|im_start|>user
{{ .Prompt }}<|im_end|>
{{ end }}<|im_start|>assistant
"""
```

### Relevance to Llamaste
Ollama's model management UX (pull, list, delete) is excellent. Llamaste could implement similar model management endpoints without adopting the full Ollama stack.

---

## 6. Authentication and Security

### llama-server Built-in
```bash
# Simple API key
./llama-server --api-key "your-secret-key"
```
Client sends: `Authorization: Bearer your-secret-key`

### nginx Reverse Proxy Authentication
```nginx
server {
    listen 443 ssl;
    server_name llm.local;

    ssl_certificate /etc/ssl/llm.crt;
    ssl_certificate_key /etc/ssl/llm.key;

    # API key validation
    location /v1/ {
        if ($http_authorization != "Bearer your-secret-key") {
            return 401;
        }

        proxy_pass http://127.0.0.1:8080;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

### Rate Limiting (nginx)
```nginx
# Define rate limit zone
limit_req_zone $binary_remote_addr zone=llm:10m rate=10r/m;

location /v1/chat/completions {
    limit_req zone=llm burst=5 nodelay;
    proxy_pass http://127.0.0.1:8080;
}
```

### Llamaste Security Model
For a LAN appliance:
- Default: no authentication (trusted LAN)
- Optional: API key via web UI configuration
- Optional: TLS with self-signed certificate
- Firewall rules restrict to LAN subnet

---

## 7. SSE Streaming

### Server-Sent Events Protocol
```
HTTP/1.1 200 OK
Content-Type: text/event-stream
Cache-Control: no-cache
Connection: keep-alive

data: {"choices": [{"delta": {"content": "Hello"}}]}

data: {"choices": [{"delta": {"content": " world"}}]}

data: [DONE]
```

### Critical nginx Configuration for Streaming
```nginx
location /v1/chat/completions {
    proxy_pass http://127.0.0.1:8080;

    # CRITICAL: Disable buffering for SSE
    proxy_buffering off;
    proxy_cache off;

    # Timeouts for long-running generation
    proxy_read_timeout 300s;
    proxy_send_timeout 300s;

    # SSE headers
    proxy_set_header Connection '';
    proxy_http_version 1.1;
    chunked_transfer_encoding off;
}
```

**`proxy_buffering off` is critical.** Without it, nginx buffers the entire response before sending to the client, destroying the streaming experience.

### JavaScript Client
```javascript
async function streamChat(messages) {
    const response = await fetch('/v1/chat/completions', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({
            messages: messages,
            stream: true,
            max_tokens: 512
        })
    });

    const reader = response.body.getReader();
    const decoder = new TextDecoder();

    while (true) {
        const {value, done} = await reader.read();
        if (done) break;

        const chunk = decoder.decode(value);
        const lines = chunk.split('\n');

        for (const line of lines) {
            if (line.startsWith('data: ') && line !== 'data: [DONE]') {
                const data = JSON.parse(line.slice(6));
                const content = data.choices?.[0]?.delta?.content;
                if (content) {
                    process.stdout.write(content);
                }
            }
        }
    }
}
```

---

## 8. Function Calling / Tool Use

### How It Works in llama-server
1. Client sends tool definitions with the chat request
2. Server applies **grammar constraints** to force valid JSON output
3. Model generates structured JSON matching the tool schema
4. Client receives tool call, executes function, sends result back
5. Multi-turn: tool results appended as messages, model continues

### Grammar-Constrained Generation
llama-server uses GBNF grammar or JSON schema to constrain output:
```json
{
    "messages": [...],
    "tools": [{
        "type": "function",
        "function": {
            "name": "search",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": {"type": "string"}
                },
                "required": ["query"]
            }
        }
    }],
    "tool_choice": "auto"
}
```

The server internally converts the JSON schema to a GBNF grammar and constrains token sampling, guaranteeing valid JSON output.

### Multi-Turn Tool Use Flow
```
User: "What's the weather in Paris?"
  -> Model: tool_call(get_weather, {location: "Paris"})
  -> Client executes get_weather("Paris") -> "15°C, cloudy"
  -> Client sends: {role: "tool", content: "15°C, cloudy", tool_call_id: "..."}
  -> Model: "The weather in Paris is currently 15°C and cloudy."
```

---

## 9. Multi-Model Serving

### Approach 1: Separate Instances + Load Balancer
```
nginx (load balancer)
├── llama-server :8081 (Qwen2.5 7B - general)
├── llama-server :8082 (Qwen2.5-Coder 7B - code)
└── llama-server :8083 (Qwen2.5 1.5B - fast/simple)
```

```nginx
upstream llm_general { server 127.0.0.1:8081; }
upstream llm_code    { server 127.0.0.1:8082; }
upstream llm_fast    { server 127.0.0.1:8083; }

location /v1/chat/completions {
    # Route by model name in request body
    # (requires lua or njs module for body inspection)
    proxy_pass http://llm_general;
}
```

### Approach 2: Ollama Hot-Swap
Ollama keeps one model loaded at a time, swapping on demand:
- First request for model X: load from disk (slow)
- Subsequent requests: instant (model cached in RAM)
- Request for model Y: unload X, load Y
- LRU eviction when RAM limit reached

### Approach 3: LiteLLM Orchestrator
```python
# config.yaml
model_list:
  - model_name: "general"
    litellm_params:
      model: "openai/local"
      api_base: "http://localhost:8081/v1"
  - model_name: "code"
    litellm_params:
      model: "openai/local"
      api_base: "http://localhost:8082/v1"
```
```bash
litellm --config config.yaml --port 4000
```
- Unified API endpoint
- Routes by model name
- Built-in rate limiting, logging, caching
- Supports 100+ provider formats

---

## 10. Production Architecture Patterns

### Pattern 1: Llamaste Minimal (Phase 1-2)
```
Client Browser
    |
    v
llama-server (port 8080)
    ├── /v1/* (inference API)
    ├── / (web UI - built into llama-server)
    └── /health (monitoring)
```
Zero additional components. llama-server handles everything.

### Pattern 2: Llamaste with Management API (Phase 2+)
```
Client Browser
    |
    v
llama-server (port 8080)
    ├── /v1/* (inference)
    └── / (chat web UI)

Management daemon (port 8081)
    ├── /api/system (RAM, CPU, temp, uptime)
    ├── /api/models (list, download, switch)
    ├── /api/config (settings CRUD)
    └── /api/cluster (mesh status)
```

### Pattern 3: Llamaste Full Production (Phase 3+)
```
Client Browser / API Consumer
    |
    v
nginx (port 443, TLS)
    ├── / -> llama-server web UI
    ├── /v1/* -> llama-server (inference)
    ├── /api/* -> management daemon
    └── /metrics -> Prometheus metrics

llama-server (port 8080, localhost only)
management daemon (port 8081, localhost only)
node_exporter (port 9100, localhost only)
```

### Llamaste API Design Principles
1. **OpenAI-compatible first**: `/v1/*` endpoints match OpenAI spec
2. **Management API separate**: System management on different port
3. **No external dependencies**: No Redis, no PostgreSQL, no message queues
4. **Configuration as JSON files**: `/data/config.json` (persistent partition)
5. **SSE streaming default**: All generation endpoints stream by default
6. **Health endpoint always available**: Even during model loading

---

## 11. Metrics and Monitoring

### llama-server /metrics (Prometheus Format)
```
# Prompt tokens processed per second
llamacpp:prompt_tokens_seconds{...}

# Generation tokens per second
llamacpp:tokens_predicted_seconds{...}

# KV cache usage
llamacpp:kv_cache_usage_ratio{...}

# Number of active requests
llamacpp:requests_processing{...}

# Queue depth
llamacpp:requests_pending{...}
```

### Llamaste Dashboard Metrics
Key metrics to surface in the web management UI:
- Current tokens/second (generation + prompt)
- RAM usage (model + KV cache + buffers)
- CPU utilization per core
- Active/queued requests
- Model info (name, size, quantization)
- Uptime
- Temperature (if available via hwmon)
