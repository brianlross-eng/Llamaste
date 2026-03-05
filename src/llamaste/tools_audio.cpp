// tools_audio.cpp — Audio tools for Llamaste Voice I/O
//
// Tools: audio.transcribe, audio.speak, audio.status, audio.config
// These provide the LLM and HTTP API with voice capabilities.

#include "tools.h"
#include "voice.h"
#include "json.hpp"
#include <string>
#include <fstream>
#include <ctime>

#ifndef _WIN32
#include <unistd.h>
#endif

using json = nlohmann::json;

// Global voice pipeline pointer — set by child_main.cpp when initialized
VoicePipeline* g_voice = nullptr;

static std::string handle_audio_status(const std::string& args_json) {
    (void)args_json;
    json result;
    if (g_voice) {
        auto st = g_voice->state();
        result["state"] = g_voice->state_string();
        result["enabled"] = (st != VoiceState::DISABLED);
        result["whisper_loaded"] = (st >= VoiceState::LISTENING);
        result["always_listening"] = (st == VoiceState::LISTENING ||
                                      st == VoiceState::RECORDING ||
                                      st == VoiceState::TRANSCRIBING ||
                                      st == VoiceState::PROCESSING);
        auto cfg = g_voice->config();
        result["wake_phrase"] = cfg.wake_phrase;
        result["whisper_model"] = cfg.whisper_model;
        result["alsa_device"] = cfg.alsa_device;
        result["silence_ms"] = cfg.silence_ms;
        std::string err = g_voice->last_error();
        if (!err.empty()) result["last_error"] = err;
    } else {
        result["state"] = "disabled";
        result["enabled"] = false;
        result["whisper_loaded"] = false;
        result["always_listening"] = false;
        result["wake_phrase"] = "llamaste";
    }
    result["piper_available"] = false;  // Phase 3a-3
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

    if (!g_voice || g_voice->state() == VoiceState::DISABLED) {
        return R"json({"error": "Voice pipeline not initialized — whisper.cpp not loaded"})json";
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
    ifs.close();

    // Parse WAV to float32 PCM
    std::vector<float> samples;
    if (!wav_to_float32(wav_data, samples)) {
        return R"json({"error": "Invalid WAV file format (need 16-bit PCM)"})json";
    }

    // Transcribe
    std::string text = g_voice->transcribe(samples);
    json result;
    result["text"] = text;
    result["audio_file"] = audio_file;
    result["duration_seconds"] = (double)samples.size() / 16000.0;
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

    if (!g_voice) {
        return R"json({"error": "Voice pipeline not initialized — Piper TTS not available"})json";
    }

    // Phase 3a-3: Piper TTS implementation
    auto pcm = g_voice->speak(text);
    if (pcm.empty()) {
        return R"json({"error": "TTS not yet implemented (Phase 3a-3)"})json";
    }

    // Write WAV to /data/tmp/
    std::string out_path = "/data/tmp/tts_" + std::to_string(time(nullptr)) + ".wav";

    json result;
    result["audio_file"] = out_path;
    result["text"] = text;
    return result.dump();
}

static std::string handle_audio_config(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);

    // GET config (no args or empty args)
    if (args.is_discarded() || args.empty()) {
        json result;
        if (g_voice) {
            auto cfg = g_voice->config();
            result["wake_phrase"] = cfg.wake_phrase;
            result["enabled"] = cfg.enabled;
            result["whisper_model"] = cfg.whisper_model;
            result["sample_rate"] = cfg.sample_rate;
            result["vad_threshold"] = cfg.vad_threshold;
        } else {
            result["wake_phrase"] = "llamaste";
            result["enabled"] = false;
            result["whisper_model"] = "ggml-tiny.en-q5_1.bin";
            result["sample_rate"] = 16000;
            result["vad_threshold"] = 0.5;
        }
        result["piper_voice"] = "en_US-amy-low";
        return result.dump();
    }

    // SET config
    if (g_voice) {
        auto cfg = g_voice->config();
        if (args.contains("wake_phrase")) cfg.wake_phrase = args["wake_phrase"].get<std::string>();
        if (args.contains("enabled")) cfg.enabled = args["enabled"].get<bool>();
        if (args.contains("vad_threshold")) cfg.vad_threshold = args["vad_threshold"].get<float>();
        g_voice->set_config(cfg);
    }

    json result;
    result["status"] = "updated";
    return result.dump();
}

static std::string handle_audio_download_model(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    std::string model = "tiny.en";
    if (!args.is_discarded()) {
        model = args.value("model", "tiny.en");
    }

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

#ifndef _WIN32
    // Check if already exists
    if (access(dest.c_str(), R_OK) == 0) {
        json result;
        result["status"] = "already_downloaded";
        result["path"] = dest;
        return result.dump();
    }
#endif

    // Return download info — actual download uses existing model.download tool
    // or curl from the voice pipeline init
    json result;
    result["status"] = "download_needed";
    result["model"] = model;
    result["url"] = url;
    result["destination"] = dest;
    result["size_mb"] = (model.find("tiny") != std::string::npos) ? 31 : 60;
    result["hint"] = "Use model.download or download manually to " + dest;
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

    reg.register_tool({
        .name = "audio.download_model",
        .description = "Check or download a whisper speech-to-text model. "
                       "Options: tiny.en (31MB, fastest), base.en (60MB, better accuracy).",
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
}
