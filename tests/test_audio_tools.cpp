// test_audio_tools.cpp — Unit tests for audio.* tools
//
// Tests tool registration, parameter validation, and stub behavior
// (whisper/piper not loaded during host tests).

#include <cassert>
#include <cstdio>
#include <cstring>
#include "../src/llamaste/tools.h"
#include "../src/llamaste/json.hpp"

using json = nlohmann::json;

static int tests_passed = 0;
static int tests_total = 0;

#define TEST(name) do { printf("TEST: %s ... ", name); tests_total++; } while(0)
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
        bool has_config = false, has_download = false;
        for (const auto& n : names) {
            if (n == "audio.transcribe") has_transcribe = true;
            if (n == "audio.speak") has_speak = true;
            if (n == "audio.status") has_status = true;
            if (n == "audio.config") has_config = true;
            if (n == "audio.download_model") has_download = true;
        }
        assert(has_transcribe);
        assert(has_speak);
        assert(has_status);
        assert(has_config);
        assert(has_download);
        PASS();
    }

    // Test 2: audio.status returns valid JSON with state
    TEST("audio.status returns state");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.status", "{}");
        auto j = json::parse(result);
        assert(j.contains("state"));
        assert(j["state"] == "disabled");
        assert(j.contains("enabled"));
        assert(j["enabled"] == false);
        PASS();
    }

    // Test 3: audio.transcribe with no audio file returns error
    TEST("audio.transcribe with no data returns error");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.transcribe", R"json({})json");
        auto j = json::parse(result);
        assert(j.contains("error"));
        PASS();
    }

    // Test 4: audio.transcribe rejects paths outside /data/
    TEST("audio.transcribe rejects paths outside /data/");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.transcribe",
            R"json({"audio_file": "/etc/passwd"})json");
        auto j = json::parse(result);
        assert(j.contains("error"));
        assert(j["error"].get<std::string>().find("/data/") != std::string::npos);
        PASS();
    }

    // Test 5: audio.speak with empty text returns error
    TEST("audio.speak with empty text returns error");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.speak", R"json({"text": ""})json");
        auto j = json::parse(result);
        assert(j.contains("error"));
        PASS();
    }

    // Test 6: audio.config returns valid config
    TEST("audio.config returns config");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.config", "{}");
        auto j = json::parse(result);
        assert(j.contains("wake_phrase"));
        assert(j["wake_phrase"] == "llamaste");
        assert(j.contains("sample_rate"));
        assert(j["sample_rate"] == 16000);
        PASS();
    }

    // Test 7: audio.download_model returns info for tiny.en
    TEST("audio.download_model returns tiny.en info");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.download_model",
            R"json({"model": "tiny.en"})json");
        auto j = json::parse(result);
        assert(j.contains("url") || j.contains("status"));
        // Either "download_needed" or "already_downloaded"
        if (j.contains("status")) {
            std::string st = j["status"];
            assert(st == "download_needed" || st == "already_downloaded");
        }
        PASS();
    }

    // Test 8: audio.download_model rejects unknown model
    TEST("audio.download_model rejects unknown model");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.download_model",
            R"json({"model": "nonexistent"})json");
        auto j = json::parse(result);
        assert(j.contains("error"));
        PASS();
    }

    // Test 9: audio.transcribe with voice pipeline disabled
    TEST("audio.transcribe returns error when pipeline disabled");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        std::string result = reg.dispatch("audio.transcribe",
            R"json({"audio_file": "/data/test.wav"})json");
        auto j = json::parse(result);
        assert(j.contains("error"));
        assert(j["error"].get<std::string>().find("not initialized") != std::string::npos);
        PASS();
    }

    // Test 10: Tool count
    TEST("5 audio tools registered");
    {
        ToolRegistry reg;
        register_audio_tools(reg);
        assert(reg.count() == 5);
        PASS();
    }

    printf("\n=== SUMMARY ===\n");
    printf("Tests passed: %d/%d\n", tests_passed, tests_total);
    return tests_passed == tests_total ? 0 : 1;
}
