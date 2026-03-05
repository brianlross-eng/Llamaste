// voice.cpp — Voice I/O pipeline for Llamaste
//
// Phase 3a-1: WAV parsing + whisper.cpp transcription
// Phase 3a-2: ALSA capture + energy VAD + always-listening + wake phrase
// Phase 3a-3: Piper TTS subprocess (future)

#include "voice.h"
#include "json.hpp"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <fstream>
#include <thread>
#include <algorithm>
#include <chrono>

using json = nlohmann::json;

#ifdef HAVE_WHISPER
#include <whisper.h>
#endif

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
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
#ifdef HAVE_ALSA
    snd_pcm_t* capture_handle = nullptr;
#endif
    std::thread voice_thread;
    std::atomic<bool> running{false};

    // Audio buffer for accumulating speech segments
    std::vector<float> speech_buffer;

    // Energy VAD state
    int silence_frames = 0;   // consecutive silent frames
    bool in_speech = false;    // currently detecting speech
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
#ifdef HAVE_ALSA
    if (impl_->capture_handle) {
        snd_pcm_close(impl_->capture_handle);
        impl_->capture_handle = nullptr;
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
#if defined(HAVE_ALSA) && defined(HAVE_WHISPER)
    if (impl_->running.load()) return;
    if (!impl_->whisper_ctx) {
        fprintf(stderr, "[voice] Cannot start: whisper not loaded\n");
        return;
    }

    // Open ALSA capture device
    int err;
    std::string device;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        device = config_.alsa_device;
    }

    err = snd_pcm_open(&impl_->capture_handle, device.c_str(),
                        SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        last_error_ = std::string("ALSA open failed: ") + snd_strerror(err);
        fprintf(stderr, "[voice] %s\n", last_error_.c_str());
        state_.store(VoiceState::ERROR);
        return;
    }

    // Configure: 16kHz, mono, 16-bit signed LE
    snd_pcm_hw_params_t* hw_params;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(impl_->capture_handle, hw_params);
    snd_pcm_hw_params_set_access(impl_->capture_handle, hw_params,
                                  SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(impl_->capture_handle, hw_params,
                                  SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(impl_->capture_handle, hw_params, 1);
    unsigned int rate = 16000;
    snd_pcm_hw_params_set_rate_near(impl_->capture_handle, hw_params,
                                     &rate, nullptr);
    // Buffer: 480 frames = 30ms at 16kHz
    snd_pcm_uframes_t period_frames = 480;
    snd_pcm_hw_params_set_period_size_near(impl_->capture_handle, hw_params,
                                            &period_frames, nullptr);

    err = snd_pcm_hw_params(impl_->capture_handle, hw_params);
    if (err < 0) {
        last_error_ = std::string("ALSA hw_params failed: ") + snd_strerror(err);
        fprintf(stderr, "[voice] %s\n", last_error_.c_str());
        snd_pcm_close(impl_->capture_handle);
        impl_->capture_handle = nullptr;
        state_.store(VoiceState::ERROR);
        return;
    }

    snd_pcm_prepare(impl_->capture_handle);

    fprintf(stderr, "[voice] ALSA capture opened: %s @ %uHz, period=%lu frames\n",
            device.c_str(), rate, (unsigned long)period_frames);

    // Start the listening thread
    impl_->running.store(true);
    impl_->voice_thread = std::thread(&VoicePipeline::voice_thread_fn, this);

    fprintf(stderr, "[voice] Always-listening thread started\n");
    state_.store(VoiceState::LISTENING);
#else
    fprintf(stderr, "[voice] Cannot start: ALSA and/or whisper not compiled in\n");
#endif
}

void VoicePipeline::stop() {
    if (impl_->running.load()) {
        impl_->running.store(false);
        if (impl_->voice_thread.joinable()) {
            impl_->voice_thread.join();
        }
    }
#ifdef HAVE_ALSA
    if (impl_->capture_handle) {
        snd_pcm_close(impl_->capture_handle);
        impl_->capture_handle = nullptr;
    }
#endif
    fprintf(stderr, "[voice] Pipeline stopped\n");
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

// Compute RMS energy of a float audio buffer
static float compute_rms(const float* samples, size_t count) {
    if (count == 0) return 0.0f;
    double sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        sum += (double)samples[i] * samples[i];
    }
    return (float)std::sqrt(sum / count);
}

// Case-insensitive substring search
static bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    std::string h = haystack, n = needle;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    return h.find(n) != std::string::npos;
}

// Extract text after the wake phrase
static std::string extract_command(const std::string& text,
                                    const std::string& wake_phrase) {
    std::string h = text, n = wake_phrase;
    std::transform(h.begin(), h.end(), h.begin(), ::tolower);
    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
    auto pos = h.find(n);
    if (pos == std::string::npos) return text;
    std::string cmd = text.substr(pos + n.size());
    // Trim leading punctuation and whitespace
    size_t start = cmd.find_first_not_of(" \t\n\r.,!?:;");
    if (start == std::string::npos) return "";
    return cmd.substr(start);
}

