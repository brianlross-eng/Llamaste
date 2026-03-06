# TTS Upgrade Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace Flite 8kHz robotic TTS with sherpa-onnx + Piper VITS for natural 16-22kHz speech.

**Architecture:** sherpa-onnx (Apache-2.0) provides a C API wrapping onnxruntime + espeak-ng for Piper VITS neural TTS. Built as a shared library in Buildroot. voice.cpp swaps synthesis backend; everything else (ALSA playback, HTTP endpoints, web UI) stays the same.

**Tech Stack:** sherpa-onnx (C API), onnxruntime (CPU), Piper VITS models, Buildroot cmake-package

**Design doc:** `docs/plans/2026-03-05-tts-upgrade-design.md`

---

### Task 1: Create sherpa-onnx Buildroot package — Config.in

**Files:**
- Create: `br2-external/package/sherpa-onnx/Config.in`

**Step 1: Create the Config.in**

```
config BR2_PACKAGE_SHERPA_ONNX
	bool "sherpa-onnx"
	depends on BR2_INSTALL_LIBSTDCPP
	depends on BR2_TOOLCHAIN_HAS_THREADS
	help
	  sherpa-onnx — Speech-to-text, text-to-speech, and more using
	  next-gen Kaldi with onnxruntime. Provides C API for offline TTS
	  using Piper VITS models.

	  https://github.com/k2-fsa/sherpa-onnx
```

**Step 2: Register package in external.mk**

Check if `br2-external/external.mk` includes sherpa-onnx. If there's a top-level `Config.in` or `external.mk` that auto-includes packages, no action needed (Buildroot's br2-external scans `package/*/Config.in` automatically).

**Step 3: Commit**

```bash
git add br2-external/package/sherpa-onnx/Config.in
git commit -m "build: sherpa-onnx Buildroot package skeleton (Config.in)"
```

---

### Task 2: Create sherpa-onnx Buildroot package — sherpa-onnx.mk

**Files:**
- Create: `br2-external/package/sherpa-onnx/sherpa-onnx.mk`

**Step 1: Write the package makefile**

Model this on `br2-external/package/whisper-cpp/whisper-cpp.mk`. Key differences: shared libs, TTS-only config, FetchContent for onnxruntime.

```makefile
################################################################################
#
# sherpa-onnx — offline TTS (Piper VITS) via onnxruntime C API
#
################################################################################

SHERPA_ONNX_VERSION = v1.11.3
SHERPA_ONNX_SITE = $(call github,k2-fsa,sherpa-onnx,$(SHERPA_ONNX_VERSION))
SHERPA_ONNX_LICENSE = Apache-2.0
SHERPA_ONNX_LICENSE_FILES = LICENSE
SHERPA_ONNX_INSTALL_STAGING = YES
SHERPA_ONNX_INSTALL_TARGET = YES

# sherpa-onnx uses FetchContent to download onnxruntime pre-built libs
# and espeak-ng source. For musl, we need onnxruntime built from source.
# SHERPA_ONNX_ENABLE_BINARY=OFF avoids building CLI tools we don't need.
SHERPA_ONNX_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-DSHERPA_ONNX_ENABLE_TTS=ON \
	-DSHERPA_ONNX_ENABLE_C_API=ON \
	-DSHERPA_ONNX_ENABLE_BINARY=OFF \
	-DSHERPA_ONNX_ENABLE_PYTHON=OFF \
	-DSHERPA_ONNX_ENABLE_TESTS=OFF \
	-DSHERPA_ONNX_ENABLE_CHECK=OFF \
	-DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
	-DSHERPA_ONNX_ENABLE_JNI=OFF \
	-DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF \
	-DSHERPA_ONNX_ENABLE_GPU=OFF \
	-DSHERPA_ONNX_ENABLE_SPEAKER_DIARIZATION=OFF \
	-DFETCHCONTENT_QUIET=OFF

# Install shared libs + C API header to staging for llamaste linking
define SHERPA_ONNX_INSTALL_STAGING_CMDS
	# C API header
	$(INSTALL) -D -m 0644 $(@D)/sherpa-onnx/c-api/c-api.h \
		$(STAGING_DIR)/usr/include/sherpa-onnx/c-api/c-api.h
	# Find and install all shared libraries
	find $(@D)/buildroot-build/lib -name "*.so*" -exec \
		cp -a {} $(STAGING_DIR)/usr/lib/ \;
endef

# Install shared libs to target rootfs
define SHERPA_ONNX_INSTALL_TARGET_CMDS
	find $(@D)/buildroot-build/lib -name "*.so*" -exec \
		cp -a {} $(TARGET_DIR)/usr/lib/ \;
endef

$(eval $(cmake-package))
```

