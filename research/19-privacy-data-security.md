# Research 19: Privacy, Data Security & Compliance

**Date**: 2026-02-26
**Scope**: Data-at-rest protection, encryption, TLS, conversation data management, audit log security, PII handling, GDPR/CCPA compliance, network security, and secure deletion for the Llamaste single-binary LLM appliance.

---

## 1. Data Map: What Sensitive Data Exists and Where

Llamaste stores all mutable data on the ext4 DATA partition (`/data`). The squashfs root is read-only and contains no user data. The following map exhaustively catalogues every piece of sensitive data.

### 1.1 Complete Data Map

```
/data/
  config/
    device.json            # Device settings: hostname, boot mode, device password hash,
                           #   JWT server secret, first-boot flag
                           # SENSITIVITY: HIGH (server secret = session forgery if leaked)

    users.json             # User accounts: username, Argon2id password hash, role, created
                           # SENSITIVITY: HIGH (password hashes, user enumeration)

    api-keys.json          # API keys: id, Argon2id hash, prefix, name, role, rate limits
                           # SENSITIVITY: HIGH (API key hashes)

    roles.json             # Role-to-permission mappings
                           # SENSITIVITY: LOW (no secrets, but access control definitions)

    network.json           # Network config: DHCP/static IP, DNS servers, WiFi credentials
                           # SENSITIVITY: MEDIUM (WiFi PSK if stored, network topology)

    tls/
      server.key           # TLS private key (if HTTPS enabled)
      server.crt           # TLS certificate
      ca.crt               # Local CA certificate (if generated)
                           # SENSITIVITY: CRITICAL (private key = impersonation)

  sessions/
    sessions.json          # Active session-to-user mappings, metadata
                           # SENSITIVITY: MEDIUM (session tokens, user associations)

    history/
      <session-id>.jsonl   # Full conversation history per session (append-only JSONL)
                           #   Contains: user messages, LLM responses, tool calls + results
                           # SENSITIVITY: HIGH (may contain PII, passwords, file contents,
                           #   secrets revealed via tool calls like fs.read_file)

  audit/
    audit.log              # Every tool call: timestamp, user, session, tool name,
                           #   parameters, result, duration
                           # SENSITIVITY: CRITICAL (contains full tool parameters and results;
                           #   fs.read_file results contain file contents; may include
                           #   passwords, API keys, personal data from read files)

    audit.log.1            # Rotated audit logs
    audit.log.2
    ...

  models/
    *.gguf                 # Model weight files (3-20 GB each)
                           # SENSITIVITY: LOW (publicly available models, but integrity
                           #   matters -- tampered models could produce harmful output)

    model-manifest.json    # Downloaded model metadata, checksums
                           # SENSITIVITY: LOW

  cache/
    semantic-cache.db      # Cached query-response pairs for common patterns
                           # SENSITIVITY: MEDIUM (contains past query/response pairs)

  user-files/              # Files uploaded or created by users through the chat
                           # SENSITIVITY: VARIABLE (entirely user-dependent)
```

### 1.2 Sensitivity Classification

| Level | Data | Risk if Exposed |
|-------|------|-----------------|
| CRITICAL | TLS private key, audit logs with tool results | Full impersonation; complete disclosure of all system interactions and file contents |
| HIGH | device.json (server secret), users.json, api-keys.json, conversation history | Session forgery, credential exposure, PII leakage |
| MEDIUM | sessions.json, network.json, semantic cache | Session hijacking, network credential exposure |
| LOW | roles.json, model files, model manifest | Access control enumeration, model integrity |

### 1.3 Threat: What Happens If the Device Is Stolen?

Without encryption, an attacker who physically obtains the device can:
- Read all conversation history (including any PII users shared)
- Extract password hashes (offline brute-force attack)
- Extract the JWT server secret (forge sessions)
- Read the TLS private key (MITM any replacement device)
- Read the full audit log (complete record of every system interaction)
- Read any files users stored on the data partition

This is the primary argument for encryption at rest.

---

## 2. Full-Disk Encryption (LUKS/dm-crypt)

### 2.1 How LUKS Works

LUKS (Linux Unified Key Setup) uses dm-crypt to provide transparent block-level encryption. All reads and writes pass through the dm-crypt layer, which encrypts/decrypts using AES-256-XTS (default). The filesystem (ext4) sits on top of the dm-crypt device and is unaware of encryption.

LUKS2 (current standard) supports multiple key slots, Argon2id key derivation, and authenticated encryption modes. The master key is encrypted with a user-supplied passphrase and stored in the LUKS header on disk.

### 2.2 The Headless Unlock Problem

The fundamental challenge for a headless appliance: someone must supply the passphrase at boot before the data partition can be mounted. Options:

| Method | Security | UX | Headless-Friendly |
|--------|----------|-----|-------------------|
| **Console passphrase** | High | Poor | No (requires keyboard/monitor) |
| **SSH early boot (dropbear-initramfs)** | High | Moderate | Yes (SSH from phone/laptop) |
| **TPM auto-unlock (Clevis + Tang)** | Medium-High | Excellent | Yes (fully automatic) |
| **USB key file** | Medium | Good | Yes (insert USB key, boot) |
| **Network key server (Tang)** | Medium-High | Excellent | Yes (auto-unlock on home LAN) |
| **No encryption** | None | Best | Yes |

