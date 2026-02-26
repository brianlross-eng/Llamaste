# MCP (Model Context Protocol) Servers for LLM Integration

## Overview

MCP is an open protocol from Anthropic that standardizes how AI applications connect to external tools, data sources, and services. For Llamaste, MCP enables the device to be discovered and used by AI-powered applications (Claude Desktop, VS Code, custom agents) as a local inference provider.

---

## 1. MCP Architecture

### Core Components

```
Host (Claude Desktop, IDE, custom app)
  └── Client (manages 1:1 connection to a server)
       └── Server (exposes tools, resources, prompts)
```

- **Host**: The AI application that initiates connections
- **Client**: Protocol handler, one per server connection
- **Server**: Provides capabilities (tools, resources, prompts) to the client
- **Transport**: Communication layer (stdio or Streamable HTTP)
- **Protocol**: JSON-RPC 2.0 with capability negotiation

### Message Flow
```
Client                    Server
  |--- initialize -------->|     (capability negotiation)
  |<-- initialize result --|
  |--- initialized ------->|     (ready)
  |                         |
  |--- tools/list --------->|     (discover available tools)
  |<-- tools list ----------|
  |                         |
  |--- tools/call --------->|     (invoke a tool)
  |<-- tool result ---------|
```

---

## 2. Three Server Primitives

### Tools (Model-Controlled)
Functions the AI model can invoke to take actions or retrieve dynamic data.

```python
@mcp.tool()
async def search_web(query: str, max_results: int = 5) -> str:
    """Search the web and return results."""
    # Implementation
    return results
```

- Discovered via `tools/list`
- Invoked via `tools/call`
- Model decides when to call based on user request
- Input validated against JSON Schema generated from type hints

### Resources (Application-Controlled)
Read-only data sources the application can access (like GET endpoints).

```python
@mcp.resource("config://settings")
async def get_settings() -> str:
    """Current server configuration."""
    return json.dumps(config)

@mcp.resource("models://list")
async def list_models() -> str:
    """Available models on this Llamaste node."""
    return json.dumps(model_list)
```

- URI-based addressing
- Application decides when to read
- Can be static or dynamic
- Supports subscriptions for change notifications

### Prompts (User-Controlled)
Reusable prompt templates that users can select.

```python
@mcp.prompt()
def code_review(code: str, language: str = "python") -> str:
    """Review code for bugs and improvements."""
    return f"Review this {language} code for bugs, security issues, and improvements:\n\n```{language}\n{code}\n```"
```

- User explicitly selects from prompt list
- Can include arguments
- Pre-formatted for optimal model interaction

---

## 3. Transports

### stdio (Local Subprocess)
- Host spawns server as child process
- Communication via stdin/stdout
- Simplest setup, zero network configuration
- Used by Claude Desktop, VS Code extensions

```json
// Claude Desktop config (~/.config/claude/claude_desktop_config.json)
{
    "mcpServers": {
        "llamaste": {
            "command": "python",
            "args": ["-m", "llamaste_mcp"],
            "env": {
                "LLAMASTE_URL": "http://192.168.1.100:8080"
            }
        }
    }
}
```

### Streamable HTTP (Remote/Network)
- Server runs as HTTP service
- Client connects via HTTP POST + SSE
- Session management via `Mcp-Session-Id` header
- Supports remote servers across network

```python
# Server
mcp.run(transport="sse", host="0.0.0.0", port=8081)

# Client connects to http://192.168.1.100:8081/sse
```

### Transport Comparison

| Feature | stdio | Streamable HTTP |
|---------|-------|----------------|
| Setup | Zero-config | Requires port |
| Network | Local only | LAN/WAN |
| Security | Process isolation | Needs auth |
| Use case | Desktop apps | Remote devices |

---

## 4. Python SDK (FastMCP)

### Installation
```bash
pip install mcp
# or
pip install "mcp[cli]"  # includes CLI tools
```

### Basic Server
```python
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("my-server")

@mcp.tool()
def add(a: int, b: int) -> int:
    """Add two numbers."""
    return a + b

@mcp.resource("greeting://hello")
def hello() -> str:
    return "Hello from MCP!"

if __name__ == "__main__":
    mcp.run()  # Default: stdio transport
```

### Type Hints -> JSON Schema
FastMCP automatically converts Python type hints to JSON Schema:

```python
from typing import Optional
from pydantic import BaseModel

class SearchParams(BaseModel):
    query: str
    max_results: int = 10
    language: Optional[str] = None

@mcp.tool()
async def search(params: SearchParams) -> str:
    """Search with structured parameters."""
    # params is validated automatically
    ...
```

