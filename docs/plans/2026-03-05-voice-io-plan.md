# Phase 3a: Voice I/O Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add always-listening voice I/O to Llamaste — microphone capture, speech-to-text via whisper.cpp, voice activity detection via Silero VAD, text-to-speech via Piper, and web UI voice integration.

**Architecture:** ALSA capture -> Silero VAD (always running, 0.4% CPU) -> whisper.cpp STT (on speech) -> wake phrase detection -> LLM agent -> Piper TTS (separate process, GPL isolation) -> ALSA playback. whisper.cpp is linked directly into the llamaste binary (MIT, C API). Piper runs as a subprocess communicating via stdin/stdout pipes.

**Tech Stack:** whisper.cpp (STT), Silero VAD (voice activity), Piper + espeak-ng + ONNX Runtime (TTS), ALSA (audio I/O), Buildroot (packaging), musl libc

**Design doc:** `docs/plans/2026-03-05-voice-io-design.md`

---

## Task 1: Enable ALSA in Kernel Config

**Files:**
- Modify: `br2-external/board/llamaste/linux.config:187` (change `# CONFIG_SOUND is not set`)

**Step 1: Add sound subsystem to kernel config**

Replace line 187 in `br2-external/board/llamaste/linux.config`:

```
# CONFIG_SOUND is not set
```

With the following block (insert after the `CONFIG_FRAMEBUFFER_CONSOLE=y` line at 182):

```
# ---------------------------------------------------------------------------
# Sound subsystem (Voice I/O)
# ---------------------------------------------------------------------------
CONFIG_SOUND=y
CONFIG_SND=y
CONFIG_SND_TIMER=y
CONFIG_SND_PCM=y
CONFIG_SND_RAWMIDI=y
CONFIG_SND_SEQUENCER=y
CONFIG_SND_OSSEMUL=y
CONFIG_SND_MIXER_OSS=y
CONFIG_SND_PCM_OSS=y
# Intel HDA (real hardware)
CONFIG_SND_HDA_INTEL=y
CONFIG_SND_HDA_CODEC_REALTEK=y
CONFIG_SND_HDA_CODEC_ANALOG=y
CONFIG_SND_HDA_CODEC_HDMI=y
CONFIG_SND_HDA_GENERIC=y
# Intel ICH / AC'97 (VirtualBox)
CONFIG_SND_INTEL8X0=y
# USB microphones
CONFIG_SND_USB_AUDIO=y
```

**Step 2: Add alsa-lib to Buildroot defconfig**

Add to `br2-external/configs/llamaste_x86_64_defconfig` after line 50 (`BR2_PACKAGE_CA_CERTIFICATES=y`):

```
# Voice I/O (Phase 3a)
BR2_PACKAGE_ALSA_LIB=y
BR2_PACKAGE_ALSA_LIB_MIXER=y
BR2_PACKAGE_ALSA_LIB_PCM_PLUGINS=y
BR2_PACKAGE_ALSA_UTILS=y
```

**Step 3: Commit**

```bash
git add br2-external/board/llamaste/linux.config br2-external/configs/llamaste_x86_64_defconfig
git commit -m "feat: enable ALSA sound subsystem for Voice I/O"
```

---

## Task 2: Create whisper-cpp Buildroot Package

**Files:**
- Create: `br2-external/package/whisper-cpp/Config.in`
- Create: `br2-external/package/whisper-cpp/whisper-cpp.mk`
- Modify: `br2-external/Config.in:2` (add source line)
- Modify: `br2-external/configs/llamaste_x86_64_defconfig` (enable package)

**Step 1: Create Config.in**

Create `br2-external/package/whisper-cpp/Config.in`:

```
config BR2_PACKAGE_WHISPER_CPP
	bool "whisper-cpp"
	help
	  whisper.cpp — C/C++ port of OpenAI's Whisper speech
	  recognition model. Provides static library (libwhisper.a)
	  for linking into the llamaste binary.
	  https://github.com/ggerganov/whisper.cpp
```

**Step 2: Create whisper-cpp.mk**

Create `br2-external/package/whisper-cpp/whisper-cpp.mk`:

```makefile
################################################################################
#
# whisper-cpp -- whisper.cpp speech-to-text library
#
################################################################################

WHISPER_CPP_VERSION = v1.7.5
WHISPER_CPP_SITE = $(call github,ggerganov,whisper.cpp,$(WHISPER_CPP_VERSION))
WHISPER_CPP_LICENSE = MIT
WHISPER_CPP_LICENSE_FILES = LICENSE
WHISPER_CPP_INSTALL_STAGING = YES

WHISPER_CPP_CONF_OPTS = \
	-DCMAKE_BUILD_TYPE=Release \
	-DGGML_STATIC=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DGGML_NATIVE=OFF \
	-DGGML_CPU=ON \
	-DGGML_CUDA=OFF \
	-DGGML_VULKAN=OFF \
	-DGGML_METAL=OFF \
	-DGGML_BLAS=OFF \
	-DWHISPER_BUILD_TESTS=OFF \
	-DWHISPER_BUILD_EXAMPLES=OFF \
	-DWHISPER_BUILD_SERVER=OFF

# Install static library + headers to staging for linking into llamaste
define WHISPER_CPP_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/src/libwhisper.a \
		$(STAGING_DIR)/usr/lib/libwhisper.a
	$(INSTALL) -D -m 0644 $(@D)/ggml/src/libggml.a \
		$(STAGING_DIR)/usr/lib/libggml.a
	$(INSTALL) -D -m 0644 $(@D)/ggml/src/libggml-base.a \
		$(STAGING_DIR)/usr/lib/libggml-base.a
	$(INSTALL) -D -m 0644 $(@D)/ggml/src/libggml-cpu.a \
		$(STAGING_DIR)/usr/lib/libggml-cpu.a
	$(INSTALL) -D -m 0644 $(@D)/include/whisper.h \
		$(STAGING_DIR)/usr/include/whisper.h
	$(INSTALL) -D -m 0644 $(@D)/ggml/include/ggml.h \
		$(STAGING_DIR)/usr/include/ggml.h
endef

$(eval $(cmake-package))
```

