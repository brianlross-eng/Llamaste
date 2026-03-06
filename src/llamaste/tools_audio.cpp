// tools_audio.cpp — Audio tools for Llamaste Voice I/O
//
// Tools: audio.transcribe, audio.speak, audio.status, audio.config,
//        audio.download_model, voice.list_tts, voice.download_tts
// These provide the LLM and HTTP API with voice capabilities.

#include "tools.h"
#include "voice.h"
#include "json.hpp"
#include <string>
#include <fstream>
#include <ctime>

#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif
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
    // Report TTS engine from pipeline state
    if (g_voice) {
        result["tts_engine"] = g_voice->tts_engine_name();
        result["tts_available"] = (g_voice->tts_engine_name() != "none");
    } else {
        result["tts_available"] = false;
        result["tts_engine"] = "none";
    }
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
        if (g_voice) {
            result["tts_engine"] = g_voice->tts_engine_name();
            auto cfg2 = g_voice->config();
            result["tts_model"] = cfg2.tts_model;
        } else {
            result["tts_engine"] = "none";
            result["tts_model"] = "";
        }
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

// ── TTS voice model table ────────────────────────────────────────────────
struct TtsVoiceInfo {
    const char* name;       // Short name shown to user
    const char* onnx_file;  // Filename of the ONNX model
    const char* repo_id;    // HuggingFace repo
    int approx_mb;          // Approximate download size
    int sample_rate;        // Output sample rate
    const char* quality;    // x-low, low, medium, high
    const char* gender;     // male, female
};

static const TtsVoiceInfo TTS_VOICES[] = {
    // ── American English (en_US) ──
    {"amy-low",       "en_US-amy-low.onnx",
     "csukuangfj/vits-piper-en_US-amy-low",
     63, 16000, "low", "female"},
    {"lessac-medium", "en_US-lessac-medium.onnx",
     "csukuangfj/vits-piper-en_US-lessac-medium",
     63, 22050, "medium", "female"},
    {"lessac-high",   "en_US-lessac-high.onnx",
     "csukuangfj/vits-piper-en_US-lessac-high",
     114, 22050, "high", "female"},
    {"ryan-low",      "en_US-ryan-low.onnx",
     "csukuangfj/vits-piper-en_US-ryan-low",
     63, 16000, "low", "male"},
    {"ryan-high",     "en_US-ryan-high.onnx",
     "csukuangfj/vits-piper-en_US-ryan-high",
     121, 22050, "high", "male"},
    {"danny-low",     "en_US-danny-low.onnx",
     "csukuangfj/vits-piper-en_US-danny-low",
     63, 16000, "low", "male"},
    {"hfc_female-medium", "en_US-hfc_female-medium.onnx",
     "csukuangfj/vits-piper-en_US-hfc_female-medium",
     63, 22050, "medium", "female"},
    // ── British English (en_GB) ──
    {"alba-medium",   "en_GB-alba-medium.onnx",
     "csukuangfj/vits-piper-en_GB-alba-medium",
     63, 22050, "medium", "female"},
    {"cori-high",     "en_GB-cori-high.onnx",
     "csukuangfj/vits-piper-en_GB-cori-high",
     114, 22050, "high", "female"},
    {"alan-low",      "en_GB-alan-low.onnx",
     "csukuangfj/vits-piper-en_GB-alan-low",
     63, 16000, "low", "male"},
    {"northern_english_male-medium", "en_GB-northern_english_male-medium.onnx",
     "csukuangfj/vits-piper-en_GB-northern_english_male-medium",
     63, 22050, "medium", "male"},
    {"southern_english_female-low", "en_GB-southern_english_female-low.onnx",
     "csukuangfj/vits-piper-en_GB-southern_english_female-low",
     63, 16000, "low", "female"},
};
static const int TTS_VOICE_COUNT = sizeof(TTS_VOICES) / sizeof(TTS_VOICES[0]);

static const TtsVoiceInfo* find_tts_voice(const std::string& name) {
    for (int i = 0; i < TTS_VOICE_COUNT; i++) {
        if (name == TTS_VOICES[i].name) return &TTS_VOICES[i];
    }
    return nullptr;
}

#if !defined(_WIN32) && defined(HAVE_LIBCURL)
// Download a single file from URL to dest_path. Returns empty string on success,
// error message on failure.
static std::string download_file(const std::string& url,
                                  const std::string& dest_path) {
    std::string part_path = dest_path + ".part";

    // Resume support
    uint64_t existing_size = 0;
    struct stat st;
    if (stat(part_path.c_str(), &st) == 0) {
        existing_size = st.st_size;
    }

    FILE* fp = fopen(part_path.c_str(), existing_size > 0 ? "ab" : "wb");
    if (!fp) return "Cannot write to " + dest_path;

    CURL* curl = curl_easy_init();
    if (!curl) { fclose(fp); return "curl init failed"; }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Llamaste/1.0");
    if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0) {
        curl_easy_setopt(curl, CURLOPT_CAINFO,
                         "/etc/ssl/certs/ca-certificates.crt");
    }
    if (existing_size > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                         (curl_off_t)existing_size);
    }

    fprintf(stderr, "[tts] Downloading: %s\n", url.c_str());
    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        return std::string("Download failed: ") + curl_easy_strerror(res);
    }
    if (http_code >= 400) {
        unlink(part_path.c_str());
        return "HTTP error " + std::to_string(http_code);
    }

    // Rename .part to final
    if (rename(part_path.c_str(), dest_path.c_str()) != 0) {
        return "Failed to rename " + part_path + " to " + dest_path;
    }
    return "";  // success
}
#endif

