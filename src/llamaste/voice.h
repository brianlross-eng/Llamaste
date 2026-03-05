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
#include <cstdint>

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

    // Opaque handle (defined in voice.cpp to avoid header deps on whisper/alsa)
    struct Impl;
    Impl* impl_ = nullptr;

    void voice_thread_fn();
};

// Convert WAV file bytes to float32 PCM samples for whisper
// Handles: 16-bit PCM WAV, any sample rate (resamples to 16kHz)
bool wav_to_float32(const std::vector<uint8_t>& wav_data,
                    std::vector<float>& out_samples,
                    int target_sample_rate = 16000);