**Step 3: Register package in Config.in**

Add to `br2-external/Config.in` after line 2:

```
source "$BR2_EXTERNAL_LLAMASTE_PATH/package/whisper-cpp/Config.in"
```

**Step 4: Enable in defconfig**

Add to `br2-external/configs/llamaste_x86_64_defconfig` after the alsa-lib lines:

```
BR2_PACKAGE_WHISPER_CPP=y
```

**Step 5: Commit**

```bash
git add br2-external/package/whisper-cpp/
git add br2-external/Config.in br2-external/configs/llamaste_x86_64_defconfig
git commit -m "feat: add whisper-cpp Buildroot package (static library)"
```

---

## Task 3: Create voice.h Header

**Files:**
- Create: `src/llamaste/voice.h`

**Step 1: Write voice.h**

Create `src/llamaste/voice.h`:

```cpp
#pragma once
// voice.h — Voice I/O pipeline for Llamaste
//
// Manages: ALSA capture, Silero VAD, whisper.cpp STT, Piper TTS subprocess.
// The voice pipeline runs in a background thread, continuously listening
// for the wake phrase and processing voice commands through the agent loop.

#include <string>
#include <vector>
#include <atomic>
#include <functional>
#include <mutex>

// Voice pipeline state
enum class VoiceState {
    DISABLED,       // No audio hardware or explicitly disabled
    INITIALIZING,   // Loading models, opening ALSA
    LISTENING,      // VAD running, waiting for speech
    RECORDING,      // Speech detected, accumulating audio
    TRANSCRIBING,   // Running whisper inference
    PROCESSING,     // Wake phrase detected, LLM processing
    SPEAKING,       // TTS playback in progress
    ERROR           // Initialization or runtime error
};

// Voice pipeline configuration
struct VoiceConfig {
    std::string wake_phrase = "llamaste";   // Case-insensitive match in transcription
    std::string whisper_model = "/data/models/ggml-tiny.en-q5_1.bin";
    std::string piper_binary = "/opt/llamaste/piper";
    std::string piper_model = "/data/models/en_US-amy-low.onnx";
    std::string alsa_device = "default";    // ALSA capture device
    int sample_rate = 16000;                // 16kHz for whisper
    float vad_threshold = 0.5f;             // Speech probability threshold
    int silence_ms = 300;                   // Silence duration to end recording
    int max_record_ms = 30000;              // Max single recording (30s)
    bool enabled = true;
};

// Voice pipeline manager
class VoicePipeline {
public:
    VoicePipeline();
    ~VoicePipeline();

    // Initialize pipeline (load models, open ALSA). Returns false on error.
    bool init(const VoiceConfig& config);

    // Start/stop the always-listening thread
    void start();
    void stop();

    // Current state
    VoiceState state() const { return state_.load(); }
    std::string state_string() const;
    std::string last_error() const;

    // One-shot transcription: transcribe a WAV buffer (for HTTP endpoint)
    // Input: raw PCM samples (16kHz, mono, float32)
    std::string transcribe(const std::vector<float>& samples);

    // One-shot TTS: synthesize text to WAV (for HTTP endpoint)
    // Returns raw PCM samples (16kHz or 22050Hz, mono, int16)
    std::vector<int16_t> speak(const std::string& text);

    // Configuration
    VoiceConfig config() const;
    void set_config(const VoiceConfig& config);

    // Set callback for when a voice command is ready for the agent
    // Signature: void(const std::string& transcribed_text)
    void set_command_callback(std::function<void(const std::string&)> cb);

private:
    std::atomic<VoiceState> state_{VoiceState::DISABLED};
    VoiceConfig config_;
    mutable std::mutex config_mutex_;
    std::string last_error_;
    std::function<void(const std::string&)> command_callback_;

    // Opaque handles (defined in voice.cpp to avoid header deps)
    struct Impl;
    Impl* impl_ = nullptr;

    void voice_thread_fn();
};

// Convert WAV file bytes to float32 PCM samples for whisper
// Handles: 16-bit PCM WAV, any sample rate (resamples to 16kHz)
bool wav_to_float32(const std::vector<uint8_t>& wav_data,
                    std::vector<float>& out_samples,
                    int target_sample_rate = 16000);
```

**Step 2: Commit**

```bash
git add src/llamaste/voice.h
git commit -m "feat: add voice.h — Voice I/O pipeline header"
```

---

## Task 4: Create tools_audio.cpp — Audio Tool Stubs

**Files:**
- Create: `src/llamaste/tools_audio.cpp`
- Modify: `src/llamaste/tools.h:56` (add declaration)
- Modify: `src/llamaste/tools.cpp:106` (add registration call)
- Modify: `src/llamaste/CMakeLists.txt:29` (add source file)

**Step 1: Write the test**

Create `tests/test_audio_tools.cpp`:

```cpp
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../src/llamaste/tools.h"
#include "../src/llamaste/json.hpp"

using json = nlohmann::json;

static int tests_passed = 0;

#define TEST(name) printf("TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)

int main() {
    printf("=== Audio Tool Tests ===\n\n");

    // Test 1: Registration
    TEST("audio tools register correctly");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        auto names = reg.tool_names();
        bool has_transcribe = false, has_speak = false, has_status = false;
        for (const auto& n : names) {
            if (n == "audio.transcribe") has_transcribe = true;
            if (n == "audio.speak") has_speak = true;
            if (n == "audio.status") has_status = true;
        }
        assert(has_transcribe);
        assert(has_speak);
        assert(has_status);
        PASS();
    }

    // Test 2: audio.status returns valid JSON
    TEST("audio.status returns state");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.status", "{}");
        auto j = json::parse(result);
        assert(j.contains("state"));
        assert(j.contains("enabled"));
        PASS();
    }

    // Test 3: audio.transcribe with no audio returns error
    TEST("audio.transcribe with no data returns error");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.transcribe", R"json({})json");
        auto j = json::parse(result);
        assert(j.contains("error"));
        PASS();
    }

    // Test 4: audio.speak with empty text returns error
    TEST("audio.speak with empty text returns error");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.speak", R"json({"text": ""})json");
        auto j = json::parse(result);
        assert(j.contains("error"));
        PASS();
    }

    printf("\n=== SUMMARY ===\n");
    printf("Tests passed: %d/4\n", tests_passed);
    return tests_passed == 4 ? 0 : 1;
}
```

**Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I src/llamaste -o tests/build/test_audio_tools tests/test_audio_tools.cpp src/llamaste/tools.cpp src/llamaste/tools_fs.cpp src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp src/llamaste/tools_model.cpp src/llamaste/tools_model_download.cpp`
Expected: FAIL — `register_audio_tools` not defined

**Step 3: Write tools_audio.cpp**

Create `src/llamaste/tools_audio.cpp`:

```cpp
// tools_audio.cpp — Audio tools for Llamaste Voice I/O
//
// Tools: audio.transcribe, audio.speak, audio.status, audio.config
// These provide the LLM and HTTP API with voice capabilities.

#include "tools.h"
#include "json.hpp"
#include <string>

using json = nlohmann::json;

// Forward declare — voice pipeline is initialized in child_main.cpp
// These stubs work without whisper/piper loaded (return errors gracefully)

static std::string handle_audio_status(const std::string& args_json) {
    (void)args_json;
    json result;
    result["state"] = "disabled";
    result["enabled"] = false;
    result["whisper_loaded"] = false;
    result["piper_available"] = false;
    result["alsa_available"] = false;
    result["wake_phrase"] = "llamaste";
    return result.dump();
}

static std::string handle_audio_transcribe(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        return R"json({"error": "Invalid JSON arguments"})json";
    }

    std::string audio_file = args.value("audio_file", "");
    if (audio_file.empty()) {
        return R"json({"error": "audio_file parameter required"})json";
    }

    // Validate path is under /data/
    if (audio_file.substr(0, 6) != "/data/") {
        return R"json({"error": "audio_file must be under /data/"})json";
    }

    // TODO: Implement with whisper.cpp when voice pipeline is initialized
    json result;
    result["error"] = "Voice pipeline not initialized — whisper.cpp not loaded";
    return result.dump();
}

static std::string handle_audio_speak(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        return R"json({"error": "Invalid JSON arguments"})json";
    }

    std::string text = args.value("text", "");
    if (text.empty()) {
        return R"json({"error": "text parameter required (non-empty)"})json";
    }

    // TODO: Implement with Piper TTS when voice pipeline is initialized
    json result;
    result["error"] = "Voice pipeline not initialized — Piper TTS not available";
    return result.dump();
}

static std::string handle_audio_config(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        return R"json({"error": "Invalid JSON arguments"})json";
    }

    // GET config (no args or empty args)
    json result;
    result["wake_phrase"] = "llamaste";
    result["enabled"] = false;
    result["whisper_model"] = "ggml-tiny.en-q5_1.bin";
    result["piper_voice"] = "en_US-amy-low";
    result["sample_rate"] = 16000;
    result["vad_threshold"] = 0.5;
    return result.dump();
}

void register_audio_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "audio.status",
        .description = "Get the current voice pipeline status including "
                       "whether whisper (STT) and piper (TTS) are loaded, "
                       "ALSA audio availability, and current listening state.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_audio_status
    });

    reg.register_tool({
        .name = "audio.transcribe",
        .description = "Transcribe an audio file (WAV format, 16kHz mono) "
                       "to text using whisper.cpp. The file must be under /data/.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "audio_file": {
                    "type": "string",
                    "description": "Path to WAV audio file under /data/"
                },
                "language": {
                    "type": "string",
                    "description": "Language code (default: en)",
                    "default": "en"
                }
            },
            "required": ["audio_file"]
        })json",
        .handler = handle_audio_transcribe
    });

    reg.register_tool({
        .name = "audio.speak",
        .description = "Convert text to speech using Piper TTS. "
                       "Generates a WAV file and optionally plays through speakers.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "text": {
                    "type": "string",
                    "description": "Text to synthesize as speech"
                },
                "play": {
                    "type": "boolean",
                    "description": "Play audio through speakers (default: true)",
                    "default": true
                }
            },
            "required": ["text"]
        })json",
        .handler = handle_audio_speak
    });

    reg.register_tool({
        .name = "audio.config",
        .description = "Get or set voice pipeline configuration including "
                       "wake phrase, VAD threshold, and TTS voice.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "wake_phrase": {
                    "type": "string",
                    "description": "Wake phrase to listen for (case-insensitive)"
                },
                "enabled": {
                    "type": "boolean",
                    "description": "Enable/disable voice pipeline"
                },
                "vad_threshold": {
                    "type": "number",
                    "description": "VAD speech probability threshold (0.0-1.0)"
                }
            }
        })json",
        .handler = handle_audio_config
    });
}
```

**Step 4: Add declaration to tools.h**

Add after line 56 in `src/llamaste/tools.h`:

```cpp
void register_audio_tools(ToolRegistry& reg);
```

**Step 5: Add registration to tools.cpp**

Add at line 106 in `src/llamaste/tools.cpp`, before the closing `}`:

```cpp
    register_audio_tools(reg);
```

**Step 6: Add source to CMakeLists.txt**

Add `tools_audio.cpp` to `LLAMASTE_SOURCES` in `src/llamaste/CMakeLists.txt` after line 23 (`tools_model_download.cpp`):

```cmake
    tools_audio.cpp