void VoicePipeline::voice_thread_fn() {
#if defined(HAVE_ALSA) && defined(HAVE_WHISPER)
    // ALSA capture buffer: 480 frames = 30ms at 16kHz
    const int FRAMES_PER_READ = 480;
    const int SAMPLE_RATE = 16000;
    const float SPEECH_RMS_THRESHOLD = 0.01f;  // Adjustable energy threshold
    const int FRAMES_PER_MS = SAMPLE_RATE / 1000;

    std::vector<int16_t> pcm_buf(FRAMES_PER_READ);
    std::vector<float> float_buf(FRAMES_PER_READ);

    impl_->speech_buffer.clear();
    impl_->speech_buffer.reserve(SAMPLE_RATE * 10); // pre-alloc 10s
    impl_->silence_frames = 0;
    impl_->in_speech = false;

    VoiceConfig cfg;
    {
        std::lock_guard<std::mutex> lock(config_mutex_);
        cfg = config_;
    }

    int silence_threshold_frames = (cfg.silence_ms * FRAMES_PER_MS);
    int max_record_frames = (cfg.max_record_ms * FRAMES_PER_MS);

    fprintf(stderr, "[voice] Listening loop: silence=%dms, max=%dms, RMS=%.4f\n",
            cfg.silence_ms, cfg.max_record_ms, SPEECH_RMS_THRESHOLD);

    while (impl_->running.load()) {
        // Read 30ms of audio from ALSA
        snd_pcm_sframes_t frames = snd_pcm_readi(
            impl_->capture_handle, pcm_buf.data(), FRAMES_PER_READ);

        if (frames < 0) {
            // Handle ALSA errors (overrun, etc.)
            frames = snd_pcm_recover(impl_->capture_handle, (int)frames, 1);
            if (frames < 0) {
                fprintf(stderr, "[voice] ALSA read error: %s\n",
                        snd_strerror((int)frames));
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            continue;
        }

        if (frames == 0) continue;

        // Convert int16 to float32
        for (snd_pcm_sframes_t i = 0; i < frames; i++) {
            float_buf[i] = pcm_buf[i] / 32768.0f;
        }

        // Compute energy (RMS)
        float rms = compute_rms(float_buf.data(), (size_t)frames);
        bool is_speech = (rms > SPEECH_RMS_THRESHOLD);

        if (is_speech) {
            if (!impl_->in_speech) {
                // Speech onset
                impl_->in_speech = true;
                impl_->speech_buffer.clear();
                state_.store(VoiceState::RECORDING);
                fprintf(stderr, "[voice] Speech detected (RMS=%.4f)\n", rms);
            }
            impl_->silence_frames = 0;

            // Accumulate audio
            impl_->speech_buffer.insert(impl_->speech_buffer.end(),
                                         float_buf.begin(),
                                         float_buf.begin() + frames);
        } else if (impl_->in_speech) {
            // Still accumulate during brief silence (for natural pauses)
            impl_->speech_buffer.insert(impl_->speech_buffer.end(),
                                         float_buf.begin(),
                                         float_buf.begin() + frames);
            impl_->silence_frames += (int)frames;

            // Check if silence exceeded threshold -> end of speech
            if (impl_->silence_frames >= silence_threshold_frames) {
                // End of speech segment — run whisper
                impl_->in_speech = false;

                size_t speech_len = impl_->speech_buffer.size();
                // Require at least 0.5s of audio (8000 samples at 16kHz)
                if (speech_len < 8000) {
                    fprintf(stderr, "[voice] Speech too short (%zu samples), discarding\n",
                            speech_len);
                    state_.store(VoiceState::LISTENING);
                    continue;
                }

                fprintf(stderr, "[voice] Speech segment: %.1fs (%zu samples)\n",
                        (float)speech_len / SAMPLE_RATE, speech_len);

                // Transcribe
                std::string text = transcribe(impl_->speech_buffer);
                state_.store(VoiceState::LISTENING);

                if (text.empty()) {
                    fprintf(stderr, "[voice] Empty transcription, continuing\n");
                    continue;
                }

                // Check for wake phrase
                {
                    std::lock_guard<std::mutex> lock(config_mutex_);
                    cfg = config_;
                }

                if (contains_ci(text, cfg.wake_phrase)) {
                    std::string command = extract_command(text, cfg.wake_phrase);
                    fprintf(stderr, "[voice] Wake phrase detected! Command: \"%s\"\n",
                            command.c_str());

                    if (!command.empty() && command_callback_) {
                        state_.store(VoiceState::PROCESSING);
                        command_callback_(command);
                        state_.store(VoiceState::LISTENING);
                    }
                } else {
                    fprintf(stderr, "[voice] No wake phrase in: \"%s\"\n",
                            text.c_str());
                }
            }
        }

        // Safety: cap recording length
        if (impl_->in_speech &&
            (int)impl_->speech_buffer.size() >= max_record_frames) {
            fprintf(stderr, "[voice] Max recording length reached, processing\n");
            impl_->in_speech = false;

            std::string text = transcribe(impl_->speech_buffer);
            state_.store(VoiceState::LISTENING);

            if (!text.empty()) {
                std::lock_guard<std::mutex> lock(config_mutex_);
                cfg = config_;

                if (contains_ci(text, cfg.wake_phrase)) {
                    std::string command = extract_command(text, cfg.wake_phrase);
                    if (!command.empty() && command_callback_) {
                        state_.store(VoiceState::PROCESSING);
                        command_callback_(command);
                        state_.store(VoiceState::LISTENING);
                    }
                }
            }
        }
    }

    fprintf(stderr, "[voice] Listening loop exited\n");
#endif
}
