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
#pragma pack(push, 1)
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
#pragma pack(pop)

bool wav_to_float32(const std::vector<uint8_t>& wav_data,
                    std::vector<float>& out_samples,
                    int target_sample_rate) {
    if (wav_data.size() < 44) return false;

    // Verify RIFF/WAVE header
    if (memcmp(wav_data.data(), "RIFF", 4) != 0 ||
        memcmp(wav_data.data() + 8, "WAVE", 4) != 0) {
        return false;
    }

    // Parse fmt chunk
    const auto* hdr = reinterpret_cast<const WavHeader*>(wav_data.data());
    if (hdr->audio_format != 1) {
        fprintf(stderr, "[voice] WAV: only PCM format supported (got %d)\n",
                hdr->audio_format);
        return false;
    }

    int channels = hdr->num_channels;
    int bps = hdr->bits_per_sample;
    int src_rate = hdr->sample_rate;

    // Find data chunk (skip past fmt and any extra chunks)
    size_t pos = 12; // after RIFF header
    size_t data_offset = 0;
    uint32_t data_size = 0;

    while (pos + 8 <= wav_data.size()) {
        uint32_t chunk_size;
        memcpy(&chunk_size, wav_data.data() + pos + 4, 4);

        if (memcmp(wav_data.data() + pos, "data", 4) == 0) {
            data_offset = pos + 8;
            data_size = chunk_size;
            break;
        }
        pos += 8 + chunk_size;
        // Align to even boundary
        if (pos % 2 != 0) pos++;
    }

    if (data_offset == 0 || data_size == 0) {
        fprintf(stderr, "[voice] WAV: data chunk not found\n");
        return false;
    }

    // Ensure we don't read past the buffer
    if (data_offset + data_size > wav_data.size()) {
        data_size = (uint32_t)(wav_data.size() - data_offset);
    }

    // Convert to float32 mono
    size_t bytes_per_sample = bps / 8;
    size_t num_samples = data_size / bytes_per_sample / channels;
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
    if (src_rate != target_sample_rate && src_rate > 0) {
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
            } else if (idx < num_samples) {
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
        impl_->whisper_ctx = nullptr;
    }
#endif
    delete impl_;
}

bool VoicePipeline::init(const VoiceConfig& config) {
    std::lock_guard<std::mutex> lock(config_mutex_);
    config_ = config;

    if (!config_.enabled) {
        state_.store(VoiceState::DISABLED);
        return true;
    }

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

    state_.store(VoiceState::TRANSCRIBING);

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
        state_.store(VoiceState::LISTENING);
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
    if (start != std::string::npos && end != std::string::npos) {
        text = text.substr(start, end - start + 1);
    } else if (start == std::string::npos) {
        text.clear();
    }

    fprintf(stderr, "[voice] Transcription: \"%s\"\n", text.c_str());
    state_.store(VoiceState::LISTENING);
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
    // TODO: Phase 3a-2 — always-listening thread with ALSA + VAD
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