```

**Step 7: Run test to verify it passes**

Run: `g++ -std=c++17 -I src/llamaste -o tests/build/test_audio_tools tests/test_audio_tools.cpp src/llamaste/tools.cpp src/llamaste/tools_fs.cpp src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp src/llamaste/tools_model.cpp src/llamaste/tools_model_download.cpp src/llamaste/tools_audio.cpp`
Expected: PASS — 4/4 tests

**Step 8: Add test to host-test.sh**

Add a new suite (Suite 10) at the end of `scripts/host-test.sh`, before the summary section:

```bash
# ---------------------------------------------------------------
# Suite 10: Audio Tools
# ---------------------------------------------------------------
echo -e "${BOLD}--- [10/10] Audio Tools ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_audio_tools" \
    "${TESTS}/test_audio_tools.cpp" \
    "${SRC}/tools.cpp" \
    "${SRC}/tools_fs.cpp" \
    "${SRC}/tools_process.cpp" \
    "${SRC}/tools_network.cpp" \
    "${SRC}/tools_system.cpp" \
    "${SRC}/tools_config.cpp" \
    "${SRC}/tools_model.cpp" \
    "${SRC}/tools_model_download.cpp" \
    "${SRC}/tools_audio.cpp" 2>&1; then
    if "${BUILD_DIR}/test_audio_tools"; then
        suite_pass "Audio Tools"
    else
        suite_fail "Audio Tools (runtime)"
    fi
else
    suite_fail "Audio Tools (compile)"