static std::string handle_voice_download_tts(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    std::string voice_name = "amy-low";
    if (!args.is_discarded()) {
        voice_name = args.value("voice", "amy-low");
    }

    const TtsVoiceInfo* voice = find_tts_voice(voice_name);
    if (!voice) {
        json out;
        out["error"] = "Unknown voice: " + voice_name;
        json voices = json::array();
        for (int i = 0; i < TTS_VOICE_COUNT; i++) {
            json v;
            v["name"] = TTS_VOICES[i].name;
            v["size_mb"] = TTS_VOICES[i].approx_mb;
            v["quality"] = TTS_VOICES[i].quality;
            v["gender"] = TTS_VOICES[i].gender;
            voices.push_back(v);
        }
        out["available_voices"] = voices;
        return out.dump();
    }

    std::string tts_dir = "/data/models/tts";
    std::string onnx_path = tts_dir + "/" + voice->onnx_file;
    std::string tokens_path = tts_dir + "/tokens.txt";

#ifndef _WIN32
    // Check if already downloaded
    if (access(onnx_path.c_str(), R_OK) == 0 &&
        access(tokens_path.c_str(), R_OK) == 0) {
        json out;
        out["status"] = "already_downloaded";
        out["voice"] = voice_name;
        out["model_path"] = onnx_path;
        out["tokens_path"] = tokens_path;
        return out.dump();
    }

#ifdef HAVE_LIBCURL
    // Create directory
    mkdir("/data/models", 0755);
    mkdir(tts_dir.c_str(), 0755);

    // Download ONNX model
    std::string onnx_url = std::string("https://huggingface.co/") +
        voice->repo_id + "/resolve/main/" + voice->onnx_file;
    std::string err = download_file(onnx_url, onnx_path);
    if (!err.empty()) {
        json out;
        out["status"] = "error";
        out["error"] = err;
        out["file"] = voice->onnx_file;
        return out.dump();
    }

    // Download tokens.txt
    std::string tokens_url = std::string("https://huggingface.co/") +
        voice->repo_id + "/resolve/main/tokens.txt";
    err = download_file(tokens_url, tokens_path);
    if (!err.empty()) {
        json out;
        out["status"] = "error";
        out["error"] = err;
        out["file"] = "tokens.txt";
        return out.dump();
    }

    json out;
    out["status"] = "downloaded";
    out["voice"] = voice_name;
    out["model_path"] = onnx_path;
    out["tokens_path"] = tokens_path;
    out["size_mb"] = voice->approx_mb;
    out["sample_rate"] = voice->sample_rate;
    out["quality"] = voice->quality;
    out["hint"] = "Restart voice pipeline to activate neural TTS";
    return out.dump();
#else
    json out;
    out["status"] = "download_needed";
    out["voice"] = voice_name;
    out["onnx_url"] = std::string("https://huggingface.co/") +
        voice->repo_id + "/resolve/main/" + voice->onnx_file;
    out["tokens_url"] = std::string("https://huggingface.co/") +
        voice->repo_id + "/resolve/main/tokens.txt";
    out["destination"] = tts_dir;
    out["hint"] = "curl not available — download manually";
    return out.dump();
#endif // HAVE_LIBCURL
#else
    json out;
    out["status"] = "not_supported";
    out["message"] = "TTS download not supported on this platform";
    return out.dump();
#endif // _WIN32
}

static std::string handle_voice_list_tts(const std::string& /*args_json*/) {
    json voices = json::array();
    for (int i = 0; i < TTS_VOICE_COUNT; i++) {
        json v;
        v["name"] = TTS_VOICES[i].name;
        v["onnx_file"] = TTS_VOICES[i].onnx_file;
        v["size_mb"] = TTS_VOICES[i].approx_mb;
        v["sample_rate"] = TTS_VOICES[i].sample_rate;
        v["quality"] = TTS_VOICES[i].quality;
        v["gender"] = TTS_VOICES[i].gender;

        // Check if installed
        std::string path = std::string("/data/models/tts/") + TTS_VOICES[i].onnx_file;
#ifndef _WIN32
        v["installed"] = (access(path.c_str(), R_OK) == 0);
#else
        v["installed"] = false;
#endif
        voices.push_back(v);
    }

    json out;
    out["voices"] = voices;
    // Report active engine
    if (g_voice) {
        out["active_engine"] = g_voice->tts_engine_name();
    } else {
        out["active_engine"] = "none";
    }
    return out.dump();
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

    reg.register_tool({
        .name = "voice.list_tts",
        .description = "List available Piper TTS voice models with install status. "
                       "Shows voice name, quality, gender, sample rate, and download size.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_voice_list_tts
    });

    reg.register_tool({
        .name = "voice.download_tts",
        .description = "Download a Piper neural TTS voice model from HuggingFace. "
                       "Downloads ONNX model + tokens to /data/models/tts/. "
                       "Default voice: amy-low (16MB, female). "
                       "Restart voice pipeline after download to activate.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "voice": {
                    "type": "string",
                    "description": "Voice name: amy-low (16MB, female) or lessac-medium (63MB, male)",
                    "default": "amy-low"
                }
            }
        })json",
        .handler = handle_voice_download_tts
    });
}
