# Design: Four Immediate Items (2026-03-12)

## 1. NVMe Install — Documentation Only

The installer (`tools_install.cpp`) already handles generic block device install via
`/sys/block/` scanning. NVMe devices (`/dev/nvme0n1`) enumerate alongside SATA/USB.
No code changes needed — add NVMe notes to INSTALL.md.

**Deliverable**: INSTALL.md update with NVMe device naming and BIOS tips.

## 2. Remote Access — HTTP Debug Endpoints

Dropbear SSH requires a shell binary (Llamaste has `BR2_SYSTEM_BIN_SH_NONE=y`).
Instead, add HTTP debug endpoints to child_main.cpp:

| Endpoint | Returns |
|---|---|
| `/debug/logs` | Last 100 lines of llama-server log |
| `/debug/dmesg` | Kernel ring buffer (read `/dev/kmsg`) |
| `/debug/wpa` | wpa_supplicant debug log |
| `/debug/sysinfo` | CPU, RAM, disk, uptime, boot mode, kernel version |
| `/debug/modules` | Loaded kernel modules |
| `/debug/network` | Interface list, IPs, routes, DNS |

Protected by existing auth (Bearer token). No new Buildroot packages needed.

## 3. Grammar-Constrained Tool JSON

Add `response_format` to `build_inference_request()` in agent.cpp.
llama-server converts JSON schema to GBNF grammar automatically.

Schema: union type allowing either plain text response or tool call:
```json
{
  "type": "json_schema",
  "schema": {
    "type": "object",
    "properties": {
      "choices": {
        "type": "array",
        "items": {
          "type": "object",
          "properties": {
            "message": {
              "type": "object",
              "properties": {
                "content": { "type": ["string", "null"] },
                "tool_calls": {
                  "type": "array",
                  "items": {
                    "type": "object",
                    "properties": {
                      "function": {
                        "type": "object",
                        "properties": {
                          "name": { "type": "string" },
                          "arguments": { "type": "string" }
                        },
                        "required": ["name", "arguments"]
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}
```

**Note**: llama-server already returns OpenAI-compatible responses. The grammar
constrains the *generation* to valid JSON, preventing malformed tool calls.
The schema should NOT wrap the full response structure — just constrain
the content/tool_calls fields that the model generates.

**Implementation**: Add `response_format` with `type: "json_object"` as a
simpler first pass. This ensures valid JSON output without needing a full
schema. Full schema can be added later if needed.

## 4. Public GitHub Repo

1. Create `README.md` — overview, features, quick start, architecture
2. Create `LICENSE` — Apache 2.0 (full text)
3. Add untracked files to `.gitignore` (JPGs, analysis txt)
4. Commit and push
