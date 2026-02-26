# Research 18: Multi-User Support, Authentication & Session Management

**Date**: 2026-02-26
**Scope**: Multi-user concurrent access, authentication, authorization, session management, and security for the Llamaste single-binary LLM appliance.

---

## 1. How Existing Local LLM Servers Handle Multi-User

### 1.1 Ollama

**Concurrency**: Ollama 0.2+ enables concurrency by default. Key environment variables:
- `OLLAMA_NUM_PARALLEL`: max parallel requests per model (default 4, RAM-dependent)
- `OLLAMA_MAX_LOADED_MODELS`: max concurrent models (default 3)
- `OLLAMA_MAX_QUEUE`: request queue size before 503 rejection (default 512)

Requests exceeding `OLLAMA_NUM_PARALLEL` are queued FIFO. Queue overflow returns 503.

**Authentication**: None built-in. Ollama was designed as a single-user tool. Community guides describe adding auth via reverse proxies (nginx, Caddy) or external tools. Some guides describe group-based access control and API token generation, but these are community-configured solutions, not native Ollama features.

**Weakness**: Despite concurrency improvements, Ollama remains fundamentally single-user. Multiple concurrent Python clients report significant slowdowns. For high-concurrency needs, vLLM is recommended.

### 1.2 LM Studio

**Concurrency**: LM Studio 0.4.0 introduced parallel inference via llama.cpp's continuous batching. `Max Concurrent Predictions` defaults to 4. Unified KV cache allows variable context sizes per request.

**Authentication**: Optional token-based API authentication, disabled by default. Tokens support permission scopes (Base API Access, Integration Access, Tool-Level Access). Multiple tokens can be created with different permissions for different users. Token management UI in Developer Settings.

**Weakness**: Still primarily single-user focused. No user accounts, no RBAC beyond token scopes. Documentation warns against exposing port 1234 without auth enabled.

### 1.3 text-generation-webui (oobabooga)

**Authentication**: Two separate mechanisms:
- Web UI: `--gradio-auth "user1:pass1,user2:pass2"` (Gradio built-in)
- API: `--api-key` for general access, `--admin-key` for model loading/unloading

**Session management**: Multi-session support was added but described as "unstable." No native RBAC. The admin/user distinction exists only at the API level (admin-key vs api-key), not in the web UI.

**Weakness**: Minimal auth bolted onto Gradio. No session isolation, no user profiles, no conversation history separation.

### 1.4 Open WebUI

**Authentication**: The most mature auth system among local LLM UIs:
- JWT-based session management
- Three user roles: Admin, User, Pending (new accounts require admin approval)
- Group-based permissions with additive security model
- Granular permission flags ("Can Delete Chats", "Can Use Web Search", etc.)
- OAuth/SSO integration (enterprise), SCIM 2.0 provisioning
- Per-model access control via ACLs

**Session management**: Full user accounts with separate conversation histories, chat management (pin, rename, archive, share, tag), and persistent sessions.

**Strength**: Open WebUI is the gold standard for local LLM multi-user management. Its RBAC model is worth studying closely for Llamaste.

**Weakness**: Requires a database (SQLite/PostgreSQL). Heavy Python application, not embeddable in a single binary.

### 1.5 vLLM

**Concurrency**: Purpose-built for high-throughput multi-user serving. Continuous batching + PagedAttention enable hundreds of concurrent requests. Memory waste reduced 4x via virtual-memory-style paging.

**Authentication**: Only supports a single API key natively. Multi-tenant auth is an open feature request. Recommended approach: external API gateways (Istio, OAuth proxies, AIBrix Gateway) with tenant metadata headers (X-Tenant-ID, X-Priority).

**Weakness**: No built-in user management. Designed for infrastructure-level deployment behind an API gateway, not direct end-user access.

### 1.6 Summary: What Works, What's Missing

| Feature | Ollama | LM Studio | Oobabooga | Open WebUI | vLLM |
|---------|--------|-----------|-----------|------------|------|
| Concurrent requests | Yes (FIFO queue) | Yes (4 default) | Limited | Via backend | Excellent |
| Built-in auth | No | Token-based | Basic | Full RBAC + JWT | Single API key |
| User accounts | No | No | No | Yes | No |
| Role-based access | No | Token scopes | Admin/user API keys | Admin/User/Pending + groups | No |
| Session isolation | No | No | Unstable | Yes | N/A (API only) |
| Conversation history | No | Local only | Gradio sessions | Full per-user | N/A |