**Important notes:**
- Version `v1.11.3` is a stable release. Adjust if a newer version has better musl support.
- The pre-built onnxruntime downloaded by FetchContent is glibc-linked. If the build fails on musl, see Task 3 for the fix.
- `INSTALL_STAGING = YES` is needed so llamaste's cmake can find the headers and libs.
- The library paths may be `buildroot-build/lib/` or `buildroot-build/_deps/` — adjust in install commands after first build attempt.

**Step 2: Create hash file**

```bash
# After downloading, compute hash. For now create placeholder:
# br2-external/package/sherpa-onnx/sherpa-onnx.hash
```

Leave hash file empty initially — Buildroot will compute it on first download. After successful download, run:
```bash
sha256sum dl/sherpa-onnx-v1.11.3.tar.gz
```
Then populate `sherpa-onnx.hash` with the result.

**Step 3: Commit**

```bash
git add br2-external/package/sherpa-onnx/sherpa-onnx.mk
git commit -m "build: sherpa-onnx Buildroot package makefile"
```

---

### Task 3: Handle onnxruntime musl compatibility

**Context:** sherpa-onnx's FetchContent downloads pre-built onnxruntime linked against glibc. These won't work on musl. This task handles the fix.

**Step 1: Attempt the default build first**

```bash
cd /root/llamaste-build/output && make sherpa-onnx
```

If it succeeds (unlikely on musl), skip to Task 4.

**Step 2: If glibc symbols fail — build onnxruntime from source**

The fix is to set `SHERPA_ONNX_BUILD_ONNXRUNTIME_FROM_SOURCE=ON` or provide pre-installed onnxruntime. Check sherpa-onnx cmake for the flag:

```bash
grep -r "BUILD_ONNXRUNTIME\|FROM_SOURCE\|ONNXRUNTIME" /root/llamaste-build/output/build/sherpa-onnx-*/CMakeLists.txt
```

**Option A:** If sherpa-onnx supports building onnxruntime from source (look for cmake option), add it to `SHERPA_ONNX_CONF_OPTS`:
```makefile
SHERPA_ONNX_CONF_OPTS += -DSHERPA_ONNX_BUILD_ONNXRUNTIME_FROM_SOURCE=ON
```

**Option B:** If no from-source flag exists, create a separate onnxruntime Buildroot package:

Create `br2-external/package/onnxruntime/Config.in`:
```
config BR2_PACKAGE_ONNXRUNTIME
	bool "onnxruntime"
	depends on BR2_INSTALL_LIBSTDCPP
	help
	  ONNX Runtime - cross-platform inference engine.
	  https://github.com/microsoft/onnxruntime
```

Create `br2-external/package/onnxruntime/onnxruntime.mk`:
```makefile
ONNXRUNTIME_VERSION = v1.22.0
ONNXRUNTIME_SITE = $(call github,microsoft,onnxruntime,$(ONNXRUNTIME_VERSION))
ONNXRUNTIME_LICENSE = MIT
ONNXRUNTIME_INSTALL_STAGING = YES

ONNXRUNTIME_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DBUILD_SHARED_LIBS=ON \
	-Donnxruntime_BUILD_UNIT_TESTS=OFF \
	-Donnxruntime_BUILD_SHARED_LIB=ON \
	-Donnxruntime_ENABLE_PYTHON=OFF \
	-Donnxruntime_USE_CUDA=OFF \
	-Donnxruntime_USE_TENSORRT=OFF \
	-Donnxruntime_MINIMAL_BUILD=ON

$(eval $(cmake-package))
```

Then point sherpa-onnx at it:
```makefile
SHERPA_ONNX_DEPENDENCIES += onnxruntime
SHERPA_ONNX_CONF_OPTS += \
	-DSHERPA_ONNXRUNTIME_INCLUDE_DIR=$(STAGING_DIR)/usr/include/onnxruntime \
	-DSHERPA_ONNXRUNTIME_LIB_DIR=$(STAGING_DIR)/usr/lib
```

