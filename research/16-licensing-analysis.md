# Llamaste Licensing Analysis

## Research Document 16 — Comprehensive Licensing Implications
**Date:** 2026-02-26
**Scope:** Static binary, bootable image, model distribution, firmware, patent landscape

---

## 1. Static Linking and GPL: The Critical Questions

### 1.1 libgcc and libstdc++ — The GCC Runtime Library Exception

The GCC Runtime Library Exception (version 3.1) explicitly permits statically linking
libgcc and libstdc++ into non-GPL programs without triggering copyleft. The key mechanism:

**"Eligible Compilation Process"**: The exception applies when compilation is done using
GCC alone or with other GPL-compatible software. Since Llamaste will be compiled with
GCC (or a GCC-compatible compiler like Clang), this requirement is satisfied.

**What the exception grants**: Permission to propagate a work of Target Code formed by
combining the Runtime Library with Independent Modules, even if such propagation would
otherwise violate GPLv3. The combined work may be conveyed under terms of your choice.

**Static vs. dynamic linking**: The exception makes no distinction between static and
dynamic linking. The same permissions apply regardless of linking method.

**Critical caveat**: If you distribute libstdc++ or libgcc as independent, standalone
libraries (not linked into your binary), you must follow GPL terms for those standalone
copies. In Llamaste's case, they are linked into the binary, so the exception applies.

**VERDICT**: Statically linking libgcc/libstdc++ into the Llamaste binary is fully
permitted. No GPL obligations propagate to our code through this path.

### 1.2 musl libc — No Issue

musl is MIT-licensed. Static linking is explicitly permitted. The only obligation is to
include the MIT copyright notice and license text in distribution. This is the simplest
component from a licensing perspective.

### 1.3 Kernel Headers and Syscalls — Not a Derivative Work

The Linux kernel COPYING file contains an explicit statement from Linus Torvalds:

> "This copyright does not cover user programs that use kernel services by normal
> system calls - this is merely considered normal use of the kernel, and does not
> fall under the heading of 'derived work.'"

The UAPI (User-space API) headers are tagged with the SPDX identifier
`GPL-2.0 WITH Linux-syscall-note`, formally documenting that including these headers
and making syscalls does not extend GPL obligations to user-space programs.

**VERDICT**: The Llamaste binary, which runs as a user-space process (PID 1) making
standard syscalls, is definitively NOT a derivative work of the Linux kernel. This is
the most well-established legal boundary in Linux licensing.

### 1.4 Summary: Static Binary License Interaction

| Component         | License                    | Linked Into Binary | GPL Propagation? |
|-------------------|----------------------------|--------------------|------------------|
| Our code          | TBD (see Section 5)       | Yes (core)         | N/A              |
| llama.cpp / ggml  | MIT                        | Yes                | No               |
| httplib           | MIT                        | Yes                | No               |
| musl libc         | MIT                        | Yes (static)       | No               |
| libgcc            | GPL-3.0 + RLE 3.1         | Yes (static)       | No (exception)   |
| libstdc++         | GPL-3.0 + RLE 3.1         | Yes (static)       | No (exception)   |
| Kernel headers    | GPL-2.0 + syscall-note    | No (compile-time)  | No (exception)   |

**Conclusion**: The Llamaste binary can use ANY license for our own code. No component
forces GPL propagation into our binary.

---

## 2. Distribution Obligations for a Bootable Linux Image

### 2.1 What the Image Contains

The Llamaste bootable image includes:
- ESP partition: GRUB2 bootloader, kernel (vmlinuz), initramfs
- Root partition: squashfs with the llamaste binary
- Data partition: ext4 with model files, user data
- Firmware blobs (if included for hardware support)

### 2.2 GPL-Licensed Components and Their Obligations

**Linux Kernel (GPL-2.0-only)**:
- Must include a copy of the GPL-2.0 license text
- Must provide or offer complete corresponding source code
- Must include kernel .config file used for the build
- Must include any patches applied to the kernel
- Source offer must be valid for at least 3 years after last distribution

**GRUB2 (GPL-3.0-or-later)**:
- Must include GPL-3.0 license text
- Must provide or offer corresponding source code
- GPL-3.0 adds anti-tivoization clause (relevant if using Secure Boot signing)