**TPM auto-unlock**: The TPM chip stores the LUKS passphrase, sealed to the platform's boot measurements (PCRs). If the disk is removed from the machine, the TPM on the original machine will not release the key. Ubuntu 24.04 and Fedora support this with Clevis. Limitations: requires a TPM 2.0 chip (present on most post-2016 PCs); does not protect against attacks where the entire machine is stolen and booted normally.

**Tang network server**: A separate device on the LAN (e.g., a Raspberry Pi) runs a Tang key server. The Llamaste device uses Clevis to auto-unlock LUKS using a key derived from Tang. If the device is taken off the home network, it cannot unlock. Limitation: requires a second always-on device.

**SSH early boot (dropbear-initramfs)**: A tiny SSH server runs in the initramfs before the root filesystem is mounted. The user SSHs in and types the passphrase. Described as "rock solid and supported by Debian stable going back many years." Practical for technically sophisticated users.

**USB key file**: A small USB drive contains the key file. Insert it at boot; remove after boot for physical security. Simple, no network dependency, works on any hardware.

### 2.3 How NAS Appliances Handle Encryption

**TrueNAS**: Uses ZFS native encryption (not LUKS). Keys can be stored in the system database (convenient but insecure if device is stolen) or protected by a user passphrase. Enterprise edition uses KMIP (Key Management Interoperability Protocol) for centralized key management. ZFS encryption is not full-disk -- metadata like file count, filesystem structure, and dataset sizes remain visible.

**Synology**: Uses eCryptFS folder-based encryption. Encrypts individual shared folders, not the full disk. Leaks file counts and sizes. Keys can be stored in a key manager or entered at boot.

**QNAP**: Offers volume-level encryption (full-disk for a volume). Requires passphrase at boot or stored key.

**Key insight**: No major NAS vendor does full-disk LUKS by default. They all use filesystem-level or volume-level encryption with stored keys for headless convenience. The trade-off between security and usability is universal.

### 2.4 Performance Impact of LUKS on Model Loading

Llamaste loads models via mmap, which means the kernel reads model data from disk on demand as pages are accessed. With LUKS, every page fault triggers a dm-crypt decryption operation.

**Benchmark data from various sources:**

| Scenario | Unencrypted | LUKS + AES-NI | Overhead |
|----------|-------------|---------------|----------|
| Sequential read (NVMe SSD) | 3,500 MB/s | 1,500-2,500 MB/s | 30-55% |
| Sequential read (SATA SSD) | 550 MB/s | 450-520 MB/s | 5-18% |
| Sequential read (HDD) | 150 MB/s | 140-148 MB/s | 1-7% |
| Random 4K read (NVMe) | 500K IOPS | 350-450K IOPS | 10-30% |
| Initial model load (7B Q4, NVMe) | ~1.3s | ~2.0-2.5s | +0.7-1.2s |
| Initial model load (7B Q4, SATA) | ~8s | ~9-10s | +1-2s |
| Steady-state inference (mmap cached) | ~0 disk I/O | ~0 disk I/O | 0% |

**Critical insight**: After the model is fully loaded into RAM (page cache), dm-crypt overhead drops to zero because all inference reads come from cached memory pages, not disk. The only real cost is the initial model load at boot, which adds 1-2 seconds on SATA SSD or under 1 second on NVMe.

**Optimization flags for LUKS performance:**
- `--sector-size=4096` during `cryptsetup luksFormat` (up to 2x improvement on NVMe)
- `no_read_workqueue,no_write_workqueue` dm-crypt options (reduces CPU overhead)
- Verify `aesni_intel` kernel module is loaded (hardware AES acceleration)
- Use `aes-xts-plain64` cipher (default, hardware-accelerated on all modern x86)

### 2.5 Selective Encryption vs Full-Disk

**Full DATA partition encryption (LUKS):**
- Pro: Simple, everything protected, no risk of sensitive data leaking to unencrypted areas
- Con: Adds 1-2s to model load; requires unlock at boot; model files are not sensitive

**Selective encryption (encrypt only sensitive directories):**
- Encrypt `/data/config/`, `/data/sessions/`, `/data/audit/` using fscrypt or a LUKS loopback
- Leave `/data/models/` unencrypted (large, not sensitive, performance-critical)
- Pro: Model loads unaffected; only small files encrypted
- Con: More complex; risk of sensitive data landing in unencrypted areas (e.g., semantic cache, swap, tmp)

**Recommendation for Llamaste**: Full DATA partition encryption with LUKS is the simpler and safer approach. The 1-2 second model load overhead is negligible for a device that boots in 30 seconds. Selective encryption adds complexity and creates edge cases where sensitive data could leak to unencrypted areas.

### 2.6 Recommendation: Tiered Encryption Strategy

| Phase | Approach | Unlock Method |
|-------|----------|---------------|
| Phase 1 | **No encryption by default** (matches NAS conventions) | N/A |
| Phase 1 | **Optional LUKS** for security-conscious users | SSH early boot (dropbear) or USB key |
| Phase 2 | **LUKS + TPM auto-unlock** for supported hardware | Automatic (TPM-sealed key) |
| Phase 3 | **LUKS + Tang** for multi-device setups | Automatic (network key server) |

Default to no encryption in Phase 1 (matching Synology, TrueNAS, and most home server appliances). Provide clear documentation for enabling LUKS. The first-boot setup wizard should ask: "Do you want to encrypt the data partition? This protects your data if the device is stolen, but requires a passphrase at boot."