**Option C:** If onnxruntime is too complex to build from source (it's notoriously difficult), consider switching Buildroot to glibc. Change in defconfig:
```
# BR2_TOOLCHAIN_BUILDROOT_MUSL is not set
BR2_TOOLCHAIN_BUILDROOT_GLIBC=y
```
This allows pre-built onnxruntime to work. Rootfs increases ~3MB. Full rebuild required.

**Step 3: Verify the build succeeds**

```bash
cd /root/llamaste-build/output && make sherpa-onnx-dirclean && make sherpa-onnx
```

Check for `libsherpa-onnx-c-api.so` in the build output:
```bash
find /root/llamaste-build/output/build/sherpa-onnx-*/ -name "*.so" | head -20
```

**Step 4: Commit whatever fix was needed**

```bash
git add br2-external/package/sherpa-onnx/ br2-external/package/onnxruntime/ br2-external/configs/
git commit -m "build: fix sherpa-onnx onnxruntime musl compatibility"
```

---

### Task 4: Update llamaste Buildroot integration

**Files:**
- Modify: `br2-external/package/llamaste/Config.in`
- Modify: `br2-external/package/llamaste/llamaste.mk`
- Modify: `br2-external/configs/llamaste_x86_64_defconfig`

**Step 1: Add sherpa-onnx select to Config.in**

In `br2-external/package/llamaste/Config.in`, add `select BR2_PACKAGE_SHERPA_ONNX` after the existing selects:

```
config BR2_PACKAGE_LLAMASTE
	bool "llamaste"
	select BR2_PACKAGE_HOST_CMAKE
	select BR2_PACKAGE_ALSA_LIB
	select BR2_PACKAGE_WHISPER_CPP
	select BR2_PACKAGE_FLITE
	select BR2_PACKAGE_SHERPA_ONNX
	help
	  Llamaste LLM-OS. Single static C++ binary that runs as PID 1.
	  Combines llama-server + agent + system tools + web UI.

	  https://github.com/user/llamaste
```

**Step 2: Add dependency to llamaste.mk**

In `br2-external/package/llamaste/llamaste.mk`, add `sherpa-onnx` to `LLAMASTE_DEPENDENCIES`:

```makefile
LLAMASTE_DEPENDENCIES = libcurl openssl whisper-cpp alsa-lib sherpa-onnx
```

**Step 3: Add to defconfig**

In `br2-external/configs/llamaste_x86_64_defconfig`, add after the `BR2_PACKAGE_FLITE=y` line:

```
BR2_PACKAGE_SHERPA_ONNX=y
```

**Step 4: Commit**

```bash
git add br2-external/package/llamaste/Config.in br2-external/package/llamaste/llamaste.mk br2-external/configs/llamaste_x86_64_defconfig
git commit -m "build: add sherpa-onnx dependency to llamaste package"
```

---

### Task 5: Update CMakeLists.txt for sherpa-onnx

**Files:**
- Modify: `src/llamaste/CMakeLists.txt` (after line 154, after the Flite section)

**Step 1: Add sherpa-onnx detection block**

Add after the Flite section (after line 154):

```cmake
# sherpa-onnx for neural TTS (Piper VITS models)
find_library(SHERPA_ONNX_LIB NAMES sherpa-onnx-c-api)
find_path(SHERPA_ONNX_INCLUDE NAMES sherpa-onnx/c-api/c-api.h)
if(SHERPA_ONNX_LIB AND SHERPA_ONNX_INCLUDE)
    message(STATUS "Found sherpa-onnx: ${SHERPA_ONNX_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_SHERPA_ONNX)
    target_include_directories(llamaste PRIVATE ${SHERPA_ONNX_INCLUDE})
    target_link_libraries(llamaste PRIVATE ${SHERPA_ONNX_LIB})
else()
    message(STATUS "sherpa-onnx not found — neural TTS disabled, using flite fallback")
endif()
```

**Step 2: Verify cmake still works on host (Windows/WSL2)**

```bash
cd /mnt/d/Llamaste/src/llamaste && mkdir -p /tmp/cmake-test && cd /tmp/cmake-test && cmake /mnt/d/Llamaste/src/llamaste 2>&1 | grep -E "sherpa|flite|whisper"
```

Expected: "sherpa-onnx not found" on host (correct, it's only available in cross-compile).

**Step 3: Commit**

```bash
git add src/llamaste/CMakeLists.txt
git commit -m "build: add sherpa-onnx detection to CMakeLists.txt"
```

---

### Task 6: Update voice.h — add sherpa-onnx config fields

**Files:**
- Modify: `src/llamaste/voice.h:28-39` (VoiceConfig struct)

**Step 1: Update VoiceConfig**

Replace the legacy piper_binary/piper_model fields with sherpa-onnx model paths:

```cpp
struct VoiceConfig {
    std::string wake_phrase = "llamaste";   // Case-insensitive match in transcription
    std::string whisper_model = "/data/models/ggml-tiny.en-q5_1.bin";
    // sherpa-onnx TTS model paths
    std::string tts_model = "/data/models/tts/en_US-amy-low.onnx";
    std::string tts_tokens = "/data/models/tts/tokens.txt";
    std::string tts_data_dir = "/data/models/tts/espeak-ng-data";
    std::string alsa_device = "default";    // ALSA capture device
    int sample_rate = 16000;                // 16kHz for whisper
    float vad_threshold = 0.5f;             // Speech probability threshold
    int silence_ms = 300;                   // Silence duration to end recording
    int max_record_ms = 30000;              // Max single recording (30s)
    bool enabled = true;
};
```

**Step 2: Commit**

```bash
git add src/llamaste/voice.h
git commit -m "feat: update VoiceConfig for sherpa-onnx TTS model paths"
```

---

### Task 7: Update voice.cpp — sherpa-onnx include and Impl struct

**Files:**
- Modify: `src/llamaste/voice.cpp:19-33` (includes section)
- Modify: `src/llamaste/voice.cpp:161-181` (Impl struct)

**Step 1: Add sherpa-onnx include**

After the `#ifdef HAVE_FLITE` block (line 33), add:

```cpp
#ifdef HAVE_SHERPA_ONNX
#include <sherpa-onnx/c-api/c-api.h>
#endif
```

**Step 2: Add sherpa-onnx fields to Impl struct**

In `struct VoicePipeline::Impl` (after line 171), add:

```cpp
#ifdef HAVE_SHERPA_ONNX
    const SherpaOnnxOfflineTts* sherpa_tts = nullptr;
    bool sherpa_initialized = false;
    std::string tts_engine_name;  // "sherpa-onnx" or "flite"
#endif
```

Also add a default tts_engine_name for tracking which engine is active. Move it outside the ifdef so it's always available:

Replace the Impl struct with:

```cpp
struct VoicePipeline::Impl {
#ifdef HAVE_WHISPER
    struct whisper_context* whisper_ctx = nullptr;
#endif
#ifdef HAVE_ALSA
    snd_pcm_t* capture_handle = nullptr;
#endif
#ifdef HAVE_FLITE
    cst_voice* flite_voice = nullptr;
    bool flite_initialized = false;
#endif
#ifdef HAVE_SHERPA_ONNX
    const SherpaOnnxOfflineTts* sherpa_tts = nullptr;
    bool sherpa_initialized = false;
#endif
    std::string tts_engine_name = "none";  // "sherpa-onnx", "flite", or "none"
    std::thread voice_thread;
    std::atomic<bool> running{false};

    // Audio buffer for accumulating speech segments
    std::vector<float> speech_buffer;

    // Energy VAD state
    int silence_frames = 0;   // consecutive silent frames
    bool in_speech = false;    // currently detecting speech
};
```

**Step 3: Commit**

```bash
git add src/llamaste/voice.cpp
git commit -m "feat: add sherpa-onnx state to VoicePipeline::Impl"
```

---

### Task 8: Update voice.cpp — init() with sherpa-onnx + RAM gating + fallback

**Files:**
- Modify: `src/llamaste/voice.cpp:237-249` (TTS init section in init())

**Step 1: Replace the Flite init block**

Replace lines 237-249 (the `// Initialize Flite TTS` block) with the full init logic:

```cpp
    // ---------------------------------------------------------------
    // Initialize TTS engine: prefer sherpa-onnx on >= 3GB RAM, else Flite
    // ---------------------------------------------------------------
    bool tts_initialized = false;

#ifdef HAVE_SHERPA_ONNX
    {
        // RAM gate: neural TTS needs ~150MB, only load on 3GB+ systems
        int ram_mb = 0;
#ifndef _WIN32
        std::ifstream meminfo("/proc/meminfo");
        std::string mline;
        while (std::getline(meminfo, mline)) {
            if (mline.find("MemTotal") == 0) {
                size_t colon = mline.find(':');
                if (colon != std::string::npos)
                    ram_mb = std::atoi(mline.c_str() + colon + 1) / 1024;
                break;
            }
        }
#endif
        bool has_model = false;
#ifndef _WIN32
        has_model = (access(config_.tts_model.c_str(), R_OK) == 0);
#endif
        if (ram_mb >= 3072 && has_model) {
            SherpaOnnxOfflineTtsConfig tts_config;
            memset(&tts_config, 0, sizeof(tts_config));
            tts_config.model.vits.model = config_.tts_model.c_str();
            tts_config.model.vits.tokens = config_.tts_tokens.c_str();
            tts_config.model.vits.data_dir = config_.tts_data_dir.c_str();
            tts_config.model.vits.length_scale = 1.0f;
            tts_config.model.vits.noise_scale = 0.667f;
            tts_config.model.vits.noise_scale_w = 0.8f;
            tts_config.model.num_threads = 2;
            tts_config.model.provider = "cpu";
            tts_config.max_num_sentences = 1;

            fprintf(stderr, "[voice] Loading sherpa-onnx TTS: %s\n",
                    config_.tts_model.c_str());

            impl_->sherpa_tts = SherpaOnnxCreateOfflineTts(&tts_config);
            if (impl_->sherpa_tts) {
                impl_->sherpa_initialized = true;
                impl_->tts_engine_name = "sherpa-onnx";
                tts_initialized = true;
                fprintf(stderr, "[voice] sherpa-onnx TTS initialized (Piper VITS)\n");
            } else {
                fprintf(stderr, "[voice] sherpa-onnx TTS init failed, falling back\n");
            }
        } else if (!has_model) {
            fprintf(stderr, "[voice] sherpa-onnx: model not found at %s\n",
                    config_.tts_model.c_str());
        } else {
            fprintf(stderr, "[voice] sherpa-onnx: skipping (RAM %dMB < 3072MB)\n",
                    ram_mb);
        }
    }
#endif

    // Fallback to Flite
#ifdef HAVE_FLITE
    if (!tts_initialized && !impl_->flite_initialized) {
        flite_init();
        impl_->flite_voice = register_cmu_us_kal(nullptr);
        if (impl_->flite_voice) {
            impl_->flite_initialized = true;
            impl_->tts_engine_name = "flite";
            tts_initialized = true;
            fprintf(stderr, "[voice] Flite TTS initialized (cmu_us_kal fallback)\n");
        } else {
            fprintf(stderr, "[voice] Flite TTS: failed to register voice\n");
        }
    }
#endif

    if (!tts_initialized) {
        fprintf(stderr, "[voice] No TTS engine available\n");
    }
```

**Step 2: Commit**

```bash
git add src/llamaste/voice.cpp
git commit -m "feat: sherpa-onnx TTS init with RAM gating and Flite fallback"
```

---

### Task 9: Update voice.cpp — speak() with sherpa-onnx synthesis

**Files:**
- Modify: `src/llamaste/voice.cpp:304-344` (speak() method)

**Step 1: Replace the speak() method**

Replace the entire speak() method with dual-engine support:

```cpp
std::vector<int16_t> VoicePipeline::speak(const std::string& text, int* out_sample_rate) {
    if (text.empty()) return {};

    state_.store(VoiceState::SPEAKING);
    fprintf(stderr, "[voice] TTS: synthesizing %zu chars via %s\n",
            text.size(), impl_->tts_engine_name.c_str());

#ifdef HAVE_SHERPA_ONNX
    if (impl_->sherpa_initialized && impl_->sherpa_tts) {
        const SherpaOnnxGeneratedAudio* audio =
            SherpaOnnxOfflineTtsGenerate(impl_->sherpa_tts, text.c_str(),
                                         /*sid=*/0, /*speed=*/1.0f);
        if (audio && audio->n > 0) {
            int sample_rate = audio->sample_rate;
            // Convert float32 (-1.0..1.0) to int16
            std::vector<int16_t> pcm(audio->n);
            for (int32_t i = 0; i < audio->n; i++) {
                float s = audio->samples[i];
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                pcm[i] = static_cast<int16_t>(s * 32767.0f);
            }
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);

            fprintf(stderr, "[voice] TTS: %zu samples @ %dHz (%.1fs)\n",
                    pcm.size(), sample_rate,
                    (float)pcm.size() / sample_rate);

            if (out_sample_rate) *out_sample_rate = sample_rate;

#ifdef HAVE_ALSA
            play_audio(pcm.data(), pcm.size(), sample_rate);
#endif
            state_.store(VoiceState::LISTENING);
            return pcm;
        }
        if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
        fprintf(stderr, "[voice] TTS: sherpa-onnx generate returned empty\n");
    }
#endif

#ifdef HAVE_FLITE
    if (impl_->flite_voice) {
        cst_wave* wave = flite_text_to_wave(text.c_str(), impl_->flite_voice);
        if (wave) {
            int num_samples = wave->num_samples;
            int sample_rate = wave->sample_rate;
            std::vector<int16_t> pcm(wave->samples, wave->samples + num_samples);
            delete_wave(wave);

            fprintf(stderr, "[voice] TTS: %d samples @ %dHz (%.1fs)\n",
                    num_samples, sample_rate,
                    (float)num_samples / sample_rate);

            if (out_sample_rate) *out_sample_rate = sample_rate;

#ifdef HAVE_ALSA
            play_audio(pcm.data(), pcm.size(), sample_rate);
#endif
            state_.store(VoiceState::LISTENING);
            return pcm;
        }
        fprintf(stderr, "[voice] TTS: flite_text_to_wave failed\n");
    }
#endif

    state_.store(VoiceState::LISTENING);
    return {};
}
```

**Step 2: Commit**

```bash
git add src/llamaste/voice.cpp
git commit -m "feat: speak() with sherpa-onnx synthesis + Flite fallback"
```

---

### Task 10: Update voice.cpp — destructor cleanup

**Files:**
- Modify: `src/llamaste/voice.cpp:185-200` (destructor)

**Step 1: Add sherpa-onnx cleanup**

After the `#ifdef HAVE_WHISPER` cleanup block (line 192) and before `delete impl_`, add:

```cpp
#ifdef HAVE_SHERPA_ONNX
    if (impl_->sherpa_tts) {
        SherpaOnnxDestroyOfflineTts(impl_->sherpa_tts);
        impl_->sherpa_tts = nullptr;
    }
#endif
```

**Step 2: Commit**

```bash
git add src/llamaste/voice.cpp
git commit -m "feat: sherpa-onnx cleanup in VoicePipeline destructor"
```

---

### Task 11: Update tools_audio.cpp — engine reporting

**Files:**
- Modify: `src/llamaste/tools_audio.cpp:48-54` (tts_available/tts_engine in handle_audio_status)
- Modify: `src/llamaste/tools_audio.cpp:122` (error message in handle_audio_speak)
- Modify: `src/llamaste/tools_audio.cpp:135` (sample rate in handle_audio_speak)

**Step 1: Fix engine reporting in handle_audio_status**

Replace lines 48-54 (the `#ifdef HAVE_FLITE` block) with dynamic engine detection:

```cpp
    // Report TTS engine from pipeline state
    if (g_voice) {
        // Access the engine name — expose via a new public method
        result["tts_engine"] = g_voice->tts_engine_name();
        result["tts_available"] = (g_voice->tts_engine_name() != "none");
    } else {
        result["tts_available"] = false;
        result["tts_engine"] = "none";
    }
```

**Step 2: Add tts_engine_name() method to VoicePipeline**

In `src/llamaste/voice.h`, add after line 57 (`std::string last_error() const;`):

```cpp
    std::string tts_engine_name() const;
```

In `src/llamaste/voice.cpp`, add after the `last_error()` implementation (after line 508):

```cpp
std::string VoicePipeline::tts_engine_name() const {
    return impl_ ? impl_->tts_engine_name : "none";
}
```

**Step 3: Fix error message in handle_audio_speak**

Replace line 122:
```cpp
        return R"json({"error": "TTS synthesis failed — flite not available"})json";
```
with:
```cpp
        return R"json({"error": "TTS synthesis failed — no TTS engine available"})json";
```

**Step 4: Fix hardcoded sample rate in handle_audio_speak**

Replace line 135 (`uint32_t sample_rate = 16000;`) with dynamic rate:

```cpp
            int tts_rate = 16000;
            auto pcm = g_voice->speak(text, &tts_rate);
```

And update the WAV encoding to use `tts_rate`:
```cpp
            uint32_t sample_rate = (uint32_t)tts_rate;
```

Also fix the duration calculation at line 164:
```cpp
    result["duration_seconds"] = (double)pcm.size() / (double)tts_rate;
```

The full replacement for `handle_audio_speak` (lines 105-166):

```cpp
static std::string handle_audio_speak(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        return R"json({"error": "Invalid JSON arguments"})json";
    }

    std::string text = args.value("text", "");
    if (text.empty()) {
        return R"json({"error": "text parameter required (non-empty)"})json";
    }

    if (!g_voice) {
        return R"json({"error": "Voice pipeline not initialized"})json";
    }

    int tts_rate = 16000;
    auto pcm = g_voice->speak(text, &tts_rate);
    if (pcm.empty()) {
        return R"json({"error": "TTS synthesis failed — no TTS engine available"})json";
    }

    // Write WAV to /data/tmp/
    std::string out_path = "/data/tmp/tts_" + std::to_string(time(nullptr)) + ".wav";
#ifndef _WIN32
    {
        std::ofstream ofs(out_path, std::ios::binary);
        if (ofs) {
            uint32_t data_size = (uint32_t)(pcm.size() * 2);
            uint32_t file_size = data_size + 36;
            uint16_t channels = 1;
            uint32_t sample_rate = (uint32_t)tts_rate;
            uint16_t bps = 16;
            uint32_t byte_rate = sample_rate * channels * bps / 8;
            uint16_t block_align = channels * bps / 8;

            ofs.write("RIFF", 4);
            ofs.write(reinterpret_cast<const char*>(&file_size), 4);
            ofs.write("WAVE", 4);
            ofs.write("fmt ", 4);
            uint32_t fmt_size = 16;
            ofs.write(reinterpret_cast<const char*>(&fmt_size), 4);
            uint16_t audio_fmt = 1;
            ofs.write(reinterpret_cast<const char*>(&audio_fmt), 2);
            ofs.write(reinterpret_cast<const char*>(&channels), 2);
            ofs.write(reinterpret_cast<const char*>(&sample_rate), 4);
            ofs.write(reinterpret_cast<const char*>(&byte_rate), 4);
            ofs.write(reinterpret_cast<const char*>(&block_align), 2);
            ofs.write(reinterpret_cast<const char*>(&bps), 2);
            ofs.write("data", 4);
            ofs.write(reinterpret_cast<const char*>(&data_size), 4);
            ofs.write(reinterpret_cast<const char*>(pcm.data()), data_size);
        }
    }
#endif

    json result;
    result["audio_file"] = out_path;
    result["text"] = text;
    result["samples"] = (int)pcm.size();
    result["sample_rate"] = tts_rate;
    result["duration_seconds"] = (double)pcm.size() / (double)tts_rate;
    result["tts_engine"] = g_voice->tts_engine_name();
    return result.dump();
}
```

**Step 5: Update audio.config to show TTS model info**

In `handle_audio_config`, replace line 188 (`result["piper_voice"] = "en_US-amy-low";`) with:

```cpp
        if (g_voice) {
            result["tts_engine"] = g_voice->tts_engine_name();
            auto cfg2 = g_voice->config();
            result["tts_model"] = cfg2.tts_model;
        } else {
            result["tts_engine"] = "none";
            result["tts_model"] = "";
        }
```

**Step 6: Commit**

```bash
git add src/llamaste/voice.h src/llamaste/voice.cpp src/llamaste/tools_audio.cpp
git commit -m "feat: dynamic TTS engine reporting and sample rate in audio tools"
```

---

### Task 12: Bundle voice model + espeak-ng-data

**Files:**
- Modify: `br2-external/board/llamaste/post_build.sh` (or create model install hook)

**Step 1: Download model files**

On the build machine:
```bash
mkdir -p /root/llamaste-build/tts-models
cd /root/llamaste-build/tts-models

# Download Piper voice model (amy, low quality, 16kHz, ~40MB)
wget https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/vits-piper-en_US-amy-low.tar.bz2
tar xjf vits-piper-en_US-amy-low.tar.bz2

# The extracted directory contains:
# en_US-amy-low.onnx, tokens.txt, espeak-ng-data/
ls -la vits-piper-en_US-amy-low/
```

**Step 2: Add model install to post_build.sh**

Add to `br2-external/board/llamaste/post_build.sh`:

```bash
# Install TTS voice model (sherpa-onnx Piper VITS)
TTS_MODEL_DIR="/root/llamaste-build/tts-models/vits-piper-en_US-amy-low"
if [ -d "$TTS_MODEL_DIR" ]; then
    mkdir -p "${TARGET_DIR}/data/models/tts"
    cp "$TTS_MODEL_DIR/en_US-amy-low.onnx" "${TARGET_DIR}/data/models/tts/"
    cp "$TTS_MODEL_DIR/tokens.txt" "${TARGET_DIR}/data/models/tts/"
    # Copy espeak-ng-data (trim to English-only for size)
    mkdir -p "${TARGET_DIR}/data/models/tts/espeak-ng-data"
    cp -r "$TTS_MODEL_DIR/espeak-ng-data/"* "${TARGET_DIR}/data/models/tts/espeak-ng-data/"
    echo "[post_build] Installed TTS model: en_US-amy-low"
fi
```

**Note:** The model goes into `/data/models/tts/` which is on the DATA partition (ext4, persistent). For the initial image, we bake it into the squashfs. At runtime, users can download different models to `/data/models/tts/` on the writable DATA partition.

**Step 3: Commit**

```bash
git add br2-external/board/llamaste/post_build.sh
git commit -m "build: bundle Piper TTS voice model in rootfs"
```

---

### Task 13: Full build + deploy + test

**Step 1: Rebuild llamaste package**

```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "
    export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
    export FORCE_UNSAFE_CONFIGURE=1
    cd /root/llamaste-build/output
    make llamaste-dirclean && make llamaste && make
"
```

**Step 2: Check binary links sherpa-onnx**

```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "
    ldd /root/llamaste-build/output/target/opt/llamaste/llamaste | grep -E 'sherpa|onnx'
"
```

Expected: `libsherpa-onnx-c-api.so => /usr/lib/libsherpa-onnx-c-api.so`

**Step 3: Check shared libs are in rootfs**

```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "
    ls -la /root/llamaste-build/output/target/usr/lib/libsherpa* /root/llamaste-build/output/target/usr/lib/libonnx*
"
```

**Step 4: Check model files are bundled**

```bash
MSYS_NO_PATHCONV=1 wsl -d Ubuntu -u root -- bash -c "
    ls -la /root/llamaste-build/output/target/data/models/tts/
"
```

**Step 5: Deploy to VDI and test**

Use the VDI deploy pattern from `scripts/deploy-to-vdi.sh`:
1. Stop VM
2. Update squashfs partition in VDI
3. Fix UUID
4. Start VM
5. Open `http://localhost:8080`
6. Test TTS via chat: type a message, click speaker button, verify audio plays
7. Check serial log for `[voice] sherpa-onnx TTS initialized (Piper VITS)`

**Step 6: Verify via API**

```bash
curl http://localhost:8080/llamaste/audio/status
```

Expected: `"tts_engine": "sherpa-onnx"`, `"tts_available": true`

```bash
curl -X POST http://localhost:8080/llamaste/audio/tts \
     -H "Content-Type: application/json" \
     -d '{"text": "Hello, this is Llamaste speaking with natural voice."}' \
     --output test.wav
```

Verify: `test.wav` is 16kHz or 22kHz, natural-sounding speech (not robotic).

**Step 7: Commit final state**

```bash
git add -A
git commit -m "feat: sherpa-onnx neural TTS — Piper VITS voice upgrade complete"
```