**Key insight**: No existing local LLM server combines inference + auth + RBAC + session management in a single binary. Open WebUI comes closest but requires a separate database and is a large Python application. Llamaste has the opportunity to be the first to integrate all of these into one binary.

---

## 2. llama.cpp Concurrent Request Handling

### 2.1 Slot System

llama-server uses **slots** (`-np N`) for concurrent request handling. Each slot is a logical scheduling unit with its own KV cache allocation.

- `-np N`: at most N requests processed simultaneously; excess requests are queued
- Default behavior: single slot (sequential processing)
- Each slot gets an independent partition of the KV cache
- `--cont-batching` (now default): enables continuous batching across slots

### 2.2 KV Cache Per Slot

Each slot maintains its own independent KV cache. The attention mask ensures tokens from one conversation cannot "see" tokens from another. This provides conversation isolation at the inference level.

**Unified KV buffer**: When enabled (default with multiple slots), the total KV cache is shared dynamically rather than hard-partitioned. This allows variable context lengths per slot.

**Cache reuse** (`cache_prompt: true`, default): Within a slot, if a new request shares a prefix with the previous request in that slot (e.g., same system prompt + prior conversation turns), only the new suffix is processed. This is critical for multi-turn conversations.

### 2.3 Slot Assignment

The `-sps` parameter (default 0.5) controls automatic slot assignment based on prompt similarity. A value of 0.5 means a slot matches if at least 50% of its cached context matches the new request. Clients can also pin requests to specific slots via `"id_slot"` in the request body.

### 2.4 Queuing Behavior