### Running Modes
```python
# stdio (for Claude Desktop)
mcp.run(transport="stdio")

# HTTP/SSE (for network access)
mcp.run(transport="sse", host="0.0.0.0", port=8081)
```

---

## 5. TypeScript SDK

### Installation
```bash
npm install @modelcontextprotocol/sdk
```

### Basic Server
```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";

const server = new McpServer({ name: "my-server", version: "1.0.0" });

server.registerTool(
    "search",
    { query: z.string(), maxResults: z.number().default(10) },
    async ({ query, maxResults }) => {
        // Implementation
        return { content: [{ type: "text", text: results }] };
    }
);

const transport = new StdioServerTransport();
await server.connect(transport);
```

---

## 6. MCP vs Alternatives

### MCP vs OpenAI Function Calling

| Aspect | MCP | OpenAI Functions |
|--------|-----|-----------------|
| Protocol | JSON-RPC 2.0 | Part of chat API |
| Discovery | Dynamic (tools/list) | Static in request |
| Transport | stdio, HTTP | HTTP only |
| Bidirectional | Yes (sampling, elicitation) | No |
| Resources | Yes (read-only data) | No |
| Prompts | Yes (templates) | No |
| Standard | Open protocol | Vendor API |

### MCP vs LangChain Tools

| Aspect | MCP | LangChain |
|--------|-----|-----------|
| Language | Any (protocol-based) | Python/JS |
| Hosting | Separate process | In-process |
| Reusability | Universal | Framework-specific |
| Discovery | Protocol-native | Code-level |

### Key MCP Advantage
MCP servers are **reusable across any MCP client**. Write once, use from Claude Desktop, VS Code, custom agents, or any MCP-compatible application.

---

## 7. Security

### Authentication
- stdio: Inherits process permissions (no auth needed)
- HTTP: OAuth 2.1 recommended for remote servers
- API key as simpler alternative for LAN devices

### Human-in-the-Loop
MCP servers can request confirmation before taking actions:
- Tools marked as requiring confirmation
- Client presents approval UI to user
- Server waits for approval before executing

### Input Validation
- All tool inputs validated against JSON Schema
- Servers should sanitize inputs (SQL injection, path traversal, etc.)
- Never trust client-provided paths without validation

### Origin Validation (HTTP Transport)
```python
# Validate Origin header for HTTP transport
ALLOWED_ORIGINS = ["http://localhost:*", "https://claude.ai"]

@app.middleware("http")
async def validate_origin(request, call_next):
    origin = request.headers.get("origin", "")
    if not any(fnmatch(origin, pattern) for pattern in ALLOWED_ORIGINS):
        return Response(status_code=403)
    return await call_next(request)
```

---

## 8. Llamaste MCP Bridge Server

### Concept
A lightweight MCP server that bridges between MCP clients (Claude Desktop, VS Code) and the Llamaste device's llama-server API.

### Two Deployment Options

#### Option A: Bridge (stdio, runs on workstation)
```
Claude Desktop -> stdio -> llamaste-mcp-bridge -> HTTP -> Llamaste device
```
- MCP server runs on the user's computer
- Connects to Llamaste over LAN HTTP
- User configures Llamaste IP in environment variable
- No changes needed on Llamaste device

#### Option B: Bundled (HTTP, runs on Llamaste device)
```
Claude Desktop -> HTTP/SSE -> Llamaste device (MCP server + llama-server)
```
- MCP server runs on the Llamaste device itself
- Exposes Streamable HTTP transport on dedicated port
- Direct network access, no bridge process

### Bridge Implementation

