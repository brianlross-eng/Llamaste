// test_tools_integration.cpp — Integration tests for all Llamaste tools
//
// Registers all tools and verifies:
// 1. Expected tool count matches
// 2. All tools are callable (dispatch returns valid JSON, not crash)
// 3. Tools that read from /proc or /sys work on the host
// 4. Filesystem tools enforce /data path restrictions
// 5. Confirmation flags are set on dangerous tools
// 6. OpenAI tools JSON is valid and complete

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include "../src/llamaste/tools.h"
#include "../src/llamaste/json.hpp"

using json = nlohmann::json;

static int tests_passed = 0;

#define TEST(name) printf("TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)

// Create /data directory for testing if it doesn't exist
static void setup_test_data_dir() {
    mkdir("/data", 0755);
    mkdir("/data/llamaste", 0755);
    mkdir("/data/llamaste/config", 0755);
    mkdir("/data/models", 0755);
    mkdir("/data/testdir", 0755);

    // Create a test file
    FILE* f = fopen("/data/testdir/hello.txt", "w");
    if (f) {
        fprintf(f, "Hello from Llamaste tests!\n");
        fclose(f);
    }
}

static void cleanup_test_data() {
    unlink("/data/testdir/hello.txt");
    unlink("/data/testdir/written.txt");
    rmdir("/data/testdir");
    // Leave /data/llamaste/config for config tests
}