---

## 3. Data in Transit -- TLS

### 3.1 The LAN TLS Problem

Llamaste runs on a LAN, typically at an IP address like `192.168.1.x` or an mDNS hostname like `llamaste.local`. TLS on a LAN is harder than on the public internet because:

1. **No Let's Encrypt for LAN**: Let's Encrypt requires a publicly resolvable domain name. `192.168.1.50` and `llamaste.local` cannot get Let's Encrypt certificates.
2. **Self-signed certificates cause browser warnings**: Every browser shows a scary warning page. Non-technical users may think the device is compromised.
3. **mDNS hostnames and TLS**: Certificates must include the `.local` hostname as a Subject Alternative Name (SAN). This works with self-signed certs but not with public CAs.

### 3.2 How Other LAN Appliances Handle TLS

**Pi-hole v6**: Generates a self-signed certificate at installation. Creates a local CA; users can add the CA cert to their browser to trust it. HTTP and HTTPS both available; HTTPS optional. A community tool called "local-https" simplifies certificate management.

**Home Assistant**: Supports self-signed certificates for local access. Recommends Let's Encrypt via DuckDNS add-on for remote access (requires port forwarding). For local-only use, self-signed is the standard approach. Nabu Casa cloud relay avoids the TLS problem entirely for remote access.

**Synology DSM**: Generates a self-signed certificate by default. Supports Let's Encrypt if the NAS has a public domain. Provides a certificate manager UI for importing custom certificates.

**Key pattern**: All major LAN appliances default to HTTP or self-signed HTTPS, with Let's Encrypt as an optional upgrade for users who have a public domain.

### 3.3 TLS Options for Llamaste

| Option | Browser Trust | Complexity | Requirement |
|--------|--------------|------------|-------------|
| **HTTP only** (default) | N/A | None | Nothing |
| **Self-signed cert** (auto-generated at first boot) | No (browser warning) | Low | openssl in binary |
| **Local CA** (generated at first boot) | Yes (after CA install) | Medium | User installs CA cert |
| **Let's Encrypt** (DNS challenge) | Yes | High | Public domain + DNS API |
| **Reverse proxy** (nginx/Caddy on another machine) | Depends on proxy config | External | Separate machine |

### 3.4 Recommendation: HTTP Default, HTTPS Optional

**Phase 1**: HTTP on port 80 is the default (matching Pi-hole, Ollama, LM Studio convention). No TLS complexity out of the box.

**Phase 1 also**: Generate a self-signed certificate at first boot and serve HTTPS on port 443 simultaneously. Users who want HTTPS can use it immediately. The web UI should offer a one-click download of the CA certificate for browser trust.

**Phase 2**: Add Let's Encrypt support for users with public domains. Add a certificate management page in the web UI settings.

**Phase 3**: TLS between mesh cluster nodes using mutual TLS (mTLS) with auto-generated certificates. Cluster nodes auto-discover via mDNS and establish encrypted channels.

### 3.5 DNS Rebinding Protection

DNS rebinding attacks allow a malicious website to trick the browser into making requests to LAN devices. This is a real threat for any HTTP service on a LAN.

**Mitigations Llamaste should implement:**
1. **Host header validation**: Reject HTTP requests where the `Host` header does not match the device's known hostname or IP. Return 403 for unrecognized hosts.
2. **Origin header checking**: For API requests from browsers, validate the `Origin` header matches the device's address.
3. **Content Security Policy**: Already planned (from research 18). `connect-src 'self'` prevents the browser from making requests to unexpected origins.
4. **Bind to specific interface**: Allow configuration to bind only to a specific network interface (e.g., `eth0` only, not the guest WiFi interface).

Chrome 142+ (2025) includes Local Network Access (LNA) protection that blocks cross-origin requests to private networks, significantly reducing the DNS rebinding attack surface for Chrome users.

---

## 4. Conversation Data Management

### 4.1 What Conversations Contain

Each conversation session is stored as a JSONL file (`/data/sessions/history/<session-id>.jsonl`). A typical conversation record includes:

- **User messages**: May contain PII (names, addresses, phone numbers, account numbers)
- **LLM responses**: May contain PII reflected back from user context
- **Tool calls**: The tool name and full parameters (e.g., `fs.read_file("/data/user-files/tax-return.pdf")`)
- **Tool results**: The full output of the tool (e.g., the entire contents of the file that was read)

**The tool result problem**: When a user says "read my tax return," the conversation history now contains the full contents of that file. This is not just metadata -- it is a verbatim copy of potentially highly sensitive data embedded in the conversation log.

### 4.2 Retention Policies

| Policy | Default | Configurable |
|--------|---------|--------------|
| Conversation retention | 90 days | Yes (1 day to forever) |
| Auto-expiry cleanup | Daily at 3 AM | Yes (cron-style schedule) |
| Maximum storage for conversations | 1 GB | Yes |
| Conversations per user | Unlimited | Yes |

The cleanup process runs as a periodic task inside the llamaste binary:
1. Scan `/data/sessions/history/` for JSONL files
2. Check the last-modified timestamp
3. Delete files older than the retention period
4. If total conversation storage exceeds the maximum, delete oldest first

### 4.3 User-Initiated Deletion