```python
from mcp.server.fastmcp import FastMCP
import httpx
import os
import json

mcp = FastMCP("llamaste")
LLAMA_BASE = os.environ.get("LLAMASTE_URL", "http://localhost:8080")


@mcp.tool()
async def llamaste_chat(
    messages: list,
    max_tokens: int = 512,
    temperature: float = 0.7
) -> str:
    """Send a chat completion request to the local Llamaste LLM server."""
    async with httpx.AsyncClient() as client:
        resp = await client.post(
            f"{LLAMA_BASE}/v1/chat/completions",
            json={
                "messages": messages,
                "max_tokens": max_tokens,
                "temperature": temperature,
            },
            timeout=120.0,
        )
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]


@mcp.tool()
async def llamaste_complete(
    prompt: str,
    max_tokens: int = 256,
    temperature: float = 0.7
) -> str:
    """Send a text completion request to the local Llamaste LLM server."""
    async with httpx.AsyncClient() as client:
        resp = await client.post(
            f"{LLAMA_BASE}/completion",
            json={
                "prompt": prompt,
                "n_predict": max_tokens,
                "temperature": temperature,
            },
            timeout=120.0,
        )
        resp.raise_for_status()
        return resp.json()["content"]


@mcp.tool()
async def llamaste_embed(text: str) -> list:
    """Generate embeddings for text using the local Llamaste LLM server."""
    async with httpx.AsyncClient() as client:
        resp = await client.post(
            f"{LLAMA_BASE}/v1/embeddings",
            json={"input": text},
            timeout=30.0,
        )
        resp.raise_for_status()
        return resp.json()["data"][0]["embedding"]


@mcp.tool()
async def llamaste_health() -> str:
    """Check the health status of the Llamaste LLM server."""
    async with httpx.AsyncClient() as client:
        resp = await client.get(f"{LLAMA_BASE}/health", timeout=5.0)
        return f"Status: {resp.status_code}, Response: {resp.text}"


@mcp.tool()
async def llamaste_models() -> str:
    """List models available on the Llamaste server."""
    async with httpx.AsyncClient() as client:
        resp = await client.get(f"{LLAMA_BASE}/v1/models", timeout=5.0)
        resp.raise_for_status()
        return json.dumps(resp.json(), indent=2)


@mcp.resource("llamaste://status")
async def get_status() -> str:
    """Current Llamaste server status."""
    async with httpx.AsyncClient() as client:
        resp = await client.get(f"{LLAMA_BASE}/health", timeout=5.0)
        return resp.text


@mcp.resource("llamaste://props")
async def get_props() -> str:
    """Llamaste server properties (model info, context size, etc.)."""
    async with httpx.AsyncClient() as client:
        resp = await client.get(f"{LLAMA_BASE}/props", timeout=5.0)
        return resp.text


if __name__ == "__main__":
    mcp.run()  # stdio for Claude Desktop
```

### Claude Desktop Configuration
```json
{
    "mcpServers": {
        "llamaste": {
            "command": "python",
            "args": ["-m", "llamaste_mcp"],
            "env": {
                "LLAMASTE_URL": "http://192.168.1.100:8080"
            }
        }
    }
}
```

### Bundled Server (Streamable HTTP)
```python
# Same tools/resources as above, but:
if __name__ == "__main__":
    mcp.run(transport="sse", host="0.0.0.0", port=8081)
```

---

## 9. Advanced MCP Features for Llamaste

### Sampling (Server-Initiated LLM Calls)
MCP allows servers to request the *client's* LLM to do work:
```python
@mcp.tool()
async def analyze_with_context(document: str) -> str:
    """Analyze a document using the host's AI model."""
    # This asks the CLIENT's model (e.g., Claude) to process
    result = await mcp.sample(
        messages=[{
            "role": "user",
            "content": f"Summarize this document:\n\n{document}"
        }],
        max_tokens=500
    )
    return result.content
```

### Resource Subscriptions
```python
@mcp.resource("llamaste://metrics")
async def get_metrics() -> str:
    """Real-time metrics from Llamaste."""
    async with httpx.AsyncClient() as client:
        resp = await client.get(f"{LLAMA_BASE}/metrics", timeout=5.0)
        return resp.text

# Client can subscribe to changes
# Server notifies on metric updates
```

### Progress Reporting
```python
@mcp.tool()
async def generate_long_text(prompt: str, tokens: int = 2048) -> str:
    """Generate a long text with progress updates."""
    # Report progress during long operations
    async with mcp.progress("Generating text...") as progress:
        result = ""
        for chunk in stream_generation(prompt, tokens):
            result += chunk
            await progress.update(len(result) / tokens)
        return result
```

---

## 10. MCP Integration in Llamaste Architecture

### Phase 2: Basic MCP Bridge
- Ship `llamaste-mcp` as a pip-installable package
- Users install on workstation, configure Claude Desktop
- Zero changes to Llamaste device

### Phase 3: Bundled MCP Server
- MCP server runs on Llamaste device
- Streamable HTTP transport on port 8081
- Auto-discovered via mDNS alongside inference API
- Claude Desktop / VS Code connects directly to device

### Phase 4: Full MCP Ecosystem
- Llamaste as MCP tool provider (inference, embeddings, RAG)
- Llamaste as MCP resource provider (model catalog, metrics, logs)
- Mesh cluster nodes exposed as separate MCP resources
- MCP-based orchestration between multiple Llamaste devices

### Service Advertisement
```xml
<!-- Avahi service file for MCP -->
<service-group>
  <name replace-wildcards="yes">Llamaste MCP on %h</name>
  <service>
    <type>_mcp._tcp</type>
    <port>8081</port>
    <txt-record>transport=sse</txt-record>
    <txt-record>version=2025-03-26</txt-record>
    <txt-record>capabilities=tools,resources,prompts</txt-record>
  </service>
</service-group>
```