**squashfs-tools (GPL-2.0)**:
- Only the build tool is GPL; the squashfs filesystem format itself has no license
- If squashfs-tools binaries are NOT included in the image (they are build-time only),
  no distribution obligation applies for squashfs-tools itself

### 2.3 Buildroot License Compliance

Buildroot provides `make legal-info` which generates:
1. `legal-info/manifest.csv` — Package names, versions, licenses
2. `legal-info/licenses/` — Collected license texts for all packages
3. `legal-info/sources/` — Source tarballs for all packages
4. `legal-info/host-manifest.csv` — Host tool information
5. A README summarizing what was collected and any warnings

**Important**: Buildroot's legal-info output is declarative and best-effort. The project
explicitly warns that it may not be fully accurate and recommends legal review.

**Recommendation**: Run `make legal-info` as part of every release build. Archive the
output alongside the image. Use CycloneDX Buildroot to generate an SBOM.

### 2.4 What to Include in the Distribution

**On the image itself** (data partition `/licenses/` directory):
- `COPYING` — GPL-2.0 text (for the kernel)
- `COPYING.v3` — GPL-3.0 text (for GRUB2)
- `LICENSE-llamaste` — Our project's license
- `LICENSE-llama-cpp` — MIT license for llama.cpp/ggml
- `LICENSE-musl` — MIT license for musl
- `LICENSE-models` — Apache 2.0 or model-specific license
- `NOTICE` — Combined attribution notice listing all components
- `THIRD-PARTY-NOTICES` — Full text of all third-party licenses
- `SOURCE-OFFER.txt` — Written offer for GPL source code

**Accompanying the download** (website or alongside the image):
- Source tarballs for kernel, GRUB2, and any other GPL components
- Build scripts and configuration files (.config, defconfig)
- Buildroot configuration used to produce the image
- Complete `legal-info/` output from Buildroot

**Written Source Offer template**:
```
This product contains copyrighted software licensed under the GNU General
Public License (GPL). A copy of that license is included in the /licenses/
directory. You may obtain the complete Corresponding Source code from us for
a period of three years after our last shipment of this product by writing
to [address] or downloading from [URL].

GPL-licensed components: Linux kernel [version], GRUB2 [version]
```

### 2.5 Lessons from Similar Projects

**Llamafile** (Mozilla): Apache 2.0 for the project, MIT for llama.cpp changes.
Designed for compatibility and upstreamability. Does not distribute a kernel or
bootloader, so avoids GPL image distribution questions entirely.