int main() {
    printf("=== Tools Integration Tests ===\n\n");

    setup_test_data_dir();

    // ---------------------------------------------------------------
    // Test 1: Register all tools and verify count
    // ---------------------------------------------------------------
    TEST("register all tools");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        int count = registry.count();
        printf("(count=%d) ", count);

        // Expected: fs(6) + process(2) + network(4) + system(6) + config(4) + model(3) = 25
        assert(count == 25);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 2: All tool names present
    // ---------------------------------------------------------------
    TEST("all expected tool names present");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto names = registry.tool_names();

        // Check representative tools from each category
        std::vector<std::string> expected = {
            "config.get", "config.list", "config.reset", "config.set",
            "fs.delete_file", "fs.disk_usage", "fs.list_directory",
            "fs.read_file", "fs.search", "fs.write_file",
            "model.current", "model.info", "model.list",
            "network.connections", "network.dns_lookup",
            "network.interfaces", "network.ping",
            "process.info", "process.list",
            "system.info", "system.memory", "system.reboot",
            "system.shutdown", "system.temperature", "system.uptime"
        };

        for (const auto& name : expected) {
            bool found = false;
            for (const auto& n : names) {
                if (n == name) { found = true; break; }
            }
            if (!found) {
                printf("FAIL: missing tool '%s'\n", name.c_str());
                assert(false);
            }
        }
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 3: Confirmation flags on dangerous tools
    // ---------------------------------------------------------------
    TEST("confirmation flags set correctly");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        // Should require confirmation
        assert(registry.needs_confirmation("fs.delete_file"));
        assert(registry.needs_confirmation("system.shutdown"));
        assert(registry.needs_confirmation("system.reboot"));
        assert(registry.needs_confirmation("config.reset"));

        // Should NOT require confirmation
        assert(!registry.needs_confirmation("fs.read_file"));
        assert(!registry.needs_confirmation("fs.list_directory"));
        assert(!registry.needs_confirmation("system.info"));
        assert(!registry.needs_confirmation("process.list"));
        assert(!registry.needs_confirmation("network.interfaces"));
        assert(!registry.needs_confirmation("config.get"));
        assert(!registry.needs_confirmation("model.list"));
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 4: OpenAI tools JSON is valid
    // ---------------------------------------------------------------
    TEST("OpenAI tools JSON is valid and complete");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto tools_json = registry.to_openai_tools_json();
        auto parsed = json::parse(tools_json);

        assert(parsed.is_array());
        assert(parsed.size() == 25);

        // Every entry should have type:"function" and function.name
        for (const auto& entry : parsed) {
            assert(entry["type"] == "function");
            assert(entry["function"].contains("name"));
            assert(entry["function"].contains("description"));
            assert(entry["function"].contains("parameters"));
            assert(!entry["function"]["description"].get<std::string>().empty());
        }
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 5: fs.list_directory on /data
    // ---------------------------------------------------------------
    TEST("fs.list_directory on /data");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("fs.list_directory", R"({"path":"/data"})");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("entries"));
        assert(parsed.contains("count"));
        printf("(%d entries) ", parsed["count"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 6: fs.list_directory rejects path traversal
    // ---------------------------------------------------------------
    TEST("fs.list_directory rejects path traversal");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("fs.list_directory", R"({"path":"/data/../etc"})");
        auto parsed = json::parse(result);
        assert(parsed.contains("error"));

        auto result2 = registry.dispatch("fs.list_directory", R"({"path":"/etc"})");
        auto parsed2 = json::parse(result2);
        assert(parsed2.contains("error"));

        auto result3 = registry.dispatch("fs.list_directory", R"({"path":"/tmp"})");
        auto parsed3 = json::parse(result3);
        assert(parsed3.contains("error"));
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 7: fs.read_file works
    // ---------------------------------------------------------------
    TEST("fs.read_file reads test file");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("fs.read_file", R"({"path":"/data/testdir/hello.txt"})");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("content"));
        std::string content = parsed["content"];
        assert(content.find("Hello from Llamaste") != std::string::npos);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 8: fs.read_file rejects paths outside /data
    // ---------------------------------------------------------------
    TEST("fs.read_file rejects /etc/passwd");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("fs.read_file", R"({"path":"/etc/passwd"})");
        auto parsed = json::parse(result);
        assert(parsed.contains("error"));
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 9: fs.write_file and read back
    // ---------------------------------------------------------------
    TEST("fs.write_file and read back");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto write_result = registry.dispatch("fs.write_file",
            R"({"path":"/data/testdir/written.txt","content":"test content 123"})");
        auto wparsed = json::parse(write_result);
        assert(!wparsed.contains("error"));
        assert(wparsed["bytes_written"] == 16);

        auto read_result = registry.dispatch("fs.read_file",
            R"({"path":"/data/testdir/written.txt"})");
        auto rparsed = json::parse(read_result);
        assert(!rparsed.contains("error"));
        assert(rparsed["content"] == "test content 123");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 10: fs.disk_usage returns data
    // ---------------------------------------------------------------
    TEST("fs.disk_usage returns disk info");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("fs.disk_usage", "{}");
        auto parsed = json::parse(result);

        // Might fail if /data doesn't exist as a separate partition
        // but should at least return without crashing
        if (!parsed.contains("error")) {
            assert(parsed.contains("total_bytes"));
            assert(parsed.contains("used_bytes"));
        }
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 11: fs.search finds test file
    // ---------------------------------------------------------------
    TEST("fs.search finds hello.txt");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("fs.search",
            R"({"pattern":"hello*","path":"/data/testdir"})");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed["count"].get<int>() >= 1);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 12: process.list returns processes
    // ---------------------------------------------------------------
    TEST("process.list returns processes");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("process.list", "{}");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("processes"));
        assert(parsed["count"].get<int>() >= 1);
        printf("(%d processes) ", parsed["count"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 13: process.info on PID 1
    // ---------------------------------------------------------------
    TEST("process.info on PID 1");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("process.info", R"({"pid":1})");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed["pid"] == 1);
        assert(parsed.contains("name"));
        printf("(name=%s) ", parsed["name"].get<std::string>().c_str());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 14: network.interfaces returns data
    // ---------------------------------------------------------------
    TEST("network.interfaces returns interfaces");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("network.interfaces", "{}");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("interfaces"));
        assert(parsed["count"].get<int>() >= 1);  // At least loopback
        printf("(%d interfaces) ", parsed["count"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 15: network.connections returns data
    // ---------------------------------------------------------------
    TEST("network.connections returns connection list");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("network.connections", "{}");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("connections"));
        printf("(%d connections) ", parsed["count"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 16: system.info returns system data
    // ---------------------------------------------------------------
    TEST("system.info returns system info");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("system.info", "{}");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("kernel"));
        assert(parsed.contains("hostname"));
        assert(parsed.contains("cpu_cores"));
        assert(parsed.contains("ram_total_mb"));
        assert(parsed["cpu_cores"].get<int>() >= 1);
        assert(parsed["ram_total_mb"].get<int>() > 0);
        printf("(cores=%d, ram=%dMB) ",
               parsed["cpu_cores"].get<int>(),
               parsed["ram_total_mb"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 17: system.uptime returns data
    // ---------------------------------------------------------------
    TEST("system.uptime returns uptime");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("system.uptime", "{}");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("uptime_seconds"));
        assert(parsed["uptime_seconds"].get<long>() > 0);
        assert(parsed.contains("human"));
        printf("(%s) ", parsed["human"].get<std::string>().c_str());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 18: system.memory returns memory data
    // ---------------------------------------------------------------
    TEST("system.memory returns memory info");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("system.memory", "{}");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("total_mb"));
        assert(parsed.contains("free_mb"));
        assert(parsed["total_mb"].get<long>() > 0);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 19: system.temperature returns (even if empty)
    // ---------------------------------------------------------------
    TEST("system.temperature returns zones");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("system.temperature", "{}");
        auto parsed = json::parse(result);

        assert(!parsed.contains("error"));
        assert(parsed.contains("zones"));
        printf("(%d zones) ", parsed["count"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 20: config.set and config.get round-trip
    // ---------------------------------------------------------------
    TEST("config.set and config.get round-trip");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto set_result = registry.dispatch("config.set",
            R"({"key":"test.integration.value","value":"42"})");
        auto sparsed = json::parse(set_result);
        assert(!sparsed.contains("error"));
        assert(sparsed["saved"] == true);

        auto get_result = registry.dispatch("config.get",
            R"({"key":"test.integration.value"})");
        auto gparsed = json::parse(get_result);
        assert(!gparsed.contains("error"));
        assert(gparsed["value"] == "42");
        assert(gparsed["source"] == "user");

        // Clean up
        unlink("/data/llamaste/config/test.integration.value");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 21: config.get returns defaults
    // ---------------------------------------------------------------
    TEST("config.get returns default values");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("config.get",
            R"({"key":"system.hostname"})");
        auto parsed = json::parse(result);
        assert(!parsed.contains("error"));
        assert(parsed["value"] == "llamaste");
        assert(parsed["source"] == "default");
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 22: config.list returns all config
    // ---------------------------------------------------------------
    TEST("config.list returns config entries");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("config.list", "{}");
        auto parsed = json::parse(result);
        assert(!parsed.contains("error"));
        assert(parsed.contains("config"));
        assert(parsed["count"].get<int>() >= 9);  // At least 9 defaults
        printf("(%d entries) ", parsed["count"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 23: config.get rejects invalid keys
    // ---------------------------------------------------------------
    TEST("config.get rejects invalid keys");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("config.get",
            R"({"key":"../../etc/passwd"})");
        auto parsed = json::parse(result);
        assert(parsed.contains("error"));
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 24: model.list returns (even if empty)
    // ---------------------------------------------------------------
    TEST("model.list returns model list");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("model.list", "{}");
        auto parsed = json::parse(result);
        assert(!parsed.contains("error"));
        assert(parsed.contains("models"));
        printf("(%d models) ", parsed["count"].get<int>());
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 25: model.current shows no model loaded
    // ---------------------------------------------------------------
    TEST("model.current shows no model loaded");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("model.current", "{}");
        auto parsed = json::parse(result);
        assert(!parsed.contains("error"));
        assert(parsed["loaded"] == false);
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 26: model.info rejects path traversal
    // ---------------------------------------------------------------
    TEST("model.info rejects path traversal");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto result = registry.dispatch("model.info",
            R"({"filename":"../../../etc/passwd"})");
        auto parsed = json::parse(result);
        assert(parsed.contains("error"));
        PASS();
    }

    // ---------------------------------------------------------------
    // Test 27: All tools return valid JSON
    // ---------------------------------------------------------------
    TEST("all tools return valid JSON on dispatch");
    {
        ToolRegistry registry;
        register_all_tools(registry);

        auto names = registry.tool_names();

        // Skip shutdown and reboot (they would actually shut down!)
        std::vector<std::string> skip = {"system.shutdown", "system.reboot"};

        for (const auto& name : names) {
            bool should_skip = false;
            for (const auto& s : skip) {
                if (name == s) { should_skip = true; break; }
            }
            if (should_skip) continue;

            auto result = registry.dispatch(name, "{}");
            auto parsed = json::parse(result, nullptr, false);
            if (parsed.is_discarded()) {
                printf("FAIL: %s returned invalid JSON: %s\n", name.c_str(), result.c_str());
                assert(false);
            }
        }
        printf("(%zu tools tested) ", names.size() - skip.size());
        PASS();
    }

    cleanup_test_data();

    printf("\n=== All %d integration tests passed. ===\n", tests_passed);
    return 0;
}