When all slots are busy, new requests are queued. There is no configurable queue size in llama-server itself (unlike Ollama's `OLLAMA_MAX_QUEUE`). Llamaste should implement its own queue with configurable limits.

### 2.5 Different System Prompts Per Slot

Supported. Each slot's cache is independent. You can pin users with different roles to different slots (e.g., admin system prompt on slot 0, guest system prompt on slot 1). The `id_slot` parameter in requests makes this explicit.

### 2.6 Host-Memory Prompt Caching (New, 2025)

A recent PR introduced `--cache-ram N` which allocates host RAM for prompt caching beyond the active slots. This acts as "extra virtual slots" for storing cached prefixes that can be quickly restored when a user returns.

### 2.7 Performance Impact of Concurrency

Benchmark data (8B model, single GPU):

| Concurrent Requests | Per-User Token Rate | Notes |
|---------------------|---------------------|-------|
| 1 | ~25 tok/s | Full speed |
| 3 | ~17 tok/s | Mild degradation |
| 10 | ~4 tok/s | Significant slowdown |
| 30 | ~1 tok/s | Barely usable |
| 100 | ~0.5 tok/s | Impractical |

Per-user throughput drops sharply because all slots share the same compute. Total throughput increases somewhat (batch efficiency), but individual latency degrades.

---

## 3. Session Isolation

### 3.1 Requirements

Each user of Llamaste needs:
- Their own conversation history (not mixed with other users)
- Persistent session across page refreshes
- Separate system prompt context (different tool permissions)
- Independence from other users' requests

### 3.2 Session ID Generation

Recommended approach for Llamaste: **server-generated opaque session tokens**.

1. On first connection (no session cookie), server generates a 128-bit random token (e.g., using `getrandom()` syscall or `/dev/urandom`)
2. Token is set as an HttpOnly, SameSite=Lax cookie: `Set-Cookie: llamaste_session=<token>; HttpOnly; SameSite=Lax; Path=/`
3. All subsequent requests include this cookie automatically
4. Server maps session token to user state (conversation history, role, slot assignment)

Why not URL parameters: Tokens leak in referrer headers, browser history, server logs. Why not localStorage: Cannot be made HttpOnly, vulnerable to XSS.

### 3.3 Server-Side Session Storage

For Llamaste (no database), sessions are stored in memory with periodic persistence to a JSON file on the data partition:

```
/data/sessions/
  sessions.json       # Session-to-user mapping, metadata
  history/
    <session-id>.jsonl  # Conversation history per session (append-only)
```

**In-memory state** (always present):
- Session token -> user role mapping
- Session token -> assigned slot ID
- Active conversation context (last N messages for prompt construction)

**On-disk state** (persisted periodically):
- Full conversation history (JSONL append-only log)
- Session metadata (creation time, last active, user role)

### 3.4 Session Expiry and Cleanup

| Session Type | Lifetime | Storage |
|--------------|----------|---------|
| Anonymous (no auth) | 24 hours idle timeout | Memory only, history on disk |
| Authenticated user | 7 days idle timeout | Memory + disk |
| API key session | Per-request (stateless) | No session needed |
| "Remember me" | 30 days | Long-lived cookie + disk |

Cleanup: A periodic task (every 5 minutes) evicts expired sessions from memory. Disk history files are retained for 30 days then deleted.

### 3.5 Shared Sessions

Not recommended for Phase 1. Future consideration: a "dashboard" mode where multiple browsers can view the same system status page without separate sessions.

---

## 4. Authentication Approaches

### 4.1 Comparison Table

| Method | Complexity | UX | Security | Use Case | Phase |
|--------|-----------|-----|----------|----------|-------|
| **No auth** | None | Best | None | Trusted home LAN, single user | 1 (default) |
| **Device PIN** | Low | Good | Low | Family device, casual access barrier | 1 |
| **Shared password** | Low | Good | Low-Med | Small office, single password for all | 1 |
| **User accounts** | Medium | Moderate | Medium | Multi-user with individual permissions | 2 |
| **API keys** | Low | N/A (API) | Medium | Programmatic access, per-app | 1 |
| **HTTP Basic Auth** | Low | Poor | Low | Legacy compatibility, reverse proxy | Skip |
| **JWT tokens** | Medium | Transparent | Medium-High | Session management after login | 1 (internal) |
| **OAuth/OIDC proxy** | High | Good | High | Enterprise SSO integration | 3+ |
| **Client certificates** | High | Poor | High | Machine-to-machine, mesh cluster | 3+ |
| **mTLS** | High | N/A | Very High | Cluster node authentication | 3+ |

### 4.2 Recommended: Layered Approach

**Phase 1 — Minimal Viable Auth**:
- Default: no auth (home use). Anyone on the LAN can access the web UI.
- Optional: single device password (set via first-boot setup or config file)
- API keys: generated on first boot, stored in `/data/config/api-keys.json`
- Internal JWT for session tracking (not user-facing; used for cookie sessions)

**Phase 2 — User Accounts**:
- Username/password accounts stored in `/data/config/users.json`
- Password hashing with Argon2id (or bcrypt as fallback for constrained RAM)
- Admin account created during first-boot setup
- New users default to "pending" role (admin must approve)

**Phase 3+ — Enterprise**:
- OAuth/OIDC proxy support (for SSO with existing identity providers)
- mTLS for cluster node authentication
- LDAP integration via external proxy

### 4.3 Device PIN / Shared Password Implementation

For the simplest auth mode (Phase 1):
1. Admin sets a PIN/password via the web UI setup wizard or config file
2. Stored as Argon2id hash in `/data/config/device.json`
3. On first visit, user sees a PIN/password prompt
4. Correct entry creates a session cookie (JWT-signed, HttpOnly)
5. Session persists for configurable duration (default: 7 days)
6. No usernames, no roles — all authenticated users get the same access level

### 4.4 API Key Implementation

OpenAI API compatibility requires `Authorization: Bearer <key>` header support.

```
/data/config/api-keys.json:
{
  "keys": [
    {
      "id": "key_abc123",
      "hash": "<argon2id hash of full key>",
      "prefix": "sk-llm-abc1",
      "name": "Default API Key",
      "role": "user",
      "created": "2026-01-15T10:00:00Z",
      "last_used": "2026-02-26T14:30:00Z",
      "rate_limit": { "rpm": 60, "tpm": 100000 }
    }
  ]
}
```

Key format: `sk-llm-<32 random hex chars>` (similar to OpenAI's `sk-` prefix convention). Only the hash is stored; the full key is shown once at creation.

---

## 5. Authorization and Role-Based Access

### 5.1 Role Definitions

| Role | Chat | Read-Only Tools | Write Tools | Destructive Tools | Admin Settings | API Access |
|------|------|-----------------|-------------|-------------------|----------------|------------|
| **admin** | Yes | Yes | Yes | Yes | Yes | Full |
| **user** | Yes | Yes | Yes | No | No | Standard |
| **guest** | Yes | No | No | No | No | None |
| **api** | N/A | Per-key | Per-key | Per-key | No | Per-key |

### 5.2 Tool Permission Categories

```
read_only:    fs.list, fs.read, fs.stat, fs.search,
              process.list, process.info,
              network.status, network.interfaces,
              system.info, system.uptime, system.ram,
              model.info, model.list

write:        fs.write, fs.mkdir, fs.copy, fs.move,
              network.configure, config.set,
              process.start

destructive:  fs.delete, fs.format,
              process.kill, process.killall,
              system.reboot, system.shutdown,
              config.reset, model.delete

admin_only:   user.create, user.delete, user.set_role,
              apikey.create, apikey.revoke,
              system.update, cluster.join, cluster.leave
```

### 5.3 Implementation Without a Database

All RBAC data lives in JSON files on the data partition:

```
/data/config/
  device.json       # Device-level settings (PIN hash, hostname, etc.)
  users.json        # User accounts: [{username, password_hash, role, created}]
  api-keys.json     # API keys: [{id, hash, prefix, name, role, rate_limit}]
  roles.json        # Role->permission mapping (editable, but ships with defaults)
```

At startup, Llamaste loads all config files into an in-memory `AuthManager` struct:
- `std::unordered_map<std::string, UserRecord> users_by_name`
- `std::unordered_map<std::string, SessionRecord> sessions_by_token`
- `std::unordered_map<std::string, ApiKeyRecord> api_keys_by_prefix`
- `std::unordered_map<std::string, RolePermissions> roles`

Changes are written back to disk immediately (fsync) after any mutation. The files are small (a few KB) so this is fast and safe against power loss.

### 5.4 Permission Check Flow

```
Request arrives
  -> Extract auth: cookie (session) or Authorization header (API key)
  -> If no auth and auth_required=false: assign "guest" role
  -> If no auth and auth_required=true: return 401
  -> Look up session/key -> get role
  -> Agent dispatches tool call
  -> Before executing tool: check role.has_permission(tool_name)
  -> If denied: return tool result "Permission denied: {tool} requires {role} role"
  -> LLM sees denial, explains to user naturally
```

The LLM never bypasses the permission check. Tool execution is gated at the C++ level before any syscall is made.

---

## 6. Concurrent Access Patterns

### 6.1 Resource Allocation Strategy

For Llamaste on a 16GB RAM system with a 7B Q4 model (~4GB):

| Component | RAM Budget |
|-----------|-----------|
| Model weights (Q4_K_M) | ~4.0 GB |
| KV cache (4 slots x 4K context, Q8_0) | ~1.0 GB |
| Host prompt cache | ~1.0 GB |
| OS + kernel | ~0.3 GB |
| Session state + web server | ~0.1 GB |
| Headroom / safety margin | ~1.6 GB |
| **Total** | **~8.0 GB** |

This leaves 8GB free on a 16GB system, or can support larger context/more slots.

### 6.2 Realistic User Limits

Based on benchmarks and memory analysis:

| RAM | Model | Max Slots | Comfortable Users | Context Per Slot |
|-----|-------|-----------|-------------------|------------------|
| 8 GB | 3B Q4 | 2 | 1-2 | 2K |
| 16 GB | 7B Q4 | 4 | 2-4 | 4K |
| 32 GB | 14B Q4 | 4-6 | 3-5 | 8K |
| 64 GB | 32B Q4 | 6-8 | 4-6 | 8K |

"Comfortable" means >5 tok/s per user for interactive chat.

### 6.3 Request Fairness

**Problem**: One user sending rapid-fire requests can monopolize all slots.

**Solution**: Fair-share scheduling with per-session quotas.

```
Per-session limits (configurable):
  max_concurrent_requests: 1      # Only 1 active generation per session
  max_queue_depth: 3              # Max 3 queued requests per session
  min_request_interval_ms: 500    # Rate limit: 2 requests/second max
  max_context_tokens: 4096        # Per-request context limit
```

Global queue uses **round-robin across sessions** rather than pure FIFO. If sessions A and B both have queued requests, they alternate: A, B, A, B... rather than A, A, A, B, B, B.

### 6.4 Slot Assignment Strategy

1. Each authenticated user gets a "preferred" slot (pinned via `id_slot`)
2. If their preferred slot is busy, request is queued for that slot
3. If the queue wait exceeds a threshold (e.g., 5s), reassign to any available slot (cache miss, but faster)
4. Anonymous/guest users share a pool of unpinned slots
5. Admin requests get priority: they jump to the front of the queue

---

## 7. Web UI Session Management

### 7.1 Cookie-Based Sessions (Recommended)

Llamaste should use HttpOnly cookies for session management:

```
Set-Cookie: llamaste_sid=<token>; HttpOnly; SameSite=Lax; Path=/; Max-Age=604800
```

- `HttpOnly`: JavaScript cannot read the cookie (XSS protection)
- `SameSite=Lax`: CSRF protection for same-site navigation
- `Path=/`: Cookie sent for all paths
- `Max-Age=604800`: 7-day expiry (configurable)
- No `Secure` flag: Llamaste runs on HTTP over LAN (no TLS by default)

When HTTPS is configured (optional), add the `Secure` flag.

### 7.2 Session Persistence

- Cookie survives browser restarts (persistent cookie with Max-Age)
- Server-side session state survives binary restarts (persisted to disk)
- Conversation history survives reboots (JSONL files on data partition)

### 7.3 Multiple Tabs

Same-origin cookies are shared across tabs. Multiple tabs from the same user automatically share the same session. The web UI should:
- Use SSE (Server-Sent Events) to push updates to all tabs
- Show a notification if another tab is actively generating
- Allow multiple conversations via a sidebar (like ChatGPT), not multiple parallel generations

### 7.4 Mobile Browser Considerations

- Cookies work identically on mobile browsers
- Session persistence may be shorter on iOS Safari (ITP may expire cookies)
- Touch-friendly UI is a Phase 2 concern
- No special session handling needed for mobile

### 7.5 "Remember Me" vs Session Timeout

| Mode | Cookie Lifetime | Use Case |
|------|----------------|----------|
| Default | 7 days | Normal home use |
| "Remember me" | 30 days | Personal device |
| Session-only | Browser close | Shared/public device |
| API | No cookie (per-request) | Programmatic access |

---

## 8. API Authentication

### 8.1 OpenAI-Compatible Bearer Token

Llamaste's API must accept the standard OpenAI auth header:

```
POST /v1/chat/completions
Authorization: Bearer sk-llm-abc123def456...
Content-Type: application/json
```

Implementation:
1. Extract `Authorization` header
2. Strip `Bearer ` prefix
3. Extract key prefix (first 12 chars) for lookup
4. Hash the full key with Argon2id
5. Compare against stored hash for that prefix
6. If match: proceed with request, applying the key's role permissions
7. If no match: return `{"error": {"message": "Invalid API key", "type": "authentication_error", "code": "invalid_api_key"}}` with HTTP 401

### 8.2 Per-Key Rate Limiting

Each API key has configurable rate limits:

```json
{
  "rate_limit": {
    "rpm": 60,          // Requests per minute
    "tpm": 100000,      // Tokens per minute
    "concurrent": 2     // Max concurrent requests
  }
}
```

Rate limiting uses a sliding window counter in memory. No external tools needed.

### 8.3 Key Management

- Keys are generated via the web UI admin panel or CLI tool
- Each key has a human-readable name, role assignment, and optional expiry
- Revocation is immediate (remove from `api-keys.json`, evict from memory)
- No key rotation mechanism in Phase 1; admin manually creates new key and revokes old

### 8.4 API Keys vs Web UI Users

API keys and web UI users are separate entities but share the same role system. An API key with role "user" has the same permissions as a web UI user with role "user." This keeps the permission model simple and consistent.

---

## 9. Security Considerations

### 9.1 Password Hashing

**Recommended: Argon2id** (PHC winner, RFC 9106).

For static linking into Llamaste, use the reference C implementation (`libargon2.a`) from the PHC repository. It compiles cleanly with musl libc and has no external dependencies.

Recommended parameters for a home LAN appliance:
- Memory: 64 MB (`m=65536`)
- Iterations: 3 (`t=3`)
- Parallelism: 4 (`p=4`)
- Salt: 16 bytes random per password
- Hash output: 32 bytes
- Estimated time: ~150ms on modern hardware

**Fallback: bcrypt** if RAM is extremely constrained (e.g., 4GB system where 64MB for hashing is significant). Bcrypt uses only 4KB RAM. Work factor 12 takes ~250ms.

Both algorithms are available in the **Botan** C++ crypto library, which also supports HMAC for JWT signing. Alternatively, use the standalone reference C libraries (smaller footprint).

### 9.2 Brute Force Protection

```
Login rate limiting:
  max_attempts: 5 per IP per 15 minutes
  lockout_duration: 15 minutes after 5 failures
  progressive_delay: 1s, 2s, 4s, 8s, 16s between attempts

API key rate limiting:
  Invalid key attempts: 10 per IP per minute
  Lockout: 5-minute block after 10 failures
```

Implementation: in-memory `std::unordered_map<ip_address, FailureRecord>` with periodic cleanup.

### 9.3 CSRF Protection

Llamaste uses a **Double Submit Cookie** pattern:

1. Server generates a random CSRF token and sets it as a cookie:
   `Set-Cookie: llamaste_csrf=<token>; SameSite=Lax; Path=/`
   (Note: NOT HttpOnly, because JS needs to read it)
2. Web UI JS reads the CSRF cookie and includes it as a header:
   `X-CSRF-Token: <token>`
3. Server verifies the header value matches the cookie value

For API requests using Bearer tokens, CSRF protection is not needed (Bearer auth is not auto-sent by browsers).

**Additional defense**: `SameSite=Lax` on the session cookie prevents most CSRF attacks on its own. The CSRF token is a defense-in-depth measure.

### 9.4 XSS Prevention

Chat applications are high-risk for XSS because user messages are displayed as HTML-like content. Llamaste must:

1. **Server-side**: HTML-encode all user input before storing. Escape `<`, `>`, `&`, `"`, `'` to their entity equivalents. Implement this in C++ as a simple character replacement function.

2. **Client-side**: Use `textContent` (not `innerHTML`) when inserting user messages into the DOM. For markdown rendering, use a sanitizing markdown library that strips raw HTML.

3. **Content Security Policy**: Set CSP headers to prevent inline script execution:
   ```
   Content-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'
   ```

4. **LLM output**: The LLM's responses also need sanitization. The LLM could be prompt-injected to output `<script>` tags. Apply the same encoding to LLM output as to user input.

### 9.5 Secure Cookie Configuration

```
Session cookie:    HttpOnly; SameSite=Lax; Path=/; Max-Age=604800
CSRF cookie:       SameSite=Lax; Path=/; Max-Age=604800  (NOT HttpOnly)
API responses:     No cookies (Bearer token auth only)
```

When HTTPS is enabled (optional), add `Secure` flag to both cookies.

### 9.6 Additional Security Headers

```
X-Content-Type-Options: nosniff
X-Frame-Options: DENY
Referrer-Policy: no-referrer
Permissions-Policy: camera=(), microphone=(), geolocation=()
```

The `X-Frame-Options: DENY` prevents clickjacking. `no-referrer` prevents session tokens from leaking in referrer headers (even though they are in cookies, not URLs).

---

## 10. Recommendations for Llamaste

### 10.1 Phase 1: Minimal Viable Auth

**Goal**: Secure enough for home use, simple enough to ship quickly.

Implement:
- [x] Session management with HttpOnly cookies (always-on, even without auth)
- [x] Anonymous sessions with conversation history persistence
- [x] Optional device password (single shared password for all users)
- [x] API key support (one default key auto-generated, OpenAI-compatible)
- [x] CSRF protection (double submit cookie)
- [x] XSS prevention (HTML encoding on all user/LLM output)
- [x] Security headers (CSP, X-Frame-Options, etc.)
- [x] Request rate limiting (per-IP)
- [x] 4 concurrent slots with round-robin fair scheduling

Skip in Phase 1:
- User accounts (everyone shares the same access level)
- Role-based access (all authenticated users are "admin")
- API key management UI (use config file or CLI)
- HTTPS/TLS (LAN only; reverse proxy can add TLS)

**Default configuration**: No auth required. Device password disabled. One API key in config file. This matches how Ollama, LM Studio, and oobabooga ship today.

### 10.2 Phase 2: Full User Management

**Goal**: Multi-user with individual accounts and role-based access.

Implement:
- [ ] User accounts (username + Argon2id-hashed password)
- [ ] Three roles: admin, user, guest
- [ ] Per-tool permission checks integrated into agent loop
- [ ] Admin web UI for user management
- [ ] API key management with per-key roles and rate limits
- [ ] Per-session slot pinning for KV cache efficiency
- [ ] Host-memory prompt cache (`--cache-ram`) for session persistence
- [ ] Brute force protection with progressive delays and lockout
- [ ] First-boot setup wizard (create admin account, set hostname)

### 10.3 Phase 3+: Enterprise Features

- OAuth/OIDC proxy support for SSO integration
- mTLS for cluster node authentication
- Audit logging (who did what, when)
- LDAP/Active Directory integration via external proxy
- HTTPS with auto-generated self-signed certificates
- Session sharing for collaborative dashboard views

### 10.4 Default Configurations

**Home deployment** (Phase 1 default):
```json
{
  "auth_required": false,
  "device_password": null,
  "max_slots": 4,
  "session_timeout_hours": 168,
  "api_keys_enabled": true,
  "default_role": "admin"
}
```

**Small business deployment** (Phase 2):
```json
{
  "auth_required": true,
  "device_password": null,
  "max_slots": 4,
  "session_timeout_hours": 24,
  "api_keys_enabled": true,
  "default_role": "pending",
  "require_admin_approval": true,
  "users_file": "/data/config/users.json",
  "brute_force_protection": true
}
```

### 10.5 Migration Path: No-Auth to Full Auth

1. **No-auth -> Device password**: Admin enables password in web UI settings. Existing anonymous sessions continue working until they expire. New connections require the password.

2. **Device password -> User accounts**: Admin creates user accounts. Device password becomes the initial admin password. Anonymous sessions are converted to "guest" sessions. Admin can assign roles to previously anonymous conversation histories by session ID.

3. **Data preservation**: Conversation history files are keyed by session ID, not username. When migrating from anonymous to authenticated, the session-to-user mapping in `sessions.json` is updated, but history files are untouched. No data loss during migration.

### 10.6 Implementation Priority

| Priority | Feature | Effort | Impact |
|----------|---------|--------|--------|
| 1 | Session cookies + conversation isolation | 2 days | Critical for multi-user |
| 2 | XSS prevention + CSP headers | 1 day | Critical for security |
| 3 | CSRF double-submit cookie | 0.5 days | Important for web UI |
| 4 | API key auth (Bearer token) | 1 day | Required for OpenAI compat |
| 5 | Optional device password | 1 day | Nice-to-have for Phase 1 |
| 6 | Rate limiting + fair scheduling | 1 day | Important for multi-user |
| 7 | User accounts + Argon2id | 2 days | Phase 2 |
| 8 | Role-based tool permissions | 2 days | Phase 2 |
| 9 | Admin UI for user management | 3 days | Phase 2 |
| 10 | HTTPS + TLS | 2 days | Phase 3 |

---

## 11. Technical Implementation Notes

### 11.1 C++ Libraries Needed

| Library | Purpose | Linking | Size |
|---------|---------|---------|------|
| Argon2 (reference C impl) | Password hashing | Static (`libargon2.a`) | ~50 KB |
| jwt-cpp (header-only) | JWT token signing/verification | Header-only | ~0 KB (headers) |
| OR: HMAC-SHA256 (from llama.cpp's ggml or custom) | JWT signing (simpler) | Already available | 0 KB |

**Simplification opportunity**: JWT tokens use HMAC-SHA256 for signing. llama.cpp already links OpenSSL or uses its own crypto primitives. Rather than adding jwt-cpp, implement minimal JWT (header.payload.signature) using existing HMAC-SHA256.

JWT structure for Llamaste sessions:
```json
Header:  {"alg": "HS256", "typ": "JWT"}
Payload: {"sid": "<session-id>", "role": "user", "iat": 1709000000, "exp": 1709604800}
Signature: HMAC-SHA256(base64url(header) + "." + base64url(payload), server_secret)
```

The server secret is generated once at first boot and stored in `/data/config/device.json`.

### 11.2 AuthManager Class Sketch

```cpp
class AuthManager {
public:
    // Initialization
    void load_config(const std::string& config_dir);
    void save_config();

    // Session management
    std::string create_session(const std::string& role);
    SessionRecord* get_session(const std::string& token);
    void expire_sessions();

    // Authentication
    bool verify_device_password(const std::string& password);
    bool verify_user_credentials(const std::string& username, const std::string& password);
    bool verify_api_key(const std::string& key);

    // Authorization
    bool has_permission(const std::string& role, const std::string& tool_name);
    std::string get_role_for_session(const std::string& session_token);

    // API key management
    std::string create_api_key(const std::string& name, const std::string& role);
    bool revoke_api_key(const std::string& key_id);

    // Rate limiting
    bool check_rate_limit(const std::string& ip, const std::string& key_id);
    void record_request(const std::string& ip, const std::string& key_id);

private:
    std::string server_secret_;  // For JWT signing
    std::unordered_map<std::string, SessionRecord> sessions_;
    std::unordered_map<std::string, UserRecord> users_;
    std::unordered_map<std::string, ApiKeyRecord> api_keys_;
    std::unordered_map<std::string, RolePermissions> roles_;
    std::unordered_map<std::string, RateLimitState> rate_limits_;
    std::mutex mutex_;  // Thread safety for concurrent access
};
```

### 11.3 Request Processing Pipeline

```
HTTP request
  |
  v
[1. Parse headers] -> Extract Cookie: llamaste_sid=... OR Authorization: Bearer ...
  |
  v
[2. Auth check] -> AuthManager::get_session() or verify_api_key()
  |                 If auth_required and no valid auth: return 401
  |                 If no auth: create anonymous session with "guest" role
  |
  v
[3. Rate limit check] -> AuthManager::check_rate_limit()
  |                       If exceeded: return 429 Too Many Requests
  |
  v
[4. CSRF check] -> For non-GET requests from web UI:
  |                 Compare X-CSRF-Token header with llamaste_csrf cookie
  |                 API requests (Bearer auth) skip CSRF check
  |
  v
[5. Route request] -> /v1/* -> OpenAI-compatible API handler
  |                   /api/* -> Llamaste native API
  |                   /* -> Static web UI files
  |
  v
[6. Agent loop] -> LLM generates response, may call tools
  |
  v
[7. Tool permission check] -> AuthManager::has_permission(role, tool_name)
  |                           If denied: tool returns error message to LLM
  |
  v
[8. Execute tool] -> Actual syscall / file operation / etc.
  |
  v
[9. Stream response] -> SSE stream back to client
```

---

## 12. Key Takeaways

1. **No existing local LLM server has integrated auth + RBAC + session management in a single binary.** Llamaste can be first.

2. **llama.cpp's slot system provides natural session isolation** at the inference level. Each slot has an independent KV cache, and `id_slot` allows pinning users to slots.

3. **Open WebUI's RBAC model is the best reference** for role/permission design. Its Admin/User/Pending three-tier system maps well to Llamaste's Admin/User/Guest.

4. **Phase 1 should ship with no auth by default** (matching Ollama/LM Studio/oobabooga convention), but with session cookies always active for conversation isolation.

5. **Argon2id is the right choice for password hashing**, with the reference C library statically linked. Bcrypt is a viable fallback for extremely constrained systems.

6. **HttpOnly cookies for sessions, Bearer tokens for API** is the clean separation. The web UI never handles raw tokens; the API never uses cookies.

7. **Fair-share scheduling (round-robin across sessions)** prevents any single user from monopolizing inference, which is critical for a shared appliance.

8. **On a 16GB system with a 7B model, 2-4 simultaneous users** is realistic for interactive chat. Beyond that, per-user latency degrades below usable thresholds.

9. **The migration path from no-auth to full auth must preserve conversation history.** Session-ID-keyed storage (not username-keyed) makes this possible.

10. **XSS is the primary web security risk** for a chat application. HTML-encoding all user AND LLM output, plus a strict CSP, are non-negotiable from day one.
