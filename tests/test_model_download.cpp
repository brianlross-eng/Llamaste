// tests/test_model_download.cpp — Model download tool tests
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cassert>
#include <string>
#include <vector>

// Minimal ToolRegistry stub so tools_model_download.cpp links without all tool files
#include "tools.h"
void ToolRegistry::register_tool(ToolDef) {}

// Forward declarations of functions we'll test (defined in tools_model_download.cpp)
std::string build_hf_download_url(const std::string& repo_id, const std::string& filename);
std::string build_hf_search_url(const std::string& query, int limit);
std::string build_hf_tree_url(const std::string& repo_id);
bool validate_gguf_filename(const std::string& filename);
bool validate_repo_id(const std::string& repo_id);
std::string format_bytes(uint64_t bytes);

struct ModelInfo {
    const char* name;
    const char* filename;
    const char* repo_id;
    int required_mb;
    int approx_size_mb;
};
const ModelInfo* get_model_table();
const ModelInfo* recommend_model(int available_mb);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, expr) do { \
    if (expr) { printf("TEST: %s ... PASS\n", name); tests_passed++; } \
    else { printf("TEST: %s ... FAIL (line %d)\n", name, __LINE__); tests_failed++; } \
} while(0)

int main() {
    printf("=== Model Download Tests ===\n");

    // URL building tests
    TEST("build_hf_download_url basic",
        build_hf_download_url("Qwen/Qwen2.5-0.5B-Instruct-GGUF", "qwen2.5-0.5b-instruct-q4_k_m.gguf")
        == "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/main/qwen2.5-0.5b-instruct-q4_k_m.gguf");

    TEST("build_hf_search_url",
        build_hf_search_url("qwen gguf", 10).find("search=qwen") != std::string::npos);

    TEST("build_hf_tree_url",
        build_hf_tree_url("Qwen/Qwen2.5-0.5B-Instruct-GGUF")
        == "https://huggingface.co/api/models/Qwen/Qwen2.5-0.5B-Instruct-GGUF/tree/main");

    // Validation tests
    TEST("validate_gguf_filename valid",
        validate_gguf_filename("qwen2.5-0.5b-instruct-q4_k_m.gguf") == true);
    TEST("validate_gguf_filename no extension",
        validate_gguf_filename("model.bin") == false);
    TEST("validate_gguf_filename path traversal",
        validate_gguf_filename("../etc/passwd.gguf") == false);
    TEST("validate_gguf_filename slash",
        validate_gguf_filename("sub/model.gguf") == false);

    TEST("validate_repo_id valid",
        validate_repo_id("Qwen/Qwen2.5-0.5B-Instruct-GGUF") == true);
    TEST("validate_repo_id no slash",
        validate_repo_id("just-a-name") == false);
    TEST("validate_repo_id traversal",
        validate_repo_id("../evil/repo") == false);

    // Format bytes
    TEST("format_bytes KB",
        format_bytes(1536) == "1.5 KB");
    TEST("format_bytes MB",
        format_bytes(524288000) == "500.0 MB");
    TEST("format_bytes GB",
        format_bytes(4294967296ULL) == "4.0 GB");

    // Model table
    const ModelInfo* table = get_model_table();
    TEST("model_table not null", table != nullptr);
    TEST("model_table first is 32B", strcmp(table[0].name, "Qwen2.5-32B-Instruct") == 0);
    TEST("model_table has repo_id", strstr(table[0].repo_id, "GGUF") != nullptr);

    // Recommend model
    TEST("recommend 4GB -> 3B", strcmp(recommend_model(4000)->filename,
        "qwen2.5-3b-instruct-q4_k_m.gguf") == 0);
    TEST("recommend 2GB -> 1.5B", strcmp(recommend_model(2500)->filename,
        "qwen2.5-1.5b-instruct-q4_k_m.gguf") == 0);
    TEST("recommend 1GB -> 0.5B", strcmp(recommend_model(1500)->filename,
        "qwen2.5-0.5b-instruct-q4_k_m.gguf") == 0);
    TEST("recommend 500MB -> null", recommend_model(500) == nullptr);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed;
}