**Ollama**: MIT-licensed. Was publicly called out for failing to include llama.cpp's
MIT copyright notice in binary distributions (GitHub issue #3185, 210+ upvotes).
This is a cautionary tale: even MIT's minimal requirements must be followed.

**OpenWrt**: GPL-licensed firmware images. Includes license files on the image in
`/usr/share/licenses/`. Source code available via their build system and website.

**Alpine Linux**: Minimal Linux distribution. Ships license information per-package
in `/usr/share/doc/[package]/`. Uses APK package manager to track licenses.

---

## 3. Model File Licensing

### 3.1 Qwen2.5-Instruct (Primary Target)

**License varies by model size**:

| Model Size | License      | Commercial Use | Redistribution |
|------------|-------------|----------------|----------------|
| 0.5B       | Apache 2.0  | Unrestricted   | Include license + note changes |
| 1.5B       | Apache 2.0  | Unrestricted   | Include license + note changes |
| 3B         | Qwen License| Restricted     | "Built with Qwen" attribution |
| 7B         | Apache 2.0  | Unrestricted   | Include license + note changes |
| 14B        | Apache 2.0  | Unrestricted   | Include license + note changes |
| 32B        | Apache 2.0  | Unrestricted   | Include license + note changes |
| 72B        | Qwen License| Restricted     | "Built with Qwen" attribution |

**Recommendation**: Ship only Apache 2.0 models (0.5B, 1.5B, 7B, 14B, 32B) with the
image. The 3B and 72B carry the custom Tongyi Qianwen license with additional
obligations. Qwen3 models are all Apache 2.0.

**Apache 2.0 redistribution requirements for models**:
- Include a copy of the Apache 2.0 license
- State any changes made (quantization counts as a modification)
- Preserve any NOTICE file from the original distribution
- Include attribution to original authors

### 3.2 Does Quantization Affect the License?

Quantization (converting from float16/bfloat16 to GGUF Q4/Q5/Q8 etc.) is a
transformation of the model weights. Under copyright law, it is analogous to
format conversion or compression. The resulting quantized model is a derivative
work of the original model.

**For Apache 2.0 models**: Quantized versions inherit Apache 2.0. You must note
that the model has been quantized (documenting the change). Official Qwen GGUF
files on Hugging Face confirm this interpretation — they carry the same license.

**For Qwen License models**: Quantized versions inherit the Qwen License.

**For all models**: The GGUF container format itself (defined by llama.cpp) is MIT-
licensed and imposes no additional restrictions.

### 3.3 Other Model Families — License Comparison

| Model Family      | License               | Commercial | Key Restrictions                    |
|-------------------|-----------------------|------------|-------------------------------------|
| Qwen2.5 (most)   | Apache 2.0            | Yes        | Standard Apache 2.0 terms           |
| Qwen2.5 3B/72B   | Qwen License          | Yes*       | Attribution, usage restrictions      |
| Qwen3 (all)      | Apache 2.0            | Yes        | Standard Apache 2.0 terms           |
| Llama 3/3.1/4    | Llama Community Lic.  | Yes*       | <700M MAU; "Built with Llama"       |
| DeepSeek R1/V3   | MIT                   | Yes        | Most permissive; no restrictions     |
| Mistral/Mixtral   | Apache 2.0 (open)     | Yes        | Standard Apache 2.0 terms           |
| Gemma 2/3        | Gemma Terms of Use    | Yes*       | Usage policy; license propagation    |
| Phi-3/4          | MIT                   | Yes        | Most permissive; no restrictions     |

*Conditional commercial use with specific requirements.

**Gemma warning**: The Gemma Terms of Use grant Google the right to "restrict
(remotely or otherwise) usage" and require downstream users to be bound by the
same restrictions. This is NOT open source by OSI standards. Redistributing Gemma
in a bootable image would require including the full Gemma Terms of Use and
ensuring users agree to them — adding significant legal complexity.

**Llama warning**: The Llama Community License includes a 700M monthly active user
threshold and a "Built with Llama" branding requirement. Derivatives inherit these
restrictions. Feasible for Llamaste but adds friction.

**Recommendation for user-downloadable models**: Document the license for each
model family clearly. When users download models through Llamaste's interface,
display the license terms and require acknowledgment before download.

---

## 4. Firmware Blob Licensing

### 4.1 The linux-firmware Package

The linux-firmware package is a collection of binary firmware files with MIXED
licenses. There are three categories:

1. **Freely redistributable**: Most firmware files include a license that permits
   redistribution in binary form with or without modification. Common examples:
   - Intel WiFi firmware (redistributable with license notice)
   - AMD GPU microcode (redistributable)
   - Realtek firmware (redistributable)

2. **Redistributable with restrictions**: Some firmware has conditions like
   "redistribution in binary form only" or "no modification permitted."

3. **Non-redistributable or unclear**: A small number of firmware files have no
   explicit license or prohibit redistribution. The Debian kernel team maintains
   a detailed tracking page of these.

### 4.2 Practical Approach for Llamaste

**Option A — Minimal firmware (recommended)**:
Include only firmware for the target hardware profiles (e.g., common Intel/AMD
WiFi, Ethernet, basic GPU). Review each firmware file's license individually.
Most Intel and AMD firmware is freely redistributable with a license notice.

**Option B — Full linux-firmware package**:
Include the entire package. This maximizes hardware compatibility but requires
auditing every firmware license. Some files may not be redistributable.

**Option C — No firmware (linux-libre approach)**:
Ship no proprietary firmware. Maximum license purity but severely limits
hardware compatibility. Only viable for specific known-good hardware.

**Recommendation**: Option A. Create a curated firmware set. Include a
`FIRMWARE-LICENSES` directory with the license text for each included firmware
blob. Document which firmware is included and why.

### 4.3 Regulatory Concerns

WiFi firmware in particular has FCC regulatory implications. Firmware for radio
transmitters must be approved via testing at an authorized lab. Distributing
modified WiFi firmware could violate FCC regulations regardless of the software
license. This is another reason to ship firmware blobs as-is without modification.

---

## 5. What License Should Llamaste's Own Code Use?

### 5.1 License Options Analysis

**MIT License**
- Pros: Maximum adoption; matches llama.cpp/ggml (simplifies integration);
  simplest to understand; no patent clause complexity; corporate-friendly
- Cons: No copyleft protection (anyone can close-source forks); no patent
  grant; no protection against proprietary cloud services using our code
- Community adoption: Highest (~44% of GitHub projects)

**Apache 2.0**
- Pros: Permissive with explicit patent grant; matches Qwen model license;
  corporate-friendly with patent protection; well-understood
- Cons: No copyleft; slightly more complex than MIT; patent retaliation clause
  may concern some contributors
- Community adoption: High (corporate preference)

**GPL-2.0-only**
- Pros: Matches Linux kernel license; strong copyleft ensures modifications
  stay open; well-tested legally
- Cons: Discourages corporate adoption; incompatible with Apache 2.0 (one-way
  only); incompatible with GPL-3.0 components in the same binary
- Community adoption: Moderate (declining for new projects)

**GPL-3.0-or-later**
- Pros: Matches GRUB2; stronger copyleft with anti-tivoization; patent grant;
  compatible with Apache 2.0 (one-way: Apache code can go into GPL-3.0)
- Cons: Incompatible with GPL-2.0-only (kernel); discourages corporate adoption;
  anti-tivoization may conflict with Secure Boot requirements
- Community adoption: Moderate

**AGPL-3.0-or-later**
- Pros: Closes the "network use" loophole (relevant since Llamaste serves over
  HTTP); strongest copyleft; all GPL-3.0 pros plus network clause
- Cons: Strongly discourages corporate adoption; many companies have blanket
  AGPL bans; overkill for a local-first appliance
- Community adoption: Low (niche, primarily databases and server software)

### 5.2 License Compatibility Matrix

Can code under license X be combined with code under license Y in a single binary?

| Our License  | MIT | Apache 2.0 | GPL-2.0 | GPL-3.0 | AGPL-3.0 | GCC RLE |
|-------------|-----|-----------|---------|---------|----------|---------|
| MIT          | OK  | OK        | OK      | OK      | OK       | OK      |
| Apache 2.0   | OK  | OK        | NO(1)   | OK(2)   | OK(2)    | OK      |
| GPL-2.0      | OK  | NO(1)     | OK      | NO(3)   | NO(3)    | OK      |
| GPL-3.0      | OK  | OK(2)     | NO(3)   | OK      | OK       | OK      |
| AGPL-3.0     | OK  | OK(2)     | NO(3)   | OK      | OK       | OK      |

Notes:
(1) Apache 2.0 and GPL-2.0-only are incompatible due to Apache's additional
    restrictions (patent retaliation clause).
(2) Apache 2.0 code can be incorporated into GPL-3.0/AGPL-3.0 works (one-way).
(3) GPL-2.0-only and GPL-3.0 are incompatible. GPL-2.0-or-later can upgrade.

### 5.3 Recommendation: Apache 2.0

**Recommended license for Llamaste's own code: Apache License 2.0**

Justification:

1. **Ecosystem alignment**: llama.cpp (MIT) and Qwen models (Apache 2.0) are the
   two most integrated components. Apache 2.0 is compatible with both. MIT code
   can be included in an Apache 2.0 project without issue.

2. **Patent protection**: Apache 2.0 includes an explicit patent grant. Given the
   patent landscape around transformers (see Section 6), this provides meaningful
   protection for contributors and users.

3. **Community adoption**: Permissive licenses maximize adoption. Llamaste's goal
   is to make local LLMs accessible — a restrictive license would undermine this.

4. **Corporate friendliness**: Small businesses, hardware vendors, and educational
   institutions can use Llamaste without legal concern.

5. **No kernel conflict**: Our binary does NOT link to the kernel. It communicates
   via syscalls. There is no license interaction with the kernel's GPL-2.0.

6. **GRUB2 is separate**: GRUB2 (GPL-3.0) is a separate binary in the ESP. It is
   not linked into our binary. No license conflict.

7. **Not AGPL**: While Llamaste serves over HTTP, it is primarily a local-first
   appliance. The HTTP server serves the local user's own web UI. AGPL would add
   unnecessary friction without meaningful benefit — users who want to modify
   Llamaste can do so, and the network clause would mainly confuse downstream
   users of a self-hosted appliance.

8. **Precedent**: Llamafile (the most similar project) uses Apache 2.0 for exactly
   these reasons.

### 5.4 License File Structure

```
/licenses/
    LICENSE                     -- Apache 2.0 (Llamaste project)
    NOTICE                      -- Attribution notice for all components
    COPYING                     -- GPL-2.0 (Linux kernel)
    COPYING.v3                  -- GPL-3.0 (GRUB2)
    third-party/
        llama-cpp-LICENSE       -- MIT (llama.cpp / ggml)
        musl-LICENSE            -- MIT (musl libc)
        httplib-LICENSE         -- MIT (cpp-httplib)
        gcc-runtime-exception   -- GCC RLE 3.1 text
    models/
        Apache-2.0              -- For Qwen2.5 Apache models
        [model-specific]        -- For other downloaded models
    firmware/
        [per-blob licenses]     -- Individual firmware licenses
    SOURCE-OFFER.txt            -- Written offer for GPL source code
```

---

## 6. Patent Concerns

### 6.1 Transformer Architecture — Google's Patents

Google holds US10,452,978B2 ("Attention-Based Sequence Transduction Neural Networks"),
which covers the self-attention mechanism central to all transformer models. Key points:

- **Scope**: The patent covers the encoder-decoder transformer with multi-head
  self-attention. There is debate about whether decoder-only models (like most
  modern LLMs) fall within the claims.

- **Apache 2.0 patent grant from Google**: Users of Google's official code
  (TensorFlow, JAX) receive an implicit patent license under Apache 2.0. This
  does NOT extend to independent implementations like llama.cpp.

- **Non-enforcement stance**: Google has not enforced this patent against any
  open-source or commercial LLM project. Analysts describe this as "strategic
  timing, not generosity" — Google benefits from ecosystem growth.

- **Risk assessment for Llamaste**: LOW. Llamaste uses llama.cpp for inference,
  not training. We do not implement the transformer architecture ourselves; we
  run pre-trained models. The patent covers the method of training/using the
  architecture, but enforcement against inference engines using third-party models
  would be unprecedented and strategically counterproductive for Google.

### 6.2 Quantization Methods

**GPTQ**: Published by IST-DASLab (academic). Open-source reference implementation.
No known patents on the GPTQ algorithm itself.

**AWQ**: Published by MIT-HAN Lab (academic). Open-source implementation. Won MLSys
2024 Best Paper. No known patents on the AWQ algorithm.

**GGUF format**: Defined by llama.cpp project (MIT-licensed). A container format, not
a quantization algorithm per se. No patent concerns.

**k-quant methods** (used in llama.cpp's Q4_K_M, Q5_K_S, etc.): Implemented within
llama.cpp. Community-developed. No known patents.

**Risk assessment**: LOW. All quantization methods used by llama.cpp are either
academic publications with open-source implementations or community-developed. No
specific patents have been identified targeting these methods for LLM weight
quantization. (Note: older SIMD quantization patents exist in the video compression
domain but are unrelated.)

### 6.3 SIMD Instructions

SIMD instruction sets (SSE, AVX, AVX-512, NEON) are implemented in hardware by Intel,
AMD, and ARM. Using these instructions in software does not incur patent royalties —
the patent licenses are embedded in the hardware purchase.

**Risk assessment**: NONE. Using SIMD instructions for computation is universally
accepted and carries no patent encumbrance for software authors.

### 6.4 Overall Patent Risk Assessment

| Area                    | Risk Level | Notes                                    |
|-------------------------|-----------|------------------------------------------|
| Transformer architecture| Low       | Google holds patents, has not enforced    |
| Self-attention mechanism| Low       | Broad claims, unclear decoder-only scope |
| Quantization (GPTQ/AWQ) | Low       | Academic origin, open-source, no patents |
| GGUF format             | None      | MIT-licensed community standard          |
| SIMD usage              | None      | Hardware-licensed                        |
| Model weights           | None      | Weights are data, not patentable methods |

---

## 7. Commercial Use Considerations

### 7.1 Can a Small Business Use Llamaste Commercially?

**Yes**, with the following considerations:

**No restrictions from**:
- Linux kernel (GPL applies to kernel, not user-space programs)
- llama.cpp / ggml (MIT — unrestricted commercial use)
- musl libc (MIT — unrestricted commercial use)
- Our code (Apache 2.0 — unrestricted commercial use, recommended)

**Minor obligations**:
- Include license texts and NOTICE file
- Provide GPL source for kernel and GRUB2 (or a written offer)
- For models: comply with the specific model's license

**Model-specific commercial restrictions**:

| Model               | Commercial Use | Restriction for Business              |
|---------------------|---------------|---------------------------------------|
| Qwen2.5 (Apache)    | Unrestricted  | None                                  |
| Qwen3 (all Apache)  | Unrestricted  | None                                  |
| DeepSeek R1/V3      | Unrestricted  | None (MIT)                            |
| Mistral (Apache)    | Unrestricted  | None                                  |
| Phi-3/4             | Unrestricted  | None (MIT)                            |
| Llama 3/4           | Conditional   | <700M MAU; "Built with Llama" branding|
| Gemma 2/3           | Conditional   | Must propagate usage restrictions     |
| Qwen2.5 3B/72B      | Conditional   | Qwen License terms apply              |

### 7.2 Recommended "Safe" Model Stack for Commercial Use

For maximum commercial flexibility with zero license friction:
1. **Primary**: Qwen2.5 7B/14B/32B (Apache 2.0) or Qwen3 (Apache 2.0)
2. **Alternative**: DeepSeek R1/V3 distilled models (MIT)
3. **Alternative**: Mistral/Mixtral open models (Apache 2.0)
4. **Avoid shipping**: Gemma (complex terms), Llama (branding requirement)

Users who download Gemma or Llama models through Llamaste's interface are accepting
those licenses individually — the Llamaste project itself bears no additional burden
beyond clearly communicating the license terms.

---

## 8. Practical Compliance Checklist for Llamaste

### 8.1 Build-Time Compliance

- [ ] Run `make legal-info` with every Buildroot release build
- [ ] Archive source tarballs for all GPL-licensed components
- [ ] Save kernel .config and build scripts alongside sources
- [ ] Generate SBOM using CycloneDX Buildroot
- [ ] Audit firmware blob licenses before inclusion
- [ ] Verify all MIT/Apache copyright notices are preserved in the binary

### 8.2 Image Contents

- [ ] Include `/licenses/` directory with all license texts
- [ ] Include `NOTICE` file with component attributions
- [ ] Include `SOURCE-OFFER.txt` with written source offer
- [ ] Include model-specific license files alongside model weights
- [ ] Include `THIRD-PARTY-NOTICES` with full license texts

### 8.3 Distribution (Website / Downloads)

- [ ] Host GPL source tarballs alongside image downloads
- [ ] Maintain source availability for 3 years after last distribution
- [ ] Display license information on download page
- [ ] Provide Buildroot configuration for reproducible builds
- [ ] Link to upstream project pages for all components

### 8.4 Runtime (In the Web UI)

- [ ] Display "About" page with license information
- [ ] Show model license before user downloads a new model
- [ ] Require license acknowledgment for non-Apache/MIT models
- [ ] Provide access to license files through the web UI

### 8.5 Binary Attribution (Lesson from Ollama)

The Ollama controversy demonstrates that even MIT's minimal attribution requirement
must be honored in binary distributions. The Llamaste binary should:

- [ ] Embed copyright notices in a `--version` or `--licenses` command-line flag
- [ ] Include a `/licenses` endpoint in the HTTP server
- [ ] Embed the NOTICE text as a string constant in the binary

---

## 9. Summary of Recommendations

1. **License Llamaste's own code under Apache 2.0**. This provides patent protection,
   aligns with the llama.cpp and Qwen ecosystem, maximizes community adoption, and
   is corporate-friendly.

2. **Ship only Apache 2.0 / MIT models** with the image. Let users download other
   models (Llama, Gemma) through the UI with license display and acknowledgment.

3. **Curate firmware blobs** rather than including the entire linux-firmware package.
   Audit each blob's license. Include per-blob license files.

4. **Create a comprehensive `/licenses/` directory** on the image with all license
   texts, a NOTICE file, and a written source offer for GPL components.

5. **Host GPL source code** alongside image downloads on the project website.
   Maintain availability for 3 years minimum.

6. **Embed attribution in the binary** via `--licenses` flag and HTTP endpoint
   to avoid the Ollama pitfall.

7. **Document the license landscape** for users who want to use Llamaste
   commercially, making it clear which models have restrictions.

8. **Patent risk is low** but should be monitored. Google's transformer patents
   are the only notable concern, and enforcement against inference engines using
   third-party models is considered unlikely.

---

## 10. Sources

### GCC Runtime Library Exception
- [GCC Runtime Library Exception FAQ](https://www.gnu.org/licenses/gcc-exception-3.1-faq.en.html)
- [GCC Runtime Library Exception 3.1 Text](https://www.gnu.org/licenses/gcc-exception-3.1.en.html)
- [libstdc++ License](https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html)
- [GPL Linking Exception (Wikipedia)](https://en.wikipedia.org/wiki/GPL_linking_exception)

### Linux Kernel Licensing and Syscall Boundary
- [Linux Kernel Licensing Rules](https://docs.kernel.org/process/license-rules.html)
- [SFLC: Linux Kernel and CDDL](https://softwarefreedom.org/resources/2016/linux-kernel-cddl.html)
- [HN: Linux syscall exception discussion](https://news.ycombinator.com/item?id=37320678)
- [Embedded Linux and Copyright Law (Barr Group)](https://barrgroup.com/blog/embedded-linux-and-copyright-law)

### Buildroot License Compliance
- [Buildroot Manual: Legal Notice](https://buildroot.org/downloads/manual/legal-notice.txt)
- [Buildroot Manual (full)](https://buildroot.org/downloads/manual/manual.html)
- [Buildroot Legal Info Scripts](https://github.com/buildroot/buildroot/tree/master/support/legal-info)
- [CycloneDX Buildroot SBOM](https://github.com/CycloneDX/cyclonedx-buildroot)

### GPL Compliance for Distribution
- [Practical GPL Compliance (Linux Foundation)](https://project.linuxfoundation.org/hubfs/Reports/Practical_GPL_Compliance_Digital.pdf)
- [GPL Compliance Guide (SFLC)](https://softwarefreedom.org/resources/2008/compliance-guide.html)
- [How to Comply with GPL v2 (Landley)](http://www.landley.net/kdocs/pending/gplv2-howto.html)

### Model Licensing
- [Qwen2.5 LICENSE (Apache 2.0)](https://huggingface.co/Qwen/Qwen2.5-7B/blob/main/LICENSE)
- [Qwen License Discussion (72B)](https://huggingface.co/Qwen/Qwen2.5-72B-Instruct/discussions/18)
- [Gemma Terms of Use](https://ai.google.dev/gemma/terms)
- [Llama 4 Community License](https://www.llama.com/llama4/license/)

### Firmware Blob Licensing
- [Debian Kernel Firmware Licensing](https://wiki.debian.org/KernelFirmwareLicensing)
- [Linux Firmware (Gentoo Wiki)](https://wiki.gentoo.org/wiki/Linux_firmware)
- [Binary Blobs in Linux (LWN)](https://lwn.net/Articles/130696/)

### Similar Project Licensing
- [Llamafile (GitHub)](https://github.com/mozilla-ai/llamafile)
- [Ollama License Issue #3185](https://github.com/ollama/ollama/issues/3185)
- [llama.cpp License Discussion](https://github.com/ggml-org/llama.cpp/discussions/6394)
- [Ollama License Violation (HN)](https://news.ycombinator.com/item?id=44003741)

### Patent Landscape
- [Google Transformer Patent US10452978B2](https://patents.google.com/patent/US10452978B2/en)
- [Google Transformer Patent Risk Analysis (PI IP LAW)](https://piip.co.kr/en/blog/google-transformer-llm-patent-risk-and-strategy)
- [Universal Transformers Patent US10740433B2](https://patents.google.com/patent/US10740433B2/en)

### License Comparison
- [Open Source License Comparison (Mend)](https://www.mend.io/blog/open-source-licenses-comparison-guide/)
- [AGPL-3.0 and Elastic's Journey](https://pureinsights.com/blog/2024/elastics-journey-from-apache-2-0-to-agpl-3/)
- [Open Source Licenses Explained (IT Media Law)](https://itmedialaw.com/en/open-source-licenses-gpl-agpl-mit-and-apache/)