Users must be able to delete their own data. The web UI should provide:

**Delete single conversation**: User selects a conversation from the sidebar and clicks "Delete." The JSONL file is deleted from disk. The in-memory conversation state is cleared. A confirmation dialog warns: "This conversation and all its content will be permanently deleted."

**Delete all conversations**: User goes to Settings > Privacy > "Delete all my conversations." All JSONL files for that user's sessions are deleted. This is a bulk operation with a confirmation step.

**"Forget this conversation"**: A softer option that clears the current conversation from the LLM context (KV cache) without deleting the history file. Useful when the user said something sensitive and wants to ensure the LLM does not reference it going forward, but does not necessarily want to delete the record.

### 4.4 Export and Portability

Users should be able to export their conversations:
- **Export single conversation**: Download as JSON or plain text
- **Export all conversations**: Download as a ZIP of JSONL files
- **Machine-readable format**: JSONL is already machine-readable; no conversion needed

This supports GDPR data portability requirements (Article 20) and migration between Llamaste devices.

### 4.5 Tool Results in Conversations

When a tool call returns sensitive data (e.g., file contents), the full result is stored in the conversation JSONL. Options for handling this:

**Option A: Store everything (current plan)**: Simple, complete record, but conversation files become a liability. If the user deletes the original file, the conversation still contains its contents.

**Option B: Truncate tool results in history**: Store only the first N characters of tool results in the conversation log. Full results are available in the current session but not persisted. Reduces storage and liability.

**Option C: Store tool results separately**: Tool results are stored in a separate file with references in the conversation JSONL. This allows separate retention policies for tool results (shorter) vs conversations (longer).

**Recommendation**: Option B for Phase 1. Truncate tool results to 500 characters in the persisted conversation history, with a note "[truncated, full result was N characters]." The LLM still sees the full result during the active session for reasoning, but the full data is not persisted forever. Users who need full tool results can check the audit log (which has its own retention policy).

---

## 5. Audit Log Security

### 5.1 What the Audit Log Contains

The audit log records every tool execution:

```json
{
  "timestamp": "2026-02-26T14:32:01.003Z",
  "session_id": "a1b2c3d4...",
  "user": "admin",
  "tool": "fs.read_file",
  "params": {"path": "/data/user-files/passwords.txt"},
  "result_size": 1247,
  "result_preview": "admin:hunter2\nroot:abc123\n...",
  "duration_ms": 3,
  "status": "success"
}
```

**The audit log is potentially the most sensitive file on the entire system.** It contains a complete record of every action taken, including file reads with their contents, network configuration changes, and process management operations.

### 5.2 Access Control

- **Read access**: Admin only. Regular users should not be able to view the audit log.
- **Write access**: The llamaste binary only (PID 1). No external process can write to it.
- **Delete access**: Nobody. The audit log should be append-only from the application's perspective. Only the rotation/retention system can remove old log files.

Implementation: Set file permissions to `0600` owned by root (which is the llamaste binary since it runs as PID 1). The web UI audit log viewer is gated behind admin role.

### 5.3 Sensitive Data Redaction in Logs

Storing full tool results in the audit log creates a surveillance record. Options:

**Option A: Full logging (no redaction)**: Complete forensic record. Useful for debugging and security incident investigation. High liability.

**Option B: Redact tool results**: Store tool name, parameters, status, and duration, but not the full result. Store only `result_size` and a flag indicating success/failure.

**Option C: Configurable redaction**: Admin chooses the logging level:
- `full`: Everything logged (for debugging or high-security environments that audit access)
- `standard`: Tool name, parameters, result size, and status logged. Result content omitted.
- `minimal`: Tool name and status only. Parameters redacted except for non-sensitive ones.

**Recommendation**: `standard` as the default. Tool parameters are logged (so you know which file was read), but tool result contents are not. This provides a useful audit trail without creating a liability. The `full` mode is available for administrators who explicitly want complete logging.

### 5.4 Log Rotation and Retention

| Parameter | Default | Configurable |
|-----------|---------|--------------|
| Max log file size | 10 MB | Yes |
| Max rotated files | 10 (100 MB total) | Yes |
| Retention period | 90 days | Yes |
| Rotation trigger | Size-based | Size or time |

Implementation: Ring buffer of log files. When `audit.log` reaches 10 MB, rotate to `audit.log.1`, `audit.log.2`, etc. Delete the oldest when the count exceeds the maximum or age exceeds retention.

### 5.5 Tamper-Evident Logging

For environments that require strong audit guarantees, the audit log should be tamper-evident. This means that any modification to past log entries is detectable.

**Phase 1 implementation (simple hash chain)**:
Each log entry includes a hash of the previous entry:

```json
{
  "seq": 42,
  "prev_hash": "sha256:a1b2c3d4...",
  "timestamp": "2026-02-26T14:32:01.003Z",
  "tool": "fs.read_file",
  ...
  "entry_hash": "sha256:e5f6g7h8..."
}
```

The hash chain means that modifying any entry invalidates all subsequent hashes. A verification tool can walk the chain and detect tampering.

**Limitation**: A sophisticated attacker with disk access can recompute the entire chain. True tamper-evidence against a privileged attacker requires external anchoring (e.g., periodically publishing a checkpoint hash to an external service). This is a Phase 3+ concern.

