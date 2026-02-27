// test_agent.cpp — Unit tests for the agent loop, conversation state, and prompt builder
//
// Tests parse_tool_calls, ConversationState, build_inference_request,
// build_system_prompt, and agent_turn with mocked inference functions.

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/llamaste/agent.h"
#include "../src/llamaste/prompt_builder.h"
#include "../src/llamaste/json.hpp"

using json = nlohmann::json;

static int tests_passed = 0;

#define TEST(name) printf("TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)

// Helper: build a mock chat completion response with tool calls
static std::string make_tool_call_response(
    const std::string& call_id,
    const std::string& tool_name,
    const std::string& arguments
) {
    json response;
    json choice;
    json message;
    message["role"] = "assistant";
    message["content"] = nullptr;

    json tc;
    tc["id"] = call_id;
    tc["type"] = "function";
    json func;
    func["name"] = tool_name;
    func["arguments"] = arguments;
    tc["function"] = func;

    message["tool_calls"] = json::array({tc});
    choice["message"] = message;
    choice["finish_reason"] = "tool_calls";
    response["choices"] = json::array({choice});

    return response.dump();
}

// Helper: build a mock chat completion response with text content only
static std::string make_text_response(const std::string& content) {
    json response;
    json choice;
    json message;
    message["role"] = "assistant";
    message["content"] = content;
    choice["message"] = message;
    choice["finish_reason"] = "stop";
    response["choices"] = json::array({choice});

    return response.dump();
}

