// test_http.cpp -- Integration tests for the Llamaste HTTP server
//
// Starts the HTTP server in a background thread and runs requests against it
// using httplib::Client. Tests all major routes: health, static files,
// agent chat, system dashboard, tools listing, conversations, and OpenAI API.
//
// Build (from project root, in WSL2):
//   g++ -std=c++17 -I src/llamaste -pthread -o tests/test_http \
//     tests/test_http.cpp src/llamaste/child_main.cpp \
//     src/llamaste/agent.cpp src/llamaste/prompt_builder.cpp \
//     src/llamaste/tools.cpp src/llamaste/tools_fs.cpp \
//     src/llamaste/tools_process.cpp src/llamaste/tools_network.cpp \
//     src/llamaste/tools_system.cpp src/llamaste/tools_config.cpp \
//     src/llamaste/tools_model.cpp src/llamaste/hwdetect.cpp
//
// Run: ./tests/test_http

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <csignal>

#include "../src/llamaste/supervisor.h"
#include "../src/llamaste/json.hpp"

// Include httplib for the client (without OpenSSL)
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
#undef CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include "../src/llamaste/httplib.h"

using json = nlohmann::json;

// Declared in child_main.cpp
extern int child_main(const SupervisorConfig& config);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("TEST: %s ... ", name)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

// Test port (high port to avoid needing root)
static const int TEST_PORT = 18321;
static const char* TEST_HOST = "localhost";

// Server thread handle
static std::thread g_server_thread;