**Phase 1 recommendation**: Simple hash chain is sufficient. It detects accidental corruption and casual tampering. Combined with LUKS encryption, it provides reasonable security for a home/small business appliance.

---

## 6. PII Handling

### 6.1 The PII Problem

Users will inevitably type personal information into the chat: names, addresses, phone numbers, Social Security numbers, medical information, financial details. The LLM processes this locally (a major privacy advantage over cloud AI), but the data is stored in conversation history and potentially in audit logs.

### 6.2 PII Detection and Redaction

Automated PII detection approaches:

| Approach | Accuracy | Performance | Complexity |
|----------|----------|-------------|------------|
| Regex-only (SSN, email, phone patterns) | ~65% recall | Fast (<1ms) | Low |
| NER model (Named Entity Recognition) | ~85% recall | Moderate (10-50ms) | Medium |
| Hybrid (NER + regex) | ~96% recall | Moderate | Medium-High |
| LLM-based detection | ~98% recall | Slow (100ms+) | High |

**For Phase 1**: Regex-based detection is the pragmatic choice. It catches structured PII (email addresses, phone numbers, SSNs, credit card numbers) with minimal code and zero additional model requirements. It will miss unstructured PII (names in context, addresses in natural language), but that is acceptable for Phase 1.

**For Phase 2**: Consider a small NER model (e.g., a fine-tuned 50MB model) for hybrid detection. This is the approach used by Microsoft Presidio and similar tools.

### 6.3 Where to Apply PII Redaction

- **Conversation history**: Optionally redact PII in stored conversations (user-configurable). Default: store as-is (users chose to type this into a local device).
- **Audit logs**: Redact PII from tool parameters in the audit log when logging level is `standard` or `minimal`. Example: `fs.read_file` parameter path is logged, but if the path contains a person's name (e.g., `/data/john-doe-taxes.pdf`), the filename is kept as-is (it is the parameter, not content).
- **Semantic cache**: Redact PII from cached query patterns. A query "What is John Smith's phone number" should not be cached with the person's name.

### 6.4 Data Minimization

Principles to follow:

1. **Do not store what you do not need**: Tool results in conversation history should be truncated (see section 4.5). The full result is only needed during the active session.
2. **Automatic expiry**: Conversations older than the retention period are deleted automatically.
3. **No analytics or telemetry**: Llamaste does not phone home. No usage data, no crash reports, no model performance metrics are sent anywhere. All processing is local.
4. **KV cache is ephemeral**: The LLM's KV cache (its "working memory") is in RAM only and is lost on reboot. No persistent copy of the inference state exists.
5. **Semantic cache expiry**: Cached query-response pairs expire after a configurable period (default: 7 days).

### 6.5 Right to Deletion

A user must be able to delete ALL their data from the system:

1. Delete all conversations (section 4.3)
2. Delete their user account (if user accounts are enabled)
3. Delete their API keys
4. Request that audit log entries related to their sessions be purged (admin operation)
5. Clear the semantic cache of entries from their sessions

The web UI should provide a "Delete My Account and All Data" button that performs all of these operations. This supports GDPR Article 17 (right to erasure).

---

## 7. GDPR / CCPA / Privacy Regulation

### 7.1 Does GDPR Apply to Llamaste?

**Yes, GDPR can apply to a local-only device** if it processes personal data of EU residents. The GDPR applies to the processing of personal data, regardless of whether it happens in the cloud or on local hardware. The data controller (the person/organization operating the Llamaste device) must comply.

However, the **household exemption** (GDPR Article 2(2)(c)) excludes processing by a natural person in the course of a purely personal or household activity. A home user running Llamaste for personal use is likely exempt. A small business using Llamaste is not exempt.

### 7.2 Data Controller vs Data Processor

| Scenario | Controller | Processor | GDPR Applies? |
|----------|-----------|-----------|---------------|
| Home user, personal use | Home user | N/A | No (household exemption) |
| Home user, side business | Home user | N/A | Yes |
| Small business | The business | N/A | Yes |
| Llamaste distributed as product | User/business is controller | Llamaste project provides the tool (not a processor) | Varies |

**Key distinction**: The Llamaste project itself is not a data processor. It provides software that users install and operate. The user/organization operating the device is the data controller. The Llamaste project's responsibility is to provide tools that enable compliance (deletion, export, encryption), not to ensure compliance itself.

### 7.3 GDPR Requirements Mapped to Llamaste Features

| GDPR Right | Article | Llamaste Feature |
|------------|---------|------------------|
| Right of access | Art. 15 | Export conversations (JSON/ZIP download) |
| Right to rectification | Art. 16 | Edit conversation history (future feature) |
| Right to erasure | Art. 17 | Delete conversation, delete all data, factory reset |
| Right to data portability | Art. 20 | Export in machine-readable format (JSONL) |
| Right to object | Art. 21 | Users control what they type; no automated profiling |
| Data minimization | Art. 5(1)(c) | Truncated tool results, auto-expiry, no telemetry |
| Security of processing | Art. 32 | Encryption (LUKS), access control, audit logging |
| Privacy by design | Art. 25 | Local-first architecture, no cloud dependency |

### 7.4 CCPA Considerations

The California Consumer Privacy Act applies to businesses that meet certain revenue or data volume thresholds. Most Llamaste deployments (home users, small businesses) will fall below these thresholds. However, the same privacy features that support GDPR also support CCPA compliance: right to know (export), right to delete, right to opt-out (no data sale -- Llamaste never shares data with third parties).