int main() {
    printf("=== Agent & Prompt Builder Unit Tests ===\n\n");

    // ---------------------------------------------------------------
    // Test 1: parse_tool_calls with a valid response containing tool calls
    // ---------------------------------------------------------------
    TEST("parse_tool_calls with valid tool calls");
    {
        std::string response = make_tool_call_response(
            "call_001", "fs.read_file", R"({"path":"/data/test.txt"})"
        );

        auto calls = parse_tool_calls(response);
        assert(calls.size() == 1);
        assert(calls[0].id == "call_001");
        assert(calls[0].name == "fs.read_file");
        assert(calls[0].arguments.find("/data/test.txt") != std::string::npos);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 2: parse_tool_calls with no tool calls (text response)
    // ---------------------------------------------------------------
    TEST("parse_tool_calls with no tool calls returns empty");
    {
        std::string response = make_text_response("Hello, how can I help?");

        auto calls = parse_tool_calls(response);
        assert(calls.empty());

        // Also verify parse_content works
        std::string content = parse_content(response);
        assert(content == "Hello, how can I help?");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 3: ConversationState — add messages and to_messages_json
    // ---------------------------------------------------------------
    TEST("ConversationState add messages and to_messages_json");
    {
        ConversationState conv;
        conv.system_prompt = "You are a helpful assistant.";

        conv.add_user_message("What files are in /data?");
        conv.add_assistant_message("Let me check that for you.");

        assert(conv.message_count() == 2);

        auto msgs = conv.to_messages_json();
        assert(msgs.is_array());
        assert(msgs.size() == 3);  // system + user + assistant

        // System message first
        assert(msgs[0]["role"] == "system");
        assert(msgs[0]["content"] == "You are a helpful assistant.");

        // User message
        assert(msgs[1]["role"] == "user");
        assert(msgs[1]["content"] == "What files are in /data?");

        // Assistant message
        assert(msgs[2]["role"] == "assistant");
        assert(msgs[2]["content"] == "Let me check that for you.");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 3b: ConversationState — tool call messages in JSON
    // ---------------------------------------------------------------
    TEST("ConversationState tool call messages in JSON");
    {
        ConversationState conv;
        conv.system_prompt = "System.";

        conv.add_user_message("List files.");

        // Assistant responds with tool call
        std::vector<ToolCall> calls;
        calls.push_back({"call_abc", "fs.list_directory", R"({"path":"/data"})"});
        conv.add_assistant_tool_calls(calls);

        // Tool result
        conv.add_tool_result("call_abc", "fs.list_directory", R"({"entries":[]})");

        auto msgs = conv.to_messages_json();
        assert(msgs.size() == 4);  // system + user + assistant(tool_calls) + tool

        // Check assistant message has tool_calls
        auto& asst_msg = msgs[2];
        assert(asst_msg["role"] == "assistant");
        assert(asst_msg["content"].is_null());
        assert(asst_msg["tool_calls"].is_array());
        assert(asst_msg["tool_calls"].size() == 1);
        assert(asst_msg["tool_calls"][0]["id"] == "call_abc");
        assert(asst_msg["tool_calls"][0]["type"] == "function");
        assert(asst_msg["tool_calls"][0]["function"]["name"] == "fs.list_directory");

        // Check tool result message
        auto& tool_msg = msgs[3];
        assert(tool_msg["role"] == "tool");
        assert(tool_msg["tool_call_id"] == "call_abc");
        assert(tool_msg["name"] == "fs.list_directory");
        assert(tool_msg["content"].get<std::string>().find("entries") != std::string::npos);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 4: ConversationState — serialize and deserialize round-trip
    // ---------------------------------------------------------------
    TEST("ConversationState serialize/deserialize round-trip");
    {
        ConversationState original;
        original.system_prompt = "You are Llamaste.";
        original.conversation_id = "conv-123";

        original.add_user_message("Hello");
        original.add_assistant_message("Hi there!");
        original.add_user_message("List files");

        std::vector<ToolCall> calls;
        calls.push_back({"call_xyz", "fs.list_directory", R"({"path":"/data"})"});
        original.add_assistant_tool_calls(calls);
        original.add_tool_result("call_xyz", "fs.list_directory", R"({"count":3})");
        original.add_assistant_message("There are 3 items in /data.");

        std::string serialized = original.serialize();

        // Verify it's valid JSON
        auto parsed = json::parse(serialized);
        assert(!parsed.is_discarded());
        assert(parsed["system_prompt"] == "You are Llamaste.");
        assert(parsed["conversation_id"] == "conv-123");

        // Deserialize and verify
        auto restored = ConversationState::deserialize(serialized);
        assert(restored.system_prompt == "You are Llamaste.");
        assert(restored.conversation_id == "conv-123");
        assert(restored.message_count() == 6);

        // Verify messages match
        const auto& msgs = restored.messages();
        assert(msgs[0].role == "user");
        assert(msgs[0].content == "Hello");
        assert(msgs[1].role == "assistant");
        assert(msgs[1].content == "Hi there!");
        assert(msgs[3].role == "assistant");
        assert(msgs[3].tool_calls.size() == 1);
        assert(msgs[3].tool_calls[0].id == "call_xyz");
        assert(msgs[3].tool_calls[0].name == "fs.list_directory");
        assert(msgs[4].role == "tool");
        assert(msgs[4].tool_call_id == "call_xyz");
        assert(msgs[4].name == "fs.list_directory");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 5: ConversationState — truncate keeps recent messages
    // ---------------------------------------------------------------
    TEST("ConversationState truncate keeps recent messages");
    {
        ConversationState conv;
        conv.system_prompt = "System prompt.";

        // Add 10 messages
        for (int i = 0; i < 5; i++) {
            conv.add_user_message("Question " + std::to_string(i));
            conv.add_assistant_message("Answer " + std::to_string(i));
        }
        assert(conv.message_count() == 10);

        // Truncate to keep last 4
        conv.truncate(4);
        assert(conv.message_count() == 4);

        // Verify the kept messages are the most recent
        const auto& msgs = conv.messages();
        assert(msgs[0].content == "Question 3");
        assert(msgs[1].content == "Answer 3");
        assert(msgs[2].content == "Question 4");
        assert(msgs[3].content == "Answer 4");

        // System prompt should still be in to_messages_json
        auto json_msgs = conv.to_messages_json();
        assert(json_msgs[0]["role"] == "system");
        assert(json_msgs[0]["content"] == "System prompt.");
        assert(json_msgs.size() == 5);  // system + 4 messages
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 5b: ConversationState — truncate with max >= size is no-op
    // ---------------------------------------------------------------
    TEST("ConversationState truncate no-op when under limit");
    {
        ConversationState conv;
        conv.add_user_message("Hello");
        conv.add_assistant_message("Hi");

        conv.truncate(10);
        assert(conv.message_count() == 2);

        conv.truncate(2);
        assert(conv.message_count() == 2);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 6: build_inference_request — verify JSON structure
    // ---------------------------------------------------------------
    TEST("build_inference_request generates valid JSON with model, messages, tools");
    {
        ConversationState conv;
        conv.system_prompt = "You are helpful.";
        conv.add_user_message("Hi");

        ToolRegistry tools;
        tools.register_tool({
            .name = "test.echo",
            .description = "Echo input",
            .parameters = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})",
            .handler = [](const std::string& args) -> std::string { return args; }
        });

        std::string request = build_inference_request(conv, tools, 1024, 0.5);
        auto parsed = json::parse(request);

        assert(parsed.contains("model"));
        assert(parsed["model"] == "llamaste");

        assert(parsed.contains("messages"));
        assert(parsed["messages"].is_array());
        assert(parsed["messages"].size() == 2);  // system + user
        assert(parsed["messages"][0]["role"] == "system");
        assert(parsed["messages"][1]["role"] == "user");

        assert(parsed.contains("tools"));
        assert(parsed["tools"].is_array());
        assert(parsed["tools"].size() == 1);
        assert(parsed["tools"][0]["type"] == "function");
        assert(parsed["tools"][0]["function"]["name"] == "test.echo");

        assert(parsed["max_tokens"] == 1024);
        // Float comparison
        assert(parsed["temperature"].get<double>() > 0.4 && parsed["temperature"].get<double>() < 0.6);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 7: build_system_prompt — verify hardware, tools, mode
    // ---------------------------------------------------------------
    TEST("build_system_prompt includes CPU model, tool names, boot mode");
    {
        HardwareInfo hw;
        hw.cpu_model = "AMD Ryzen 7 5800X";
        hw.cpu_cores = 16;
        hw.ram_total_mb = 32768;
        hw.ram_free_mb = 24576;
        hw.gpu_detected = true;
        hw.gpu_name = "NVIDIA";
        hw.has_avx2 = true;

        ToolRegistry tools;
        auto noop = [](const std::string&) -> std::string { return "{}"; };
        tools.register_tool({.name = "fs.read_file", .description = "Read", .parameters = "{}", .handler = noop});
        tools.register_tool({.name = "system.info", .description = "Info", .parameters = "{}", .handler = noop});
        tools.register_tool({.name = "network.status", .description = "Net", .parameters = "{}", .handler = noop});

        std::string prompt = build_system_prompt(hw, tools, "server");

        // Check identity
        assert(prompt.find("Llamaste") != std::string::npos);
        assert(prompt.find("operating system") != std::string::npos);

        // Check hardware
        assert(prompt.find("AMD Ryzen 7 5800X") != std::string::npos);
        assert(prompt.find("16") != std::string::npos);     // cores
        assert(prompt.find("32768") != std::string::npos);   // total RAM
        assert(prompt.find("24576") != std::string::npos);   // free RAM
        assert(prompt.find("NVIDIA") != std::string::npos);
        assert(prompt.find("AVX2") != std::string::npos);

        // Check boot mode
        assert(prompt.find("SERVER") != std::string::npos);

        // Check tools are listed
        assert(prompt.find("fs.read_file") != std::string::npos);
        assert(prompt.find("system.info") != std::string::npos);
        assert(prompt.find("network.status") != std::string::npos);

        // Check rules
        assert(prompt.find("confirmation") != std::string::npos ||
               prompt.find("Confirmation") != std::string::npos);
        assert(prompt.find("/data") != std::string::npos);

        // Test desktop mode
        std::string desktop_prompt = build_system_prompt(hw, tools, "desktop");
        assert(desktop_prompt.find("DESKTOP") != std::string::npos);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 8: agent_turn — simple text response (no tool calls)
    // ---------------------------------------------------------------
    TEST("agent_turn with simple text response (no tool calls)");
    {
        ConversationState conv;
        conv.system_prompt = "You are helpful.";
        conv.add_user_message("Hello");

        ToolRegistry tools;

        // Mock inference that returns a simple text response
        auto mock_inference = [](const std::string& request) -> std::string {
            return make_text_response("Hello! How can I help you today?");
        };

        std::string result = agent_turn(conv, tools, mock_inference);
        assert(result == "Hello! How can I help you today?");

        // Conversation should have the assistant message added
        assert(conv.message_count() == 2);  // user + assistant
        const auto& msgs = conv.messages();
        assert(msgs[1].role == "assistant");
        assert(msgs[1].content == "Hello! How can I help you today?");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 9: agent_turn — tool call then text response
    // ---------------------------------------------------------------
    TEST("agent_turn with tool call then text response");
    {
        ConversationState conv;
        conv.system_prompt = "You are helpful.";
        conv.add_user_message("What is 3 + 7?");

        ToolRegistry tools;
        tools.register_tool({
            .name = "math.add",
            .description = "Add two numbers",
            .parameters = R"({"type":"object","properties":{"a":{"type":"integer"},"b":{"type":"integer"}},"required":["a","b"]})",
            .handler = [](const std::string& args_json) -> std::string {
                auto args = json::parse(args_json);
                int a = args["a"];
                int b = args["b"];
                json result;
                result["sum"] = a + b;
                return result.dump();
            }
        });

        int call_count = 0;
        auto mock_inference = [&call_count](const std::string& request) -> std::string {
            call_count++;
            if (call_count == 1) {
                // First call: LLM decides to use a tool
                return make_tool_call_response(
                    "call_add_1", "math.add", R"({"a":3,"b":7})"
                );
            } else {
                // Second call: LLM sees the tool result and responds with text
                return make_text_response("The sum of 3 and 7 is 10.");
            }
        };

        std::string result = agent_turn(conv, tools, mock_inference);
        assert(result == "The sum of 3 and 7 is 10.");
        assert(call_count == 2);

        // Verify conversation structure:
        // user -> assistant(tool_calls) -> tool -> assistant(text)
        assert(conv.message_count() == 4);
        const auto& msgs = conv.messages();

        assert(msgs[0].role == "user");
        assert(msgs[1].role == "assistant");
        assert(msgs[1].tool_calls.size() == 1);
        assert(msgs[1].tool_calls[0].name == "math.add");
        assert(msgs[2].role == "tool");
        assert(msgs[2].tool_call_id == "call_add_1");
        assert(msgs[2].name == "math.add");
        // Verify the tool was actually dispatched and got the right result
        auto tool_result = json::parse(msgs[2].content);
        assert(tool_result["sum"] == 10);
        assert(msgs[3].role == "assistant");
        assert(msgs[3].content == "The sum of 3 and 7 is 10.");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 10: agent_turn — max rounds limit
    // ---------------------------------------------------------------
    TEST("agent_turn respects max_rounds limit");
    {
        ConversationState conv;
        conv.system_prompt = "System.";
        conv.add_user_message("loop forever");

        ToolRegistry tools;
        tools.register_tool({
            .name = "test.noop",
            .description = "Does nothing",
            .parameters = R"({"type":"object","properties":{}})",
            .handler = [](const std::string&) -> std::string { return R"({"ok":true})"; }
        });

        int call_count = 0;
        auto mock_inference = [&call_count](const std::string& request) -> std::string {
            call_count++;
            // Always return a tool call, never text
            return make_tool_call_response("call_" + std::to_string(call_count), "test.noop", "{}");
        };

        std::string result = agent_turn(conv, tools, mock_inference, 3);
        assert(call_count == 3);
        assert(result.find("maximum") != std::string::npos);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 11: parse_tool_calls with multiple tool calls
    // ---------------------------------------------------------------
    TEST("parse_tool_calls with multiple tool calls");
    {
        json response;
        json choice;
        json message;
        message["role"] = "assistant";
        message["content"] = nullptr;

        json tc1;
        tc1["id"] = "call_1";
        tc1["type"] = "function";
        tc1["function"] = {{"name", "fs.read_file"}, {"arguments", R"({"path":"/data/a.txt"})"}};

        json tc2;
        tc2["id"] = "call_2";
        tc2["type"] = "function";
        tc2["function"] = {{"name", "fs.read_file"}, {"arguments", R"({"path":"/data/b.txt"})"}};

        message["tool_calls"] = json::array({tc1, tc2});
        choice["message"] = message;
        response["choices"] = json::array({choice});

        auto calls = parse_tool_calls(response.dump());
        assert(calls.size() == 2);
        assert(calls[0].id == "call_1");
        assert(calls[0].name == "fs.read_file");
        assert(calls[1].id == "call_2");
        assert(calls[1].name == "fs.read_file");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 12: parse_tool_calls with invalid/empty JSON
    // ---------------------------------------------------------------
    TEST("parse_tool_calls with invalid JSON returns empty");
    {
        auto calls1 = parse_tool_calls("");
        assert(calls1.empty());

        auto calls2 = parse_tool_calls("not json at all");
        assert(calls2.empty());

        auto calls3 = parse_tool_calls("{}");
        assert(calls3.empty());

        auto calls4 = parse_tool_calls(R"({"choices":[]})");
        assert(calls4.empty());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 13: ConversationState with empty system prompt
    // ---------------------------------------------------------------
    TEST("ConversationState with empty system prompt");
    {
        ConversationState conv;
        // No system prompt set
        conv.add_user_message("Hello");

        auto msgs = conv.to_messages_json();
        // Should NOT include a system message
        assert(msgs.size() == 1);
        assert(msgs[0]["role"] == "user");
        PASS();
    }

    printf("\n=== All %d Agent & Prompt Builder tests passed. ===\n", tests_passed);
    return 0;
}
