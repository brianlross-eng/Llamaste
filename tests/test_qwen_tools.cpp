// test_qwen_tools.cpp — Unit tests for Qwen2.5 native tool call parsing
//
// Tests parse_qwen_tool_calls() which converts <tool_call>...</tool_call>
// XML blocks (Qwen2.5 native format) to OpenAI-compatible tool_calls JSON.
//
// Compile (from project root):
//   g++ -std=c++17 -I src/llamaste -pthread -o tests/build/test_qwen_tools \
//     tests/test_qwen_tools.cpp src/llamaste/agent.cpp tests/test_stubs.cpp

#include <cassert>
#include <cstdio>
#include <string>
#include "../src/llamaste/agent.h"
#include "../src/llamaste/json.hpp"

using json = nlohmann::json;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) do { \
    printf("TEST: %s ... ", name); \
} while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

static void test_plain_text_no_calls() {
    TEST("plain text returns empty array");
    auto calls = parse_qwen_tool_calls("Hello, how can I help you today?");
    if (calls.is_array() && calls.empty()) PASS();
    else FAIL("expected empty array");
}

static void test_single_call_no_args() {
    TEST("single call with empty arguments");
    std::string text =
        "Let me check that.\n"
        "<tool_call>\n"
        "{\"name\": \"system.info\", \"arguments\": {}}\n"
        "</tool_call>\n";
    auto calls = parse_qwen_tool_calls(text);
    if (!calls.is_array() || calls.size() != 1) { FAIL("expected 1 call"); return; }
    if (calls[0]["function"]["name"] != "system.info") { FAIL("wrong name"); return; }
    if (calls[0]["type"] != "function")                { FAIL("wrong type"); return; }
    if (!calls[0]["id"].is_string())                   { FAIL("missing id"); return; }
    PASS();
}

static void test_call_with_args() {
    TEST("call with arguments maps to OpenAI arguments-as-string format");
    std::string text =
        "<tool_call>\n"
        "{\"name\": \"fs.read_file\", \"arguments\": {\"path\": \"/data/notes.txt\", \"max_bytes\": 1024}}\n"
        "</tool_call>\n";
    auto calls = parse_qwen_tool_calls(text);
    if (!calls.is_array() || calls.size() != 1) { FAIL("expected 1 call"); return; }
    if (calls[0]["function"]["name"] != "fs.read_file") { FAIL("wrong name"); return; }
    // arguments must be a serialized JSON string per OpenAI spec
    if (!calls[0]["function"]["arguments"].is_string()) { FAIL("arguments not a string"); return; }
    auto args = json::parse(calls[0]["function"]["arguments"].get<std::string>(), nullptr, false);
    if (args.is_discarded())              { FAIL("arguments not valid JSON"); return; }
    if (args["path"] != "/data/notes.txt"){ FAIL("wrong path"); return; }
    if (args["max_bytes"] != 1024)        { FAIL("wrong max_bytes"); return; }
    PASS();
}

static void test_multiple_calls() {
    TEST("multiple sequential calls all parsed");
    std::string text =
        "<tool_call>\n"
        "{\"name\": \"fs.list_directory\", \"arguments\": {\"path\": \"/data\"}}\n"
        "</tool_call>\n"
        "<tool_call>\n"
        "{\"name\": \"system.info\", \"arguments\": {}}\n"
        "</tool_call>\n";
    auto calls = parse_qwen_tool_calls(text);
    if (!calls.is_array() || calls.size() != 2) { FAIL("expected 2 calls"); return; }
    if (calls[0]["function"]["name"] != "fs.list_directory") { FAIL("wrong name[0]"); return; }
    if (calls[1]["function"]["name"] != "system.info")       { FAIL("wrong name[1]"); return; }
    PASS();
}

static void test_malformed_skipped() {
    TEST("malformed JSON block skipped, valid block parsed");
    std::string text =
        "<tool_call>\nNOT VALID JSON\n</tool_call>\n"
        "<tool_call>\n{\"name\": \"system.info\", \"arguments\": {}}\n</tool_call>\n";
    auto calls = parse_qwen_tool_calls(text);
    if (!calls.is_array() || calls.size() != 1) { FAIL("expected 1 valid call"); return; }
    if (calls[0]["function"]["name"] != "system.info") { FAIL("wrong name"); return; }
    PASS();
}

static void test_inline_no_newlines() {
    TEST("inline format without surrounding newlines");
    std::string text = "<tool_call>{\"name\": \"network.status\", \"arguments\": {}}</tool_call>";
    auto calls = parse_qwen_tool_calls(text);
    if (!calls.is_array() || calls.size() != 1) { FAIL("expected 1 call"); return; }
    if (calls[0]["function"]["name"] != "network.status") { FAIL("wrong name"); return; }
    PASS();
}

static void test_missing_name_skipped() {
    TEST("missing name field skipped entirely");
    std::string text = "<tool_call>{\"arguments\": {}}</tool_call>";
    auto calls = parse_qwen_tool_calls(text);
    if (calls.is_array() && calls.empty()) PASS();
    else FAIL("expected empty array");
}

static void test_unique_ids() {
    TEST("each call gets a unique id string");
    std::string text =
        "<tool_call>{\"name\": \"tool.a\", \"arguments\": {}}</tool_call>"
        "<tool_call>{\"name\": \"tool.b\", \"arguments\": {}}</tool_call>";
    auto calls = parse_qwen_tool_calls(text);
    if (!calls.is_array() || calls.size() != 2) { FAIL("expected 2 calls"); return; }
    if (calls[0]["id"] == calls[1]["id"])        { FAIL("ids must be unique"); return; }
    if (!calls[0]["id"].is_string())             { FAIL("id[0] not string"); return; }
    if (!calls[1]["id"].is_string())             { FAIL("id[1] not string"); return; }
    PASS();
}

static void test_args_preserialized_string() {
    TEST("arguments as pre-serialized JSON string");
    // Some models emit arguments already JSON-encoded as a string
    std::string text =
        "<tool_call>"
        "{\"name\": \"fs.read_file\", \"arguments\": \"{\\\"path\\\": \\\"/data/x.txt\\\"}\"}"
        "</tool_call>";
    auto calls = parse_qwen_tool_calls(text);
    if (!calls.is_array() || calls.size() != 1) { FAIL("expected 1 call"); return; }
    auto args = json::parse(calls[0]["function"]["arguments"].get<std::string>(), nullptr, false);
    if (args.is_discarded())       { FAIL("args not valid JSON"); return; }
    if (args["path"] != "/data/x.txt") { FAIL("wrong path"); return; }
    PASS();
}

int main() {
    printf("=== Qwen2.5 Tool Call Parser Tests ===\n\n");

    test_plain_text_no_calls();
    test_single_call_no_args();
    test_call_with_args();
    test_multiple_calls();
    test_malformed_skipped();
    test_inline_no_newlines();
    test_missing_name_skipped();
    test_unique_ids();
    test_args_preserialized_string();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