### 7.5 Home Assistant's Privacy Model as Reference

Home Assistant, the closest comparable open-source local-first project, takes this approach:
- **Local control and privacy first** is the core philosophy
- All processing occurs on-device; no data is stored in the cloud
- Operated as a non-profit (Open Home Foundation) that cannot be sold
- Fully open-source, allowing complete transparency into data handling
- Optional cloud relay (Nabu Casa) is also open-source so users can verify what data is transmitted

Llamaste should adopt the same philosophy: **local-first, open-source, transparent, user-controlled**.

### 7.6 Privacy Policy Requirements

If Llamaste is distributed as a product (even free/open-source), it should include:
- A privacy policy in the documentation explaining what data is collected and stored
- An in-device privacy notice accessible from the web UI settings
- Clear documentation of data flows (what stays local vs what could leave the device, e.g., model downloads, NTP sync)
- Disclosure that conversation data is stored locally and may contain PII

A template privacy policy should ship with the project.

---

## 8. Network Security

### 8.1 Threat Model for a LAN Appliance

The Llamaste web UI is accessible to anyone on the same LAN. This includes:
- Family members and housemates (trusted)
- Guest WiFi users (untrusted)
- IoT devices on the same network (potentially compromised)
- An attacker who gains access to the LAN (via WiFi, compromised device, physical access)

### 8.2 Threat Model Table

| Threat | Likelihood | Impact | Mitigation |
|--------|-----------|--------|------------|
| **Stolen device** (physical theft) | Low-Medium | Critical (all data exposed) | LUKS encryption, device password |
| **LAN eavesdropping** (ARP spoofing, MITM) | Low | High (credentials, conversations intercepted) | HTTPS/TLS, HSTS |
| **DNS rebinding** (malicious website accesses LAN device) | Medium | High (unauthorized tool execution) | Host header validation, CORS, CSP |
| **Guest network access** (untrusted user on same LAN) | Medium | Medium (unauthorized use of LLM, data access) | Device password, interface binding |
| **Brute force login** | Medium | Medium (account compromise) | Rate limiting, lockout, Argon2id |
| **XSS in chat** (prompt injection producing malicious HTML) | Medium | Medium (session hijacking, data theft) | HTML encoding, CSP, HttpOnly cookies |
| **CSRF** (malicious site triggers tool calls) | Low-Medium | Medium (unauthorized actions) | CSRF tokens, SameSite cookies |
| **Model poisoning** (tampered GGUF file) | Low | High (harmful/biased output) | SHA-256 checksum verification on download |
| **Malicious USB boot media** | Low | Critical (full system compromise) | Secure Boot (Phase 4), signed images |
| **LLM prompt injection via tool results** | Medium | Low-Medium (LLM performs unintended actions) | Tool output sanitization, confirmation gates |

### 8.3 Network Interface Binding

By default, Llamaste binds to `0.0.0.0` (all interfaces). This means it is accessible from any network the device is connected to, including guest WiFi if the device has multiple interfaces.

Configuration option:
```json
{
  "bind_address": "0.0.0.0",       // Default: all interfaces
  "bind_interface": null,            // Optional: bind to specific interface (e.g., "eth0")
  "allowed_networks": ["192.168.1.0/24"]  // Optional: restrict access to specific subnets
}
```

### 8.4 Firewall Rules

Llamaste should configure minimal firewall rules at boot (using raw iptables/nftables syscalls, no userspace tools needed):

```
ACCEPT: tcp dport 80  (HTTP web UI)
ACCEPT: tcp dport 443 (HTTPS, if enabled)
ACCEPT: udp dport 5353 (mDNS for .local discovery)
ACCEPT: udp dport 5354 (mesh cluster discovery, Phase 3)
ACCEPT: tcp dport 8081 (MCP server, Phase 3)
ACCEPT: icmp (ping)
DROP: everything else
```

### 8.5 ARP Spoofing and MITM

On an unencrypted HTTP connection, any device on the LAN can perform ARP spoofing to intercept traffic between the user's browser and the Llamaste device. This exposes:
- Session cookies (session hijacking)
- Conversation content (eavesdropping)
- API keys (if sent in headers)