// Start the server in a background thread
static void start_server() {
    SupervisorConfig config;
    config.model_path = "";  // stub mode
    config.boot_mode = "server";
    config.http_port = TEST_PORT;
    config.cpu_cores = 4;
    config.ram_total_mb = 16384;

    g_server_thread = std::thread([config]() {
        child_main(config);
    });

    // Wait for server to be ready
    httplib::Client cli(TEST_HOST, TEST_PORT);
    cli.set_connection_timeout(1);
    cli.set_read_timeout(2);

    for (int i = 0; i < 50; i++) {  // up to 5 seconds
        auto res = cli.Get("/health");
        if (res && res->status == 200) {
            fprintf(stderr, "[test] Server ready after %d attempts\n", i + 1);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    fprintf(stderr, "[test] WARNING: Server may not be ready\n");
}

// Stop the server by sending SIGTERM-like signal (set g_running to false)
// We'll use the /health endpoint to verify it was running, then just
// send a signal to the child process. Since we're in the same process,
// we'll use raise(SIGINT).
static void stop_server() {
    // The child_main checks g_running which is set by signal handler.
    // Since the server is in a thread (not a forked process), we need
    // to trigger it differently. We'll just kill the server by raising
    // SIGINT which our signal handler catches.
    raise(SIGINT);

    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
    fprintf(stderr, "[test] Server stopped\n");
}

int main() {
    printf("=== Llamaste HTTP Server Integration Tests ===\n\n");

    // Start server
    fprintf(stderr, "[test] Starting HTTP server on port %d...\n", TEST_PORT);
    start_server();

    httplib::Client cli(TEST_HOST, TEST_PORT);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    // ---------------------------------------------------------------
    // Test 1: GET /health returns 200 with valid JSON
    // ---------------------------------------------------------------
    TEST("GET /health returns 200 with valid JSON");
    {
        auto res = cli.Get("/health");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            FAIL("Expected 200");
        } else {
            auto j = json::parse(res->body, nullptr, false);
            if (j.is_discarded()) {
                FAIL("Invalid JSON");
            } else if (!j.contains("status") || j["status"] != "ok") {
                FAIL("Missing or incorrect status field");
            } else if (!j.contains("tools_count") || j["tools_count"].get<int>() <= 0) {
                FAIL("Missing or zero tools_count");
            } else {
                PASS();
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 2: GET / returns HTML content
    // ---------------------------------------------------------------
    TEST("GET / returns HTML content");
    {
        auto res = cli.Get("/");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else if (res->body.find("<!DOCTYPE html>") == std::string::npos &&
                   res->body.find("<html") == std::string::npos) {
            FAIL("Response does not contain HTML");
        } else if (res->body.find("Llamaste") == std::string::npos) {
            FAIL("Response does not contain 'Llamaste'");
        } else {
            PASS();
        }
    }

    // ---------------------------------------------------------------
    // Test 3: GET /style.css returns CSS content
    // ---------------------------------------------------------------
    TEST("GET /style.css returns CSS content");
    {
        auto res = cli.Get("/style.css");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else if (res->body.find(":root") == std::string::npos &&
                   res->body.find("--bg-primary") == std::string::npos) {
            FAIL("Response does not look like CSS");
        } else {
            PASS();
        }
    }

    // ---------------------------------------------------------------
    // Test 4: GET /chat.js returns JavaScript content
    // ---------------------------------------------------------------
    TEST("GET /chat.js returns JavaScript content");
    {
        auto res = cli.Get("/chat.js");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else if (res->body.find("function") == std::string::npos) {
            FAIL("Response does not look like JavaScript");
        } else {
            PASS();
        }
    }

    // ---------------------------------------------------------------
    // Test 5: GET /dashboard.js returns JavaScript content
    // ---------------------------------------------------------------
    TEST("GET /dashboard.js returns JavaScript content");
    {
        auto res = cli.Get("/dashboard.js");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else if (res->body.find("function") == std::string::npos) {
            FAIL("Response does not look like JavaScript");
        } else {
            PASS();
        }
    }

    // ---------------------------------------------------------------
    // Test 6: POST /llamaste/chat (non-streaming) returns valid response
    // ---------------------------------------------------------------
    TEST("POST /llamaste/chat (non-streaming) returns valid response");
    {
        json body;
        body["message"] = "Hello!";
        body["stream"] = false;
        body["conversation_id"] = "test-conv-001";

        auto res = cli.Post("/llamaste/chat", body.dump(), "application/json");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else {
            auto j = json::parse(res->body, nullptr, false);
            if (j.is_discarded()) {
                FAIL("Invalid JSON response");
            } else if (!j.contains("content") || j["content"].get<std::string>().empty()) {
                FAIL("Missing or empty content field");
            } else if (!j.contains("conversation_id")) {
                FAIL("Missing conversation_id field");
            } else {
                PASS();
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 7: POST /llamaste/chat with empty message returns 400
    // ---------------------------------------------------------------
    TEST("POST /llamaste/chat with empty message returns 400");
    {
        json body;
        body["message"] = "";
        body["stream"] = false;

        auto res = cli.Post("/llamaste/chat", body.dump(), "application/json");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 400) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 400, got %d", res->status);
            FAIL(buf);
        } else {
            PASS();
        }
    }

    // ---------------------------------------------------------------
    // Test 8: POST /llamaste/chat with invalid JSON returns 400
    // ---------------------------------------------------------------
    TEST("POST /llamaste/chat with invalid JSON returns 400");
    {
        auto res = cli.Post("/llamaste/chat", "not json", "application/json");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 400) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 400, got %d", res->status);
            FAIL(buf);
        } else {
            PASS();
        }
    }

    // ---------------------------------------------------------------
    // Test 9: GET /llamaste/system returns valid system JSON
    // ---------------------------------------------------------------
    TEST("GET /llamaste/system returns valid system JSON");
    {
        auto res = cli.Get("/llamaste/system");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else {
            auto j = json::parse(res->body, nullptr, false);
            if (j.is_discarded()) {
                FAIL("Invalid JSON response");
            } else if (!j.contains("model")) {
                FAIL("Missing 'model' field");
            } else if (!j.contains("uptime_seconds")) {
                FAIL("Missing 'uptime_seconds' field");
            } else if (!j.contains("ram_total_mb")) {
                FAIL("Missing 'ram_total_mb' field");
            } else if (!j.contains("cpu_percent")) {
                FAIL("Missing 'cpu_percent' field");
            } else if (!j.contains("disk_total_gb")) {
                FAIL("Missing 'disk_total_gb' field");
            } else {
                PASS();
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 10: GET /llamaste/tools returns a JSON array
    // ---------------------------------------------------------------
    TEST("GET /llamaste/tools returns a JSON array");
    {
        auto res = cli.Get("/llamaste/tools");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else {
            auto j = json::parse(res->body, nullptr, false);
            if (j.is_discarded()) {
                FAIL("Invalid JSON response");
            } else if (!j.is_array()) {
                FAIL("Response is not a JSON array");
            } else if (j.empty()) {
                FAIL("Tools array is empty");
            } else {
                // Verify each entry has the correct structure
                bool valid = true;
                for (const auto& tool : j) {
                    if (!tool.contains("type") || tool["type"] != "function" ||
                        !tool.contains("function") || !tool["function"].contains("name")) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    FAIL("Tool entries have incorrect structure");
                } else {
                    PASS();
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 11: GET /llamaste/conversations returns conversations list
    // ---------------------------------------------------------------
    TEST("GET /llamaste/conversations returns conversations list");
    {
        auto res = cli.Get("/llamaste/conversations");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else {
            auto j = json::parse(res->body, nullptr, false);
            if (j.is_discarded()) {
                FAIL("Invalid JSON response");
            } else if (!j.contains("conversations") || !j["conversations"].is_array()) {
                FAIL("Missing or invalid 'conversations' field");
            } else {
                // We should have the conversation from test 6
                bool found = false;
                for (const auto& conv : j["conversations"]) {
                    if (conv.value("id", "") == "test-conv-001") {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    FAIL("test-conv-001 not found in conversations list");
                } else {
                    PASS();
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 12: POST /v1/chat/completions returns OpenAI-format response
    // ---------------------------------------------------------------
    TEST("POST /v1/chat/completions returns OpenAI-format response");
    {
        json body;
        body["model"] = "llamaste";
        body["messages"] = json::array({
            {{"role", "user"}, {"content", "Hello"}}
        });

        auto res = cli.Post("/v1/chat/completions", body.dump(), "application/json");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else {
            auto j = json::parse(res->body, nullptr, false);
            if (j.is_discarded()) {
                FAIL("Invalid JSON response");
            } else if (!j.contains("choices") || !j["choices"].is_array()) {
                FAIL("Missing or invalid 'choices' field");
            } else if (j["choices"].empty()) {
                FAIL("Choices array is empty");
            } else if (!j["choices"][0].contains("message")) {
                FAIL("Missing 'message' in first choice");
            } else if (j["choices"][0]["message"].value("role", "") != "assistant") {
                FAIL("Message role is not 'assistant'");
            } else if (j["choices"][0]["message"].value("content", "").empty()) {
                FAIL("Message content is empty");
            } else {
                PASS();
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 13: POST /llamaste/chat (streaming) returns SSE data
    // ---------------------------------------------------------------
    TEST("POST /llamaste/chat (streaming) returns SSE data");
    {
        json body;
        body["message"] = "What can you do?";
        body["stream"] = true;
        body["conversation_id"] = "test-conv-sse";

        // For streaming, we use the regular Post and read the full body
        // (httplib accumulates the chunked response)
        auto res = cli.Post("/llamaste/chat", body.dump(), "application/json");
        if (!res) {
            FAIL("No response from server");
        } else if (res->status != 200) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Expected 200, got %d", res->status);
            FAIL(buf);
        } else {
            // Check that the body contains SSE events
            bool has_token = res->body.find("\"type\":\"token\"") != std::string::npos;
            bool has_done = res->body.find("\"type\":\"done\"") != std::string::npos;
            bool has_sse_done = res->body.find("[DONE]") != std::string::npos;
            bool has_data_prefix = res->body.find("data: ") != std::string::npos;

            if (!has_data_prefix) {
                FAIL("Response does not contain SSE 'data: ' prefix");
            } else if (!has_token) {
                FAIL("Response does not contain token events");
            } else if (!has_done) {
                FAIL("Response does not contain done event");
            } else if (!has_sse_done) {
                FAIL("Response does not contain [DONE] terminator");
            } else {
                PASS();
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 14: Conversation persists across requests
    // ---------------------------------------------------------------
    TEST("Conversation persists across requests");
    {
        // First message
        json body1;
        body1["message"] = "My name is TestUser";
        body1["stream"] = false;
        body1["conversation_id"] = "test-persist";

        auto res1 = cli.Post("/llamaste/chat", body1.dump(), "application/json");
        if (!res1 || res1->status != 200) {
            FAIL("First request failed");
        } else {
            // Second message to same conversation
            json body2;
            body2["message"] = "What was my name?";
            body2["stream"] = false;
            body2["conversation_id"] = "test-persist";

            auto res2 = cli.Post("/llamaste/chat", body2.dump(), "application/json");
            if (!res2 || res2->status != 200) {
                FAIL("Second request failed");
            } else {
                auto j = json::parse(res2->body, nullptr, false);
                if (j.is_discarded()) {
                    FAIL("Invalid JSON response");
                } else {
                    // message_count should be > 2 (both user msgs + assistant responses)
                    int count = j.value("message_count", 0);
                    if (count >= 4) {
                        PASS();
                    } else {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "Expected >= 4 messages, got %d", count);
                        FAIL(buf);
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------
    // Test 15: Unknown route returns error JSON
    // ---------------------------------------------------------------
    TEST("Unknown route returns error JSON");
    {
        auto res = cli.Get("/nonexistent/route");
        if (!res) {
            FAIL("No response from server");
        } else {
            // Should return 404 with JSON error body
            auto j = json::parse(res->body, nullptr, false);
            if (res->status != 404) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Expected 404, got %d", res->status);
                FAIL(buf);
            } else if (j.is_discarded() || !j.contains("error")) {
                FAIL("Response is not JSON error");
            } else {
                PASS();
            }
        }
    }

    // ---------------------------------------------------------------
    // Summary
    // ---------------------------------------------------------------
    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);

    // Stop the server
    stop_server();

    return tests_failed > 0 ? 1 : 0;
}
