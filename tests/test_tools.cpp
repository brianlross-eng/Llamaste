// test_tools.cpp — Unit tests for the ToolRegistry
//
// Tests registration, dispatch, error handling, confirmation flags,
// and OpenAI-compatible JSON generation.

#include <cassert>
#include <cstdio>
#include <string>
#include "../src/llamaste/tools.h"
#include "../src/llamaste/json.hpp"

using json = nlohmann::json;

static int tests_passed = 0;

#define TEST(name) printf("TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)

int main() {
    printf("=== ToolRegistry Unit Tests ===\n\n");

    // ---------------------------------------------------------------
    // Test 1: Register a tool and verify count
    // ---------------------------------------------------------------
    TEST("register tool and count");
    {
        ToolRegistry registry;
        assert(registry.count() == 0);

        registry.register_tool({
            .name = "test.echo",
            .description = "Echo back the input",
            .parameters = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})",
            .handler = [](const std::string& args_json) -> std::string {
                return R"({"result":"echoed"})";
            }
        });

        assert(registry.count() == 1);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 2: Dispatch a registered tool
    // ---------------------------------------------------------------
    TEST("dispatch registered tool");
    {
        ToolRegistry registry;
        registry.register_tool({
            .name = "test.echo",
            .description = "Echo back the input",
            .parameters = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})",
            .handler = [](const std::string& args_json) -> std::string {
                auto args = json::parse(args_json, nullptr, false);
                std::string text = args.value("text", "default");
                json result;
                result["echoed"] = text;
                return result.dump();
            }
        });

        auto result = registry.dispatch("test.echo", R"({"text":"hello"})");
        assert(result.find("hello") != std::string::npos);

        auto parsed = json::parse(result);
        assert(parsed["echoed"] == "hello");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 3: Dispatch unknown tool returns error
    // ---------------------------------------------------------------
    TEST("dispatch unknown tool returns error");
    {
        ToolRegistry registry;
        auto result = registry.dispatch("nonexistent", "{}");
        assert(result.find("error") != std::string::npos);
        assert(result.find("unknown tool") != std::string::npos);

        auto parsed = json::parse(result);
        assert(parsed.contains("error"));
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 4: Tool that throws exception is caught
    // ---------------------------------------------------------------
    TEST("tool exception is caught");
    {
        ToolRegistry registry;
        registry.register_tool({
            .name = "test.throw",
            .description = "Always throws",
            .parameters = R"({"type":"object","properties":{}})",
            .handler = [](const std::string&) -> std::string {
                throw std::runtime_error("intentional failure");
            }
        });

        auto result = registry.dispatch("test.throw", "{}");
        assert(result.find("error") != std::string::npos);
        assert(result.find("intentional failure") != std::string::npos);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 5: to_openai_tools_json generates valid JSON
    // ---------------------------------------------------------------
    TEST("to_openai_tools_json generates valid JSON");
    {
        ToolRegistry registry;
        registry.register_tool({
            .name = "test.alpha",
            .description = "First tool",
            .parameters = R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})",
            .handler = [](const std::string&) -> std::string { return "{}"; }
        });
        registry.register_tool({
            .name = "test.beta",
            .description = "Second tool",
            .parameters = R"({"type":"object","properties":{}})",
            .handler = [](const std::string&) -> std::string { return "{}"; }
        });

        auto tools_json = registry.to_openai_tools_json();
        auto parsed = json::parse(tools_json);

        assert(parsed.is_array());
        assert(parsed.size() == 2);

        // Should be sorted alphabetically
        assert(parsed[0]["function"]["name"] == "test.alpha");
        assert(parsed[1]["function"]["name"] == "test.beta");

        // Check structure
        assert(parsed[0]["type"] == "function");
        assert(parsed[0]["function"]["description"] == "First tool");
        assert(parsed[0]["function"]["parameters"]["type"] == "object");
        assert(parsed[0]["function"]["parameters"]["required"][0] == "x");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 6: tool_names returns sorted list
    // ---------------------------------------------------------------
    TEST("tool_names returns sorted list");
    {
        ToolRegistry registry;
        auto noop = [](const std::string&) -> std::string { return "{}"; };

        registry.register_tool({.name = "z.tool", .description = "", .parameters = "{}", .handler = noop});
        registry.register_tool({.name = "a.tool", .description = "", .parameters = "{}", .handler = noop});
        registry.register_tool({.name = "m.tool", .description = "", .parameters = "{}", .handler = noop});

        auto names = registry.tool_names();
        assert(names.size() == 3);
        assert(names[0] == "a.tool");
        assert(names[1] == "m.tool");
        assert(names[2] == "z.tool");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 7: needs_confirmation flag
    // ---------------------------------------------------------------
    TEST("needs_confirmation flag");
    {
        ToolRegistry registry;
        auto noop = [](const std::string&) -> std::string { return "{}"; };

        registry.register_tool({
            .name = "safe.tool",
            .description = "Safe",
            .parameters = "{}",
            .handler = noop,
            .requires_confirmation = false
        });
        registry.register_tool({
            .name = "dangerous.tool",
            .description = "Dangerous",
            .parameters = "{}",
            .handler = noop,
            .requires_confirmation = true
        });

        assert(!registry.needs_confirmation("safe.tool"));
        assert(registry.needs_confirmation("dangerous.tool"));
        assert(!registry.needs_confirmation("nonexistent"));
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 8: Register overwrites existing tool
    // ---------------------------------------------------------------
    TEST("register overwrites existing tool");
    {
        ToolRegistry registry;
        registry.register_tool({
            .name = "test.dup",
            .description = "Version 1",
            .parameters = "{}",
            .handler = [](const std::string&) -> std::string { return R"({"v":1})"; }
        });
        registry.register_tool({
            .name = "test.dup",
            .description = "Version 2",
            .parameters = "{}",
            .handler = [](const std::string&) -> std::string { return R"({"v":2})"; }
        });

        assert(registry.count() == 1);
        auto result = registry.dispatch("test.dup", "{}");
        assert(result.find("\"v\":2") != std::string::npos);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 9: Handler receives correct arguments
    // ---------------------------------------------------------------
    TEST("handler receives correct arguments");
    {
        ToolRegistry registry;
        registry.register_tool({
            .name = "test.add",
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

        auto result = registry.dispatch("test.add", R"({"a":3,"b":7})");
        auto parsed = json::parse(result);
        assert(parsed["sum"] == 10);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 10: Malformed parameters schema handled gracefully
    // ---------------------------------------------------------------
    TEST("malformed parameters schema handled in to_openai_tools_json");
    {
        ToolRegistry registry;
        registry.register_tool({
            .name = "test.bad_schema",
            .description = "Has bad schema",
            .parameters = "not valid json {{{",
            .handler = [](const std::string&) -> std::string { return "{}"; }
        });

        // Should not crash — falls back to empty object schema
        auto tools_json = registry.to_openai_tools_json();
        auto parsed = json::parse(tools_json);
        assert(parsed.is_array());
        assert(parsed.size() == 1);
        assert(parsed[0]["function"]["parameters"]["type"] == "object");
        PASS();
    }

    printf("\n=== All %d ToolRegistry tests passed. ===\n", tests_passed);
    return 0;
}