fi
echo ""
```

Also update all suite count references from `9` to `10` (e.g., `[1/9]` becomes `[1/10]`).

**Step 9: Commit**

```bash
git add src/llamaste/tools_audio.cpp src/llamaste/tools.h src/llamaste/tools.cpp
git add src/llamaste/CMakeLists.txt tests/test_audio_tools.cpp scripts/host-test.sh
git commit -m "feat: add audio.* tool stubs (transcribe, speak, status, config)"
```

---

## Task 5: Add HTTP Audio Endpoints to child_main.cpp

**Files:**
- Modify: `src/llamaste/child_main.cpp` (add routes near other `/llamaste/*` routes)

**Step 1: Add audio HTTP endpoints**

Add the following routes in `child_main.cpp` alongside the other `svr.Post`/`svr.Get` routes (after the existing `/llamaste/chat` route):

```cpp
    // --- Voice I/O endpoints ---

    // POST /llamaste/audio/transcribe — Upload WAV, return transcription
    svr.Post("/llamaste/audio/transcribe", require_auth(
        [&registry](const httplib::Request& req, httplib::Response& res) {
            // Accept multipart/form-data with "audio" file field
            // or raw application/octet-stream body
            if (req.has_file("audio")) {
                const auto& file = req.get_file_value("audio");
                // Save to temp file under /data/
                std::string tmp_path = "/data/tmp/audio_upload_" +
                    std::to_string(time(nullptr)) + ".wav";
                {
                    std::ofstream ofs(tmp_path, std::ios::binary);
                    ofs.write(file.content.data(), file.content.size());
                }
                // Dispatch to audio.transcribe tool
                json args;
                args["audio_file"] = tmp_path;
                std::string result = registry.dispatch("audio.transcribe",
                                                        args.dump());
                // Clean up temp file
                unlink(tmp_path.c_str());
                res.set_content(result, "application/json");
            } else if (!req.body.empty()) {
                // Raw WAV body
                std::string tmp_path = "/data/tmp/audio_upload_" +
                    std::to_string(time(nullptr)) + ".wav";
                {
                    std::ofstream ofs(tmp_path, std::ios::binary);
                    ofs.write(req.body.data(), req.body.size());
                }
                json args;
                args["audio_file"] = tmp_path;
                std::string result = registry.dispatch("audio.transcribe",
                                                        args.dump());
                unlink(tmp_path.c_str());
                res.set_content(result, "application/json");
            } else {
                json err;
                err["error"] = "No audio data provided";
                res.status = 400;
                res.set_content(err.dump(), "application/json");
            }
        }
    ));

    // POST /llamaste/audio/speak — Text to speech, return WAV audio
    svr.Post("/llamaste/audio/speak", require_auth(
        [&registry](const httplib::Request& req, httplib::Response& res) {
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded()) {
                res.status = 400;
                json err;
                err["error"] = "Invalid JSON";
                res.set_content(err.dump(), "application/json");
                return;
            }
            std::string result = registry.dispatch("audio.speak", req.body);
            auto j = json::parse(result, nullptr, false);
            if (!j.is_discarded() && j.contains("audio_file")) {
                // Return WAV file
                std::string path = j["audio_file"].get<std::string>();
                std::ifstream ifs(path, std::ios::binary);
                if (ifs) {
                    std::string wav((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
                    res.set_content(wav, "audio/wav");
                } else {
                    res.set_content(result, "application/json");
                }
            } else {
                // Error or no audio — return JSON
                res.set_content(result, "application/json");
            }
        }
    ));

    // GET /llamaste/audio/status — Voice pipeline state
    svr.Get("/llamaste/audio/status", require_auth(
        [&registry](const httplib::Request& req, httplib::Response& res) {
            std::string result = registry.dispatch("audio.status", "{}");
            res.set_content(result, "application/json");
        }
    ));

    // GET/POST /llamaste/audio/config — Voice settings
    svr.Get("/llamaste/audio/config", require_auth(
        [&registry](const httplib::Request& req, httplib::Response& res) {
            std::string result = registry.dispatch("audio.config", "{}");
            res.set_content(result, "application/json");
        }
    ));
    svr.Post("/llamaste/audio/config", require_auth(
        [&registry](const httplib::Request& req, httplib::Response& res) {
            std::string result = registry.dispatch("audio.config", req.body);
            res.set_content(result, "application/json");
        }
    ));
```

**Step 2: Add required includes**

Add to the top of child_main.cpp (if not already present):

```cpp
#include <fstream>
```

**Step 3: Create /data/tmp directory in init.cpp**

Ensure `/data/tmp` exists for audio uploads. In `init.cpp`, after the existing `mkdir("/data/models"...)` call:

```cpp
mkdir("/data/tmp", 0755);
```

**Step 4: Commit**

```bash
git add src/llamaste/child_main.cpp src/llamaste/init.cpp
git commit -m "feat: add HTTP audio endpoints (transcribe, speak, status, config)"
```

---

## Task 6: Implement WAV Parsing + whisper.cpp Integration in voice.cpp

**Files:**
- Create: `src/llamaste/voice.cpp`
- Modify: `src/llamaste/CMakeLists.txt` (add voice.cpp, link whisper)

**Step 1: Write voice.cpp with WAV parsing and whisper transcription**

Create `src/llamaste/voice.cpp`:

```cpp
// voice.cpp — Voice I/O pipeline for Llamaste
//
// Phase 3a-1: WAV parsing + whisper.cpp transcription
// Phase 3a-2: ALSA capture + VAD + always-listening (future)
// Phase 3a-3: Piper TTS subprocess (future)

#include "voice.h"
#include "json.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <thread>

using json = nlohmann::json;

#ifdef HAVE_WHISPER
#include <whisper.h>
#endif

// ---------------------------------------------------------------------------
// WAV file parsing
// ---------------------------------------------------------------------------

// Standard WAV header (44 bytes)
struct WavHeader {
    char riff[4];           // "RIFF"
    uint32_t file_size;     // File size - 8
    char wave[4];           // "WAVE"
    char fmt_id[4];         // "fmt "
    uint32_t fmt_size;      // Format chunk size (16 for PCM)
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

bool wav_to_float32(const std::vector<uint8_t>& wav_data,
                    std::vector<float>& out_samples,
                    int target_sample_rate) {
    if (wav_data.size() < 44) return false;

    auto* hdr = reinterpret_cast<const WavHeader*>(wav_data.data());
    if (memcmp(hdr->riff, "RIFF", 4) != 0 ||
        memcmp(hdr->wave, "WAVE", 4) != 0) {
        return false;
    }

    if (hdr->audio_format != 1) {
        fprintf(stderr, "[voice] WAV: only PCM format supported (got %d)\n",
                hdr->audio_format);
        return false;
    }

    // Find data chunk (skip past fmt and any extra chunks)
    size_t pos = 12; // after RIFF header
    const uint8_t* data = wav_data.data();
    size_t data_offset = 0;
    uint32_t data_size = 0;

    while (pos + 8 <= wav_data.size()) {
        char chunk_id[5] = {};
        memcpy(chunk_id, data + pos, 4);
        uint32_t chunk_size;
        memcpy(&chunk_size, data + pos + 4, 4);

        if (memcmp(chunk_id, "data", 4) == 0) {
            data_offset = pos + 8;
            data_size = chunk_size;
            break;
        }
        pos += 8 + chunk_size;
    }

    if (data_offset == 0 || data_size == 0) {
        fprintf(stderr, "[voice] WAV: data chunk not found\n");
        return false;
    }

    int channels = hdr->num_channels;
    int bps = hdr->bits_per_sample;
    int src_rate = hdr->sample_rate;

    // Convert to float32 mono
    size_t num_samples = data_size / (bps / 8) / channels;
    std::vector<float> raw(num_samples);

    if (bps == 16) {
        const int16_t* samples = reinterpret_cast<const int16_t*>(
            wav_data.data() + data_offset);
        for (size_t i = 0; i < num_samples; i++) {
            // Take first channel if stereo
            raw[i] = samples[i * channels] / 32768.0f;
        }
    } else if (bps == 32) {
        const int32_t* samples = reinterpret_cast<const int32_t*>(
            wav_data.data() + data_offset);
        for (size_t i = 0; i < num_samples; i++) {
            raw[i] = samples[i * channels] / 2147483648.0f;
        }
    } else {
        fprintf(stderr, "[voice] WAV: unsupported bits/sample: %d\n", bps);
        return false;
    }

    // Resample if needed (simple linear interpolation)
    if (src_rate != target_sample_rate) {
        double ratio = (double)target_sample_rate / src_rate;
        size_t new_len = (size_t)(num_samples * ratio);
        out_samples.resize(new_len);
        for (size_t i = 0; i < new_len; i++) {
            double src_pos = i / ratio;
            size_t idx = (size_t)src_pos;
            double frac = src_pos - idx;
            if (idx + 1 < num_samples) {
                out_samples[i] = (float)(raw[idx] * (1.0 - frac) +
                                         raw[idx + 1] * frac);
            } else {
                out_samples[i] = raw[idx];
            }
        }
    } else {
        out_samples = std::move(raw);
    }

    fprintf(stderr, "[voice] WAV: %zu samples, %dHz %dch %dbit -> %zu samples @ %dHz\n",
            num_samples, src_rate, channels, bps,
            out_samples.size(), target_sample_rate);
    return true;
}

// ---------------------------------------------------------------------------
// VoicePipeline implementation
// ---------------------------------------------------------------------------

struct VoicePipeline::Impl {
#ifdef HAVE_WHISPER
    struct whisper_context* whisper_ctx = nullptr;
#endif
    std::thread voice_thread;
    std::atomic<bool> running{false};
};

VoicePipeline::VoicePipeline() : impl_(new Impl) {}

VoicePipeline::~VoicePipeline() {
    stop();
#ifdef HAVE_WHISPER
    if (impl_->whisper_ctx) {
        whisper_free(impl_->whisper_ctx);
    }
#endif
    delete impl_;
}

bool VoicePipeline::init(const VoiceConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
    state_.store(VoiceState::INITIALIZING);

#ifdef HAVE_WHISPER
    // Load whisper model
    fprintf(stderr, "[voice] Loading whisper model: %s\n",
            config_.whisper_model.c_str());

    struct whisper_context_params cparams = whisper_context_default_params();
    impl_->whisper_ctx = whisper_init_from_file_with_params(
        config_.whisper_model.c_str(), cparams);

    if (!impl_->whisper_ctx) {
        last_error_ = "Failed to load whisper model: " + config_.whisper_model;
        fprintf(stderr, "[voice] %s\n", last_error_.c_str());
        state_.store(VoiceState::ERROR);
        return false;
    }

    fprintf(stderr, "[voice] Whisper model loaded successfully\n");
    state_.store(VoiceState::LISTENING);
    return true;
#else
    last_error_ = "whisper.cpp not compiled in (HAVE_WHISPER not defined)";
    fprintf(stderr, "[voice] %s\n", last_error_.c_str());
    state_.store(VoiceState::DISABLED);
    return false;
#endif
}

std::string VoicePipeline::transcribe(const std::vector<float>& samples) {
#ifdef HAVE_WHISPER
    if (!impl_->whisper_ctx) {
        return "";
    }

    struct whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_realtime = false;
    wparams.print_progress = false;
    wparams.print_timestamps = false;
    wparams.print_special = false;
    wparams.single_segment = true;
    wparams.no_context = true;
    wparams.language = "en";

    if (whisper_full(impl_->whisper_ctx, wparams,
                     samples.data(), (int)samples.size()) != 0) {
        fprintf(stderr, "[voice] whisper_full failed\n");
        return "";
    }

    int n_segments = whisper_full_n_segments(impl_->whisper_ctx);
    std::string text;
    for (int i = 0; i < n_segments; i++) {
        text += whisper_full_get_segment_text(impl_->whisper_ctx, i);
    }

    // Trim leading/trailing whitespace
    size_t start = text.find_first_not_of(" \t\n\r");
    size_t end = text.find_last_not_of(" \t\n\r");
    if (start != std::string::npos) {
        text = text.substr(start, end - start + 1);
    }

    fprintf(stderr, "[voice] Transcription: \"%s\"\n", text.c_str());
    return text;
#else
    (void)samples;
    return "";
#endif
}

std::vector<int16_t> VoicePipeline::speak(const std::string& text) {
    // TODO: Phase 3a-3 — Piper TTS implementation
    (void)text;
    return {};
}

void VoicePipeline::start() {
    // TODO: Phase 3a-2 — always-listening thread
}

void VoicePipeline::stop() {
    if (impl_->running.load()) {
        impl_->running.store(false);
        if (impl_->voice_thread.joinable()) {
            impl_->voice_thread.join();
        }
    }
}

std::string VoicePipeline::state_string() const {
    switch (state_.load()) {
        case VoiceState::DISABLED:      return "disabled";
        case VoiceState::INITIALIZING:  return "initializing";
        case VoiceState::LISTENING:     return "listening";
        case VoiceState::RECORDING:     return "recording";
        case VoiceState::TRANSCRIBING:  return "transcribing";
        case VoiceState::PROCESSING:    return "processing";
        case VoiceState::SPEAKING:      return "speaking";
        case VoiceState::ERROR:         return "error";
    }
    return "unknown";
}

std::string VoicePipeline::last_error() const { return last_error_; }

VoiceConfig VoicePipeline::config() const {
    std::lock_guard<std::mutex> lock(config_mutex_);
    return config_;
}

void VoicePipeline::set_config(const VoiceConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;
}

void VoicePipeline::set_command_callback(
    std::function<void(const std::string&)> cb) {
    command_callback_ = std::move(cb);
}

void VoicePipeline::voice_thread_fn() {
    // TODO: Phase 3a-2 — ALSA capture + VAD + always-listening loop
}
```

**Step 2: Update CMakeLists.txt**

Add `voice.cpp` to `LLAMASTE_SOURCES` in `src/llamaste/CMakeLists.txt` after `tools_audio.cpp`:

```cmake
    voice.cpp
```

Add whisper library linking after the CURL section (after line 99):

```cmake
# whisper.cpp for speech-to-text (Voice I/O)
find_library(WHISPER_LIB NAMES whisper PATHS "${CMAKE_FIND_ROOT_PATH}/usr/lib" NO_DEFAULT_PATH)
find_library(GGML_LIB NAMES ggml PATHS "${CMAKE_FIND_ROOT_PATH}/usr/lib" NO_DEFAULT_PATH)
find_library(GGML_BASE_LIB NAMES ggml-base PATHS "${CMAKE_FIND_ROOT_PATH}/usr/lib" NO_DEFAULT_PATH)
find_library(GGML_CPU_LIB NAMES ggml-cpu PATHS "${CMAKE_FIND_ROOT_PATH}/usr/lib" NO_DEFAULT_PATH)
if(WHISPER_LIB)
    message(STATUS "Found whisper: ${WHISPER_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_WHISPER)
    target_link_libraries(llamaste PRIVATE ${WHISPER_LIB})
    if(GGML_LIB)
        target_link_libraries(llamaste PRIVATE ${GGML_LIB})
    endif()
    if(GGML_BASE_LIB)
        target_link_libraries(llamaste PRIVATE ${GGML_BASE_LIB})
    endif()
    if(GGML_CPU_LIB)
        target_link_libraries(llamaste PRIVATE ${GGML_CPU_LIB})
    endif()
else()
    message(STATUS "whisper not found - speech-to-text disabled")
endif()

# ALSA for audio capture/playback (Voice I/O)
find_library(ALSA_LIB NAMES asound)
if(ALSA_LIB)
    message(STATUS "Found ALSA: ${ALSA_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_ALSA)
    target_link_libraries(llamaste PRIVATE ${ALSA_LIB})
else()
    message(STATUS "ALSA not found - audio capture disabled")
endif()
```

**Step 3: Commit**

```bash
git add src/llamaste/voice.cpp src/llamaste/CMakeLists.txt
git commit -m "feat: implement voice.cpp with WAV parsing + whisper.cpp STT"
```

---

## Task 7: Wire Voice Pipeline into tools_audio.cpp

**Files:**
- Modify: `src/llamaste/tools_audio.cpp` (connect to VoicePipeline)
- Modify: `src/llamaste/child_main.cpp` (instantiate VoicePipeline)

**Step 1: Add global voice pipeline pointer**

In `child_main.cpp`, add a global (near the other globals like `g_llama_pid`):

```cpp
#include "voice.h"
static VoicePipeline* g_voice = nullptr;
```

In the child process startup (after model loading), add voice pipeline initialization:

```cpp
    // Initialize voice pipeline (if whisper model available)
    VoicePipeline voice_pipeline;
    VoiceConfig vcfg;
    if (access(vcfg.whisper_model.c_str(), R_OK) == 0) {
        if (voice_pipeline.init(vcfg)) {
            g_voice = &voice_pipeline;
            fprintf(stderr, "[child] Voice pipeline initialized\n");
        }
    } else {
        fprintf(stderr, "[child] Whisper model not found at %s — voice disabled\n",
                vcfg.whisper_model.c_str());
    }
```

**Step 2: Update tools_audio.cpp handlers to use global pipeline**

In `tools_audio.cpp`, add:

```cpp
#include "voice.h"

// Defined in child_main.cpp
extern VoicePipeline* g_voice;
```

Update `handle_audio_status`:

```cpp
static std::string handle_audio_status(const std::string& args_json) {
    (void)args_json;
    json result;
    if (g_voice) {
        result["state"] = g_voice->state_string();
        result["enabled"] = (g_voice->state() != VoiceState::DISABLED);
        result["whisper_loaded"] = (g_voice->state() >= VoiceState::LISTENING);
        auto cfg = g_voice->config();
        result["wake_phrase"] = cfg.wake_phrase;
    } else {
        result["state"] = "disabled";
        result["enabled"] = false;
        result["whisper_loaded"] = false;
    }
    result["piper_available"] = false;  // Phase 3a-3
    return result.dump();
}
```

Update `handle_audio_transcribe`:

```cpp
static std::string handle_audio_transcribe(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        return R"json({"error": "Invalid JSON arguments"})json";
    }

    std::string audio_file = args.value("audio_file", "");
    if (audio_file.empty()) {
        return R"json({"error": "audio_file parameter required"})json";
    }
    if (audio_file.substr(0, 6) != "/data/") {
        return R"json({"error": "audio_file must be under /data/"})json";
    }

    if (!g_voice || g_voice->state() == VoiceState::DISABLED) {
        return R"json({"error": "Voice pipeline not initialized"})json";
    }

    // Read WAV file
    std::ifstream ifs(audio_file, std::ios::binary);
    if (!ifs) {
        json err;
        err["error"] = "Cannot open audio file: " + audio_file;
        return err.dump();
    }
    std::vector<uint8_t> wav_data(
        (std::istreambuf_iterator<char>(ifs)),
        std::istreambuf_iterator<char>());

    // Parse WAV to float32 PCM
    std::vector<float> samples;
    if (!wav_to_float32(wav_data, samples)) {
        return R"json({"error": "Invalid WAV file format"})json";
    }

    // Transcribe
    std::string text = g_voice->transcribe(samples);
    json result;
    result["text"] = text;
    result["audio_file"] = audio_file;
    result["duration_seconds"] = (double)samples.size() / 16000.0;
    return result.dump();
}
```

**Step 3: Commit**

```bash
git add src/llamaste/tools_audio.cpp src/llamaste/child_main.cpp
git commit -m "feat: wire voice pipeline into audio tools + child_main"
```

---

## Task 8: Build and Test in WSL2

**Files:** None (build/test only)

**Step 1: Run host tests**

```bash
cd /mnt/d/Llamaste && bash scripts/host-test.sh
```

Expected: 10/10 suites pass (audio tools suite is new)

**Step 2: Build Buildroot with whisper-cpp and alsa-lib**

```bash
cd /root/llamaste-build/output
# Save updated defconfig
make savedefconfig
# Rebuild with new packages
make whisper-cpp
make alsa-lib
make llamaste-dirclean && make llamaste
make
```

**Step 3: Test in QEMU**

```bash
bash /mnt/d/Llamaste/scripts/qemu-boot-test.sh /root/llamaste-build/output/images/llamaste.img
```

Verify: system boots, `audio.status` tool shows state (disabled if no model, or listening if model present)

**Step 4: Commit any fixups**

```bash
git add -A && git commit -m "fix: build integration fixes for voice I/O"
```

---

## Task 9: Download whisper model at first boot

**Files:**
- Modify: `src/llamaste/tools_audio.cpp` (add model download tool)

**Step 1: Add audio.download_model tool**

Add to `tools_audio.cpp`:

```cpp
static std::string handle_audio_download_model(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    std::string model = args.value("model", "tiny.en");

    // Map model name to URL
    std::string url;
    std::string filename;
    if (model == "tiny.en" || model == "ggml-tiny.en-q5_1") {
        url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
              "ggml-tiny.en-q5_1.bin";
        filename = "ggml-tiny.en-q5_1.bin";
    } else if (model == "base.en" || model == "ggml-base.en-q5_1") {
        url = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
              "ggml-base.en-q5_1.bin";
        filename = "ggml-base.en-q5_1.bin";
    } else {
        json err;
        err["error"] = "Unknown model: " + model +
                       ". Options: tiny.en (31MB), base.en (60MB)";
        return err.dump();
    }

    std::string dest = "/data/models/" + filename;

    // Check if already exists
    if (access(dest.c_str(), R_OK) == 0) {
        json result;
        result["status"] = "already_downloaded";
        result["path"] = dest;
        return result.dump();
    }

    // Download using libcurl (reuse pattern from tools_model_download.cpp)
    // ... (use existing download infrastructure)

    json result;
    result["status"] = "downloading";
    result["model"] = model;
    result["url"] = url;
    result["destination"] = dest;
    return result.dump();
}
```

Register it:

```cpp
    reg.register_tool({
        .name = "audio.download_model",
        .description = "Download a whisper speech-to-text model. "
                       "Options: tiny.en (31MB, fastest), base.en (60MB, better).",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "model": {
                    "type": "string",
                    "description": "Model name: tiny.en (31MB) or base.en (60MB)",
                    "default": "tiny.en"
                }
            }
        })json",
        .handler = handle_audio_download_model
    });
```

**Step 2: Commit**

```bash
git add src/llamaste/tools_audio.cpp
git commit -m "feat: add audio.download_model tool for whisper model download"
```

---

## Task 10: Web UI Microphone Button (Phase 3a-4)

**Files:**
- Modify: `src/llamaste/web/chat.js` (add mic button + WebAudio)
- Modify: `src/llamaste/web/style.css` (mic button styles)

**Step 1: Add microphone button to chat.js**

Add to `chat.js` after the send button handler:

```javascript
// --- Voice I/O ---
let mediaRecorder = null;
let audioChunks = [];
let isRecording = false;

const micBtn = document.getElementById('mic-btn');
if (micBtn) {
    micBtn.addEventListener('click', async () => {
        if (isRecording) {
            // Stop recording
            mediaRecorder.stop();
            micBtn.classList.remove('recording');
            isRecording = false;
            return;
        }

        try {
            const stream = await navigator.mediaDevices.getUserMedia({
                audio: { sampleRate: 16000, channelCount: 1 }
            });
            mediaRecorder = new MediaRecorder(stream, {
                mimeType: 'audio/webm;codecs=opus'
            });
            audioChunks = [];

            mediaRecorder.ondataavailable = (e) => {
                audioChunks.push(e.data);
            };

            mediaRecorder.onstop = async () => {
                stream.getTracks().forEach(t => t.stop());
                const blob = new Blob(audioChunks, { type: 'audio/webm' });

                // Upload to transcribe endpoint
                const formData = new FormData();
                formData.append('audio', blob, 'recording.webm');
                try {
                    const resp = await fetch('/llamaste/audio/transcribe', {
                        method: 'POST',
                        body: formData,
                        credentials: 'include'
                    });
                    const data = await resp.json();
                    if (data.text) {
                        // Insert transcription into chat input
                        document.getElementById('chat-input').value = data.text;
                    } else if (data.error) {
                        console.error('Transcription error:', data.error);
                    }
                } catch (err) {
                    console.error('Transcription failed:', err);
                }
            };

            mediaRecorder.start();
            micBtn.classList.add('recording');
            isRecording = true;

            // Auto-stop after 30 seconds
            setTimeout(() => {
                if (isRecording && mediaRecorder.state === 'recording') {
                    mediaRecorder.stop();
                    micBtn.classList.remove('recording');
                    isRecording = false;
                }
            }, 30000);
        } catch (err) {
            console.error('Microphone access denied:', err);
        }
    });
}
```

**Step 2: Add mic button to index.html**

In the chat input area, add before the send button:

```html
<button id="mic-btn" class="mic-btn" title="Voice input">&#127908;</button>
```

**Step 3: Add CSS styles**

Add to `style.css`:

```css
.mic-btn {
    background: none;
    border: 1px solid var(--border-color, #444);
    border-radius: 50%;
    width: 36px;
    height: 36px;
    cursor: pointer;
    font-size: 18px;
    display: flex;
    align-items: center;
    justify-content: center;
    transition: background 0.2s;
}
.mic-btn:hover { background: rgba(255,255,255,0.1); }
.mic-btn.recording {
    background: #c0392b;
    border-color: #e74c3c;
    animation: pulse 1s infinite;
}
@keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.6; }
}
```

**Step 4: Commit**

```bash
git add src/llamaste/web/chat.js src/llamaste/web/index.html src/llamaste/web/style.css
git commit -m "feat: add microphone button to web UI for voice input"
```

---

## Task 11: Build, Test End-to-End, Update VirtualBox

**Files:** None (build/test only)

**Step 1: Run host tests**

```bash
cd /mnt/d/Llamaste && bash scripts/host-test.sh
```

Expected: 10/10 suites pass

**Step 2: Full Buildroot rebuild**

```bash
cd /root/llamaste-build/output
make llamaste-dirclean && make llamaste
make
```

**Step 3: Update VirtualBox VM**

Flash updated image to VDI and boot. Verify:
- `audio.status` tool returns state info
- Mic button appears in web UI chat
- `/llamaste/audio/status` endpoint responds

**Step 4: Rebuild ISO**

```bash
bash /mnt/d/Llamaste/scripts/build-iso.sh /root/llamaste-build
```

**Step 5: Commit**

```bash
git add -A && git commit -m "chore: Phase 3a-1 build verification"
```

---

## Future Tasks (Phase 3a-2, 3a-3, 3a-4)

These tasks extend the foundation built in Tasks 1-11:

### Phase 3a-2: VAD + Always-Listening Pipeline
- Integrate Silero VAD ONNX model into voice.cpp
- Add ALSA capture thread (16kHz, mono, S16_LE)
- Implement VAD-gated recording (accumulate on speech, stop on silence)
- Wake phrase detection in transcription
- Connect to agent loop via command callback
- `/llamaste/audio/status` shows real-time state

### Phase 3a-3: Piper TTS Output
- Create `br2-external/package/piper-tts/` Buildroot package
- Build ONNX Runtime for musl (may need pre-built binary)
- Build espeak-ng and Piper
- Implement TTS in voice.cpp via fork/exec + stdin/stdout pipes
- ALSA playback of synthesized audio
- Wire `audio.speak` tool to Piper subprocess

### Phase 3a-4: Web UI Voice Integration
- Audio playback for LLM responses (auto-TTS toggle)
- Voice status indicator in status bar
- Push-to-talk vs always-listening toggle in UI
- WebM to WAV conversion on server side (or use WAV recording in browser)

---

## Summary

| Task | Description | Key Files |
|------|-------------|-----------|
| 1 | Enable ALSA kernel + Buildroot config | linux.config, defconfig |
| 2 | Create whisper-cpp Buildroot package | package/whisper-cpp/*.mk |
| 3 | Create voice.h header | voice.h |
| 4 | Create audio tool stubs + tests | tools_audio.cpp, test_audio_tools.cpp |
| 5 | Add HTTP audio endpoints | child_main.cpp |
| 6 | Implement WAV parsing + whisper STT | voice.cpp, CMakeLists.txt |
| 7 | Wire voice pipeline into tools | tools_audio.cpp, child_main.cpp |
| 8 | Build and test in WSL2 + QEMU | Build verification |
| 9 | Whisper model download tool | tools_audio.cpp |
| 10 | Web UI microphone button | chat.js, style.css, index.html |
| 11 | End-to-end build + VirtualBox test | Build verification |