**Mitigation**: HTTPS. When TLS is enabled, ARP spoofing can intercept encrypted traffic but cannot decrypt it (the attacker would need the server's private key). This is why HTTPS should be available from Phase 1, even if not the default.

---

## 9. Secure Deletion

### 9.1 The Problem with ext4 + SSD

When Llamaste deletes a conversation file with `unlink()`:
- **ext4**: Marks the inode and data blocks as free. The file's data blocks remain on disk until overwritten by new data. On ext4 with journaling, fragments may also exist in the journal.
- **SSD with TRIM**: The kernel sends a TRIM command to the SSD, telling it the blocks are no longer needed. The SSD's garbage collector will eventually erase the physical NAND cells, but the timing is unpredictable (could be milliseconds or hours).
- **SSD without TRIM**: Data blocks remain on the NAND flash indefinitely until the SSD's wear-leveling and garbage collection processes happen to overwrite them.

### 9.2 Why Software Shredding Fails on SSDs

Tools like `shred` do not work reliably on SSDs because:
1. **Wear leveling**: The SSD may write the "overwrite" data to a different physical NAND cell than the original data, leaving the original intact.
2. **ext4 copy-on-write behavior**: ext4 with certain features (e.g., data=journal) may not overwrite data in place.
3. **SSD over-provisioning**: SSDs have more physical storage than their advertised capacity. Data in the over-provisioned area is inaccessible to the OS but potentially recoverable with forensic tools.

### 9.3 Deletion Effectiveness by Storage Type

| Storage | `unlink()` only | `unlink()` + TRIM | `shred` + `unlink()` | Crypto erase |
|---------|-----------------|--------------------|-----------------------|--------------|
| HDD | Data remains | N/A | Effective | Effective |
| SATA SSD | Data remains briefly | Mostly effective | Unreliable | Effective |
| NVMe SSD | Data remains briefly | Mostly effective | Unreliable | Effective |
| USB flash | Data remains | Usually no TRIM support | Partially effective | Effective |
| eMMC | Data remains | Varies | Unreliable | Effective |

### 9.4 Cryptographic Erasure: The Best Approach

The most reliable method for secure deletion on SSDs is **cryptographic erasure**: encrypt data with a unique key, and when you want to "delete" it, destroy the key.

**How this applies to Llamaste:**

If the DATA partition is LUKS-encrypted, then "factory reset" = destroy the LUKS header (which contains the encrypted master key). Without the master key, all data on the partition is cryptographically unrecoverable. This takes milliseconds and works regardless of storage type.

For per-conversation deletion without full-disk encryption, Llamaste could:
1. Encrypt each conversation JSONL file with a unique per-file key
2. Store the key in memory and in a key file on disk
3. To "delete" a conversation: delete the key, then unlink the file
4. Without the key, the file contents are unrecoverable even if the data blocks persist on the SSD

**Phase 1 recommendation**: Use `unlink()` for file deletion (standard POSIX). Document that this is not forensically secure on SSDs. Recommend LUKS for users who need strong deletion guarantees. Implement cryptographic erasure for factory reset if LUKS is enabled.

### 9.5 Factory Reset

"Factory reset" should:
1. Delete all data on `/data/` partition (conversations, config, audit logs, user accounts)
2. Regenerate first-boot state (new server secret, new default API key)
3. Keep model files (optional -- models are large and not sensitive)

Implementation:
- If LUKS: Destroy LUKS header + recreate LUKS + format ext4. Cryptographically secure.
- If no LUKS: `rm -rf /data/*` + recreate directory structure. Not forensically secure on SSD, but sufficient for most use cases. Optionally, issue `blkdiscard` on the partition to trigger SSD TRIM on all blocks.

The web UI should provide a "Factory Reset" option in Settings > System > Advanced, protected by a confirmation dialog and the admin password.

---

## 10. Recommendations for Llamaste

### 10.1 Privacy-by-Design Principles

1. **Local-first**: All inference and data processing happens on the device. No cloud dependency. No telemetry.
2. **Data minimization**: Do not store what you do not need. Truncate tool results in conversation logs. Auto-expire old conversations.
3. **Transparency**: Users can see exactly what data exists (privacy dashboard), export it, and delete it.
4. **User control**: Users decide their privacy/convenience trade-off (encryption, retention, logging level).
5. **Secure defaults**: Authentication required for destructive operations. Audit logging enabled. Sensible retention periods.
6. **Open source**: All code is auditable. No hidden data collection. Privacy claims are verifiable.

### 10.2 Phase 1: Minimal Viable Privacy

Implementation priority for Phase 1:

| # | Feature | Effort | Impact |
|---|---------|--------|--------|
| 1 | Conversation auto-expiry (configurable retention) | 1 day | High -- limits liability |
| 2 | User-initiated conversation deletion (single + bulk) | 1 day | High -- GDPR right to erasure |
| 3 | Conversation export (JSONL download) | 0.5 day | Medium -- data portability |
| 4 | Truncate tool results in stored conversations | 0.5 day | High -- data minimization |
| 5 | Audit log with configurable redaction levels | 1 day | High -- security + privacy balance |
| 6 | Audit log rotation (size + age based) | 0.5 day | Medium -- storage management |
| 7 | Host header validation (DNS rebinding protection) | 0.5 day | Medium -- network security |
| 8 | Self-signed TLS certificate generation at first boot | 1 day | Medium -- data in transit |
| 9 | Privacy dashboard in web UI (what data exists, storage used) | 1 day | Medium -- transparency |
| 10 | Factory reset via web UI | 0.5 day | Medium -- device lifecycle |
| 11 | Simple hash-chain audit log integrity | 0.5 day | Low -- tamper evidence |
| 12 | Regex-based PII detection in audit log redaction | 1 day | Low -- automated privacy |
| | **Total** | **~9 days** | |

### 10.3 Default Settings: Secure by Default

```json
{
  "privacy": {
    "conversation_retention_days": 90,
    "auto_expire_enabled": true,
    "tool_result_max_stored_chars": 500,
    "audit_log_level": "standard",
    "audit_log_max_size_mb": 10,
    "audit_log_max_files": 10,
    "audit_log_retention_days": 90,
    "semantic_cache_ttl_days": 7,
    "pii_redaction_in_audit": true,
    "telemetry_enabled": false,
    "analytics_enabled": false
  },
  "security": {
    "tls_enabled": false,
    "tls_auto_generate_cert": true,
    "encryption_at_rest": false,
    "host_header_validation": true,
    "firewall_enabled": true,
    "model_checksum_verification": true
  }
}
```

### 10.4 Privacy Dashboard in Web UI

The web UI should include a "Privacy & Data" page accessible from Settings:

```
Privacy & Data
==============

Storage Overview:
  Conversations:  47 sessions, 12.3 MB
  Audit logs:     8.7 MB (3 files)
  Semantic cache: 0.4 MB (231 entries)
  Configuration:  0.1 MB
  Models:         12.4 GB (2 models)
  Total user data: 21.5 MB (excluding models)

Retention Settings:
  [Conversations expire after] [90] days
  [Audit logs expire after] [90] days
  [Semantic cache expires after] [7] days

Actions:
  [Export all my conversations]  (ZIP download)
  [Delete all my conversations]  (requires confirmation)
  [Clear semantic cache]
  [View audit log]               (admin only)
  [Factory reset]                (admin only, requires confirmation)

Encryption:
  Data partition: [Not encrypted]
  TLS: [Self-signed certificate active]
    [Download CA certificate for browser trust]
```

### 10.5 Configuration for Different Threat Models

**Home user** (default -- "I trust everyone on my WiFi"):
```json
{
  "auth_required": false,
  "encryption_at_rest": false,
  "tls_enabled": false,
  "conversation_retention_days": 90,
  "audit_log_level": "standard"
}
```

**Privacy-conscious home user** ("I want reasonable privacy"):
```json
{
  "auth_required": true,
  "encryption_at_rest": true,
  "tls_enabled": true,
  "conversation_retention_days": 30,
  "audit_log_level": "minimal",
  "tool_result_max_stored_chars": 100
}
```

**Small business** ("We handle customer data"):
```json
{
  "auth_required": true,
  "encryption_at_rest": true,
  "tls_enabled": true,
  "conversation_retention_days": 365,
  "audit_log_level": "full",
  "audit_log_retention_days": 365,
  "pii_redaction_in_audit": true,
  "bind_interface": "eth0",
  "allowed_networks": ["10.0.1.0/24"]
}
```

### 10.6 What NOT to Implement in Phase 1

- Full NER-based PII detection (too complex, requires additional model)
- Per-conversation encryption keys (adds complexity with minimal benefit if LUKS is available)
- External audit log anchoring (Phase 3+ for enterprise)
- Homomorphic encryption or confidential computing (research-grade, not practical)
- LUKS TPM auto-unlock (requires Clevis integration, Phase 2)
- Let's Encrypt integration (requires public domain, Phase 2)
- GDPR compliance documentation generator (nice-to-have, Phase 2)

---

## 11. Summary: The Llamaste Privacy Advantage

Llamaste's core privacy value proposition compared to cloud AI services:

| Aspect | Cloud AI (ChatGPT, etc.) | Llamaste |
|--------|--------------------------|----------|
| Where inference happens | Provider's servers | Your device |
| Who sees your prompts | Provider + subprocessors | Nobody (local only) |
| Data retention | Provider-controlled | User-controlled |
| Data deletion | Trust provider's claim | Verifiable (open source, local disk) |
| Network dependency | Required | None (works offline) |
| Telemetry | Extensive | None |
| Model training on your data | Possible (opt-out varies) | Impossible (local model) |
| Regulatory compliance | Provider's responsibility | User's responsibility (tools provided) |
| Physical security | Provider's data center | User's responsibility |

The honest pitch: **"Your conversations never leave your device. You control your data -- how long it is kept, who can see it, and when it is deleted. The code is open source so you can verify these claims."**

The honest caveat: **"Physical security is your responsibility. If someone steals the device and it is not encrypted, they can read your data. Enable encryption if this concerns you."**

---

## 12. Key Takeaways

1. **The audit log is the most sensitive file on the system**, more so than conversation history, because it contains a complete record of every tool execution including file contents. Default to redacted (`standard`) logging.

2. **LUKS encryption is practical for Llamaste** with minimal performance impact (1-2s added to model load). It should be optional in Phase 1, recommended for business use.

3. **Secure deletion on SSDs is fundamentally unreliable without encryption.** The only reliable approach is cryptographic erasure (LUKS + destroy key). Document this honestly.

4. **HTTP is an acceptable default for a LAN appliance** (matching Pi-hole, Ollama, Home Assistant, Synology conventions). Generate self-signed TLS cert at first boot for users who want HTTPS.

5. **Conversation tool results are a data liability.** Truncating stored tool results to 500 characters dramatically reduces the risk of the conversation log becoming a copy of the user's entire filesystem.

6. **GDPR household exemption covers most home users**, but small businesses need the compliance tools. Provide export, deletion, and retention controls that enable GDPR compliance.

7. **DNS rebinding is the most underappreciated network threat** for LAN appliances. Host header validation is a simple, effective mitigation that should be in Phase 1.

8. **Follow Home Assistant's model**: local-first, open-source, transparent, user-controlled. This is the proven approach for privacy-focused home appliances.

9. **Privacy is Llamaste's competitive advantage** over cloud AI. Every design decision should reinforce the message: your data stays on your device, under your control.

10. **Phase 1 privacy features (~9 days of work)** provide a solid foundation: conversation management, audit log security, data minimization, basic TLS, and a privacy dashboard. This is sufficient for launch and can be incrementally improved.
