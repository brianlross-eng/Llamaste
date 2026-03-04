# Model Download & USB Sideload Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable searching, downloading GGUF models from Hugging Face, and importing from USB drives.

**Architecture:** Five new tools in `tools_model_download.cpp` using libcurl for HTTPS. Pure helper functions are testable without libcurl. USB sideload uses mount/copy syscalls.

**Tech Stack:** C++17, libcurl, OpenSSL (Buildroot), nlohmann/json

**Design doc:** `docs/plans/2026-03-08-model-download-design.md`

---

### Task 1: Buildroot & Kernel Config Changes

**Files:**
- Modify: `br2-external/configs/llamaste_x86_64_defconfig`
- Modify: `br2-external/board/llamaste/linux.config`

**Step 1: Add OpenSSL + libcurl to defconfig**

Add these lines after `BR2_PACKAGE_LLAMA_SERVER=y` (around line 41):

```
# HTTPS downloads (model download from Hugging Face)
BR2_PACKAGE_OPENSSL=y
BR2_PACKAGE_LIBCURL=y
```

**Step 2: Add NTFS3 to kernel config**

Add after the VFAT/FAT lines (around line 58 in linux.config):

```
# NTFS read support for USB drives
CONFIG_NTFS3_FS=y
```

**Step 3: Commit**

```bash
git add br2-external/configs/llamaste_x86_64_defconfig br2-external/board/llamaste/linux.config
git commit -m "build: add openssl + libcurl for model downloads, NTFS3 for USB"
```

---

### Task 2: Test File + Helper Function Tests

**Files:**
- Create: `tests/test_model_download.cpp`

**Step 1: Create test file with tests for pure helper functions**

These test URL building, validation, model table, and disk space formatting — no libcurl needed.

```cpp
// tests/test_model_download.cpp — Model download tool tests
#include <cstdio>
#include <cstring>
#include <cassert>
#include <string>
#include <vector>

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
```

**Step 2: Verify it does NOT compile yet (no implementation)**

Run: `cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -o tests/build/test_model_download tests/test_model_download.cpp 2>&1 | head -5`
Expected: Linker errors — undefined reference to `build_hf_download_url` etc.

**Step 3: Commit test file**

```bash
git add tests/test_model_download.cpp
git commit -m "test: add model download helper function tests"
```

---

### Task 3: Implement Helper Functions + model.recommended

**Files:**
- Create: `src/llamaste/tools_model_download.cpp`
- Modify: `src/llamaste/tools.h` (add declaration)
- Modify: `src/llamaste/tools.cpp` (register call)

**Step 1: Create tools_model_download.cpp with helpers + model.recommended**

```cpp
// tools_model_download.cpp — Model download, search, and USB import tools
//
// Provides tools for discovering and downloading GGUF models from
// Hugging Face, importing from USB drives, and recommending models
// based on available RAM.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <sstream>
#include <fstream>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#ifdef HAVE_LIBCURL
#include <curl/curl.h>
#endif

using json = nlohmann::json;

// =========================================================================
// Model table (shared with main.cpp select_model — single source of truth)
// =========================================================================

struct ModelInfo {
    const char* name;
    const char* filename;
    const char* repo_id;
    int required_mb;     // Min RAM to run this model
    int approx_size_mb;  // Approximate download size
};

static const ModelInfo MODEL_TABLE[] = {
    {"Qwen2.5-32B-Instruct",  "qwen2.5-32b-instruct-q4_k_m.gguf",
     "Qwen/Qwen2.5-32B-Instruct-GGUF",  22000, 19000},
    {"Qwen2.5-14B-Instruct",  "qwen2.5-14b-instruct-q4_k_m.gguf",
     "Qwen/Qwen2.5-14B-Instruct-GGUF",  11000, 8700},
    {"Qwen2.5-7B-Instruct",   "qwen2.5-7b-instruct-q4_k_m.gguf",
     "Qwen/Qwen2.5-7B-Instruct-GGUF",    6500, 4700},
    {"Qwen2.5-3B-Instruct",   "qwen2.5-3b-instruct-q4_k_m.gguf",
     "Qwen/Qwen2.5-3B-Instruct-GGUF",    4000, 2000},
    {"Qwen2.5-1.5B-Instruct", "qwen2.5-1.5b-instruct-q4_k_m.gguf",
     "Qwen/Qwen2.5-1.5B-Instruct-GGUF",  2500, 1000},
    {"Qwen2.5-0.5B-Instruct", "qwen2.5-0.5b-instruct-q4_k_m.gguf",
     "Qwen/Qwen2.5-0.5B-Instruct-GGUF",  1500,  400},
    {nullptr, nullptr, nullptr, 0, 0}
};

// =========================================================================
// Pure helper functions (testable without libcurl)
// =========================================================================

std::string build_hf_download_url(const std::string& repo_id, const std::string& filename) {
    return "https://huggingface.co/" + repo_id + "/resolve/main/" + filename;
}

std::string build_hf_search_url(const std::string& query, int limit) {
    // URL-encode spaces as +
    std::string encoded;
    for (char c : query) {
        if (c == ' ') encoded += '+';
        else encoded += c;
    }
    return "https://huggingface.co/api/models?search=" + encoded
         + "+gguf&sort=downloads&direction=-1&limit=" + std::to_string(limit);
}

std::string build_hf_tree_url(const std::string& repo_id) {
    return "https://huggingface.co/api/models/" + repo_id + "/tree/main";
}

bool validate_gguf_filename(const std::string& filename) {
    if (filename.size() < 6) return false;
    if (filename.substr(filename.size() - 5) != ".gguf") return false;
    if (filename.find('/') != std::string::npos) return false;
    if (filename.find("..") != std::string::npos) return false;
    return true;
}

bool validate_repo_id(const std::string& repo_id) {
    if (repo_id.find('/') == std::string::npos) return false;
    if (repo_id.find("..") != std::string::npos) return false;
    // Must be "org/repo" format
    auto slash = repo_id.find('/');
    if (slash == 0 || slash == repo_id.size() - 1) return false;
    return true;
}

std::string format_bytes(uint64_t bytes) {
    char buf[32];
    if (bytes >= 1073741824ULL) {
        snprintf(buf, sizeof(buf), "%.1f GB", bytes / 1073741824.0);
    } else if (bytes >= 1048576ULL) {
        snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    }
    return buf;
}

const ModelInfo* get_model_table() {
    return MODEL_TABLE;
}

const ModelInfo* recommend_model(int available_mb) {
    for (int i = 0; MODEL_TABLE[i].name; i++) {
        if (available_mb >= MODEL_TABLE[i].required_mb) {
            return &MODEL_TABLE[i];
        }
    }
    return nullptr;
}

static uint64_t get_free_disk_mb(const char* path) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) return 0;
    return (uint64_t)st.f_bavail * st.f_frsize / (1024 * 1024);
}

// =========================================================================
// Tool: model.recommended
// =========================================================================

static std::string handle_model_recommended(const std::string& args_json) {
    // Get available RAM (read from /proc/meminfo)
    int available_mb = 0;
    std::ifstream meminfo("/proc/meminfo");
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.find("MemAvailable:") == 0) {
            // "MemAvailable:   3787432 kB"
            long kb = 0;
            sscanf(line.c_str(), "MemAvailable: %ld", &kb);
            available_mb = (int)(kb / 1024);
            break;
        }
    }

    const ModelInfo* rec = recommend_model(available_mb);

    json result;
    result["ram_available_mb"] = available_mb;
    result["free_disk_mb"] = get_free_disk_mb("/data");

    if (rec) {
        result["recommended"] = true;
        result["model_name"] = rec->name;
        result["filename"] = rec->filename;
        result["repo_id"] = rec->repo_id;
        result["ram_required_mb"] = rec->required_mb;
        result["approx_download_mb"] = rec->approx_size_mb;
        result["download_url"] = build_hf_download_url(rec->repo_id, rec->filename);

        // Check if already downloaded
        std::string path = std::string("/data/models/") + rec->filename;
        result["already_downloaded"] = (access(path.c_str(), R_OK) == 0);
    } else {
        result["recommended"] = false;
        result["message"] = "Not enough RAM for any supported model (need at least 1500 MB)";
    }

    return result.dump(2);
}

// =========================================================================
// Registration
// =========================================================================

void register_model_download_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "model.recommended",
        .description = "Show the recommended model for this hardware based on available RAM. "
                       "Returns model name, download URL, size, and whether it's already downloaded.",
        .parameters = R"json({"type":"object","properties":{},"required":[]})json",
        .handler = handle_model_recommended
    });

    // model.search, model.files, model.download, model.usb_import
    // will be added in subsequent tasks
}
```

**Step 2: Add declaration to tools.h**

After `void register_model_tools(ToolRegistry& reg);` (line 54), add:
```cpp
void register_model_download_tools(ToolRegistry& reg);
```

**Step 3: Add registration call to tools.cpp**

In `register_all_tools()` after `register_model_tools(reg);` (line 104), add:
```cpp
    register_model_download_tools(reg);
```

**Step 4: Compile and run tests**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -o tests/build/test_model_download \
    tests/test_model_download.cpp src/llamaste/tools_model_download.cpp 2>&1 && \
    tests/build/test_model_download
```
Expected: All 18 tests PASS

**Step 5: Commit**

```bash
git add src/llamaste/tools_model_download.cpp src/llamaste/tools.h src/llamaste/tools.cpp
git commit -m "feat: add model.recommended tool and download helper functions"
```

---

### Task 4: Implement model.search + model.files

**Files:**
- Modify: `src/llamaste/tools_model_download.cpp`

**Step 1: Add libcurl HTTP GET helper and model.search + model.files handlers**

Add after the `handle_model_recommended` function but before `register_model_download_tools`:

```cpp
// =========================================================================
// libcurl HTTP helper (guarded — only available in Buildroot builds)
// =========================================================================

#ifdef HAVE_LIBCURL
static size_t curl_write_cb(void* contents, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static std::string http_get(const std::string& url, long timeout_seconds = 15) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Llamaste/0.1");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return "";
    return response;
}
#endif // HAVE_LIBCURL

// =========================================================================
// Tool: model.search
// =========================================================================

static std::string handle_model_search(const std::string& args_json) {
    json args = json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.contains("query")) {
        return R"json({"error":"Required parameter: query"})json";
    }

    std::string query = args["query"].get<std::string>();
    int limit = args.value("limit", 10);
    if (limit > 20) limit = 20;

#ifdef HAVE_LIBCURL
    std::string url = build_hf_search_url(query, limit);
    std::string response = http_get(url);
    if (response.empty()) {
        return R"json({"error":"Failed to connect to Hugging Face API. Check network connection."})json";
    }

    json hf_results = json::parse(response, nullptr, false);
    if (hf_results.is_discarded() || !hf_results.is_array()) {
        return R"json({"error":"Invalid response from Hugging Face API"})json";
    }

    json results = json::array();
    for (auto& model : hf_results) {
        json entry;
        entry["repo_id"] = model.value("modelId", "");
        entry["author"] = model.value("author", "");
        entry["downloads"] = model.value("downloads", 0);
        entry["likes"] = model.value("likes", 0);
        entry["last_modified"] = model.value("lastModified", "");
        // Include pipeline_tag and tags if useful
        if (model.contains("pipeline_tag"))
            entry["pipeline_tag"] = model["pipeline_tag"];
        results.push_back(entry);
    }

    json out;
    out["query"] = query;
    out["count"] = results.size();
    out["models"] = results;
    return out.dump(2);
#else
    return R"json({"error":"Model search requires HTTPS support (libcurl). Not available in host builds."})json";
#endif
}

// =========================================================================
// Tool: model.files
// =========================================================================

static std::string handle_model_files(const std::string& args_json) {
    json args = json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.contains("repo_id")) {
        return R"json({"error":"Required parameter: repo_id"})json";
    }

    std::string repo_id = args["repo_id"].get<std::string>();
    if (!validate_repo_id(repo_id)) {
        return R"json({"error":"Invalid repo_id format. Use 'owner/repo' format."})json";
    }

#ifdef HAVE_LIBCURL
    std::string url = build_hf_tree_url(repo_id);
    std::string response = http_get(url);
    if (response.empty()) {
        return R"json({"error":"Failed to fetch file list. Check repo_id and network."})json";
    }

    json tree = json::parse(response, nullptr, false);
    if (tree.is_discarded() || !tree.is_array()) {
        return R"json({"error":"Repository not found or invalid response"})json";
    }

    json gguf_files = json::array();
    for (auto& item : tree) {
        std::string path = item.value("path", "");
        if (path.size() > 5 && path.substr(path.size() - 5) == ".gguf") {
            json file;
            file["filename"] = path;
            uint64_t size = item.value("size", (uint64_t)0);
            file["size_bytes"] = size;
            file["size_human"] = format_bytes(size);
            gguf_files.push_back(file);
        }
    }

    json out;
    out["repo_id"] = repo_id;
    out["count"] = gguf_files.size();
    out["files"] = gguf_files;
    return out.dump(2);
#else
    return R"json({"error":"File listing requires HTTPS support (libcurl). Not available in host builds."})json";
#endif
}
```

**Step 2: Register the two new tools in `register_model_download_tools`**

Add after the model.recommended registration:

```cpp
    reg.register_tool({
        .name = "model.search",
        .description = "Search Hugging Face for GGUF model repositories. "
                       "Returns a list of matching repos with download counts.",
        .parameters = R"json({"type":"object","properties":{
            "query":{"type":"string","description":"Search query, e.g. 'qwen2.5 7b'"},
            "limit":{"type":"integer","description":"Max results (default 10, max 20)"}
        },"required":["query"]})json",
        .handler = handle_model_search
    });

    reg.register_tool({
        .name = "model.files",
        .description = "List GGUF files in a Hugging Face model repository. "
                       "Use after model.search to see available quantization variants.",
        .parameters = R"json({"type":"object","properties":{
            "repo_id":{"type":"string","description":"HF repo, e.g. 'Qwen/Qwen2.5-3B-Instruct-GGUF'"}
        },"required":["repo_id"]})json",
        .handler = handle_model_files
    });
```

**Step 3: Verify tests still pass**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -o tests/build/test_model_download \
    tests/test_model_download.cpp src/llamaste/tools_model_download.cpp 2>&1 && \
    tests/build/test_model_download
```
Expected: 18 tests PASS (libcurl code guarded by `#ifdef`)

**Step 4: Commit**

```bash
git add src/llamaste/tools_model_download.cpp
git commit -m "feat: add model.search and model.files tools (HuggingFace API)"
```

---

### Task 5: Implement model.download

**Files:**
- Modify: `src/llamaste/tools_model_download.cpp`

**Step 1: Add download progress callback and model.download handler**

Add after `handle_model_files` but before registration:

```cpp
// =========================================================================
// Tool: model.download
// =========================================================================

#ifdef HAVE_LIBCURL
struct DownloadProgress {
    uint64_t total_bytes;
    uint64_t downloaded_bytes;
    double speed_bps;
};

static int curl_progress_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
    auto* prog = (DownloadProgress*)clientp;
    prog->total_bytes = (uint64_t)dltotal;
    prog->downloaded_bytes = (uint64_t)dlnow;
    return 0;  // 0 = continue, non-zero = abort
}
#endif

static std::string handle_model_download(const std::string& args_json) {
    json args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) {
        return R"json({"error":"Invalid JSON arguments"})json";
    }

    std::string repo_id = args.value("repo_id", "");
    std::string filename = args.value("filename", "");

    if (repo_id.empty() || filename.empty()) {
        return R"json({"error":"Required parameters: repo_id, filename"})json";
    }
    if (!validate_repo_id(repo_id)) {
        return R"json({"error":"Invalid repo_id format"})json";
    }
    if (!validate_gguf_filename(filename)) {
        return R"json({"error":"Invalid filename. Must end with .gguf, no path components."})json";
    }

    std::string dest_path = std::string("/data/models/") + filename;
    std::string part_path = dest_path + ".part";

    // Check if already downloaded
    if (access(dest_path.c_str(), R_OK) == 0) {
        json out;
        out["status"] = "already_exists";
        out["path"] = dest_path;
        out["message"] = "Model already downloaded";
        return out.dump(2);
    }

    // Check disk space
    uint64_t free_mb = get_free_disk_mb("/data");

#ifdef HAVE_LIBCURL
    std::string url = build_hf_download_url(repo_id, filename);

    // Ensure /data/models/ exists
    mkdir("/data/models", 0755);

    // Check for partial download (resume support)
    uint64_t existing_size = 0;
    struct stat st;
    if (stat(part_path.c_str(), &st) == 0) {
        existing_size = st.st_size;
    }

    FILE* fp = fopen(part_path.c_str(), existing_size > 0 ? "ab" : "wb");
    if (!fp) {
        return R"json({"error":"Cannot write to /data/models/. Check disk and permissions."})json";
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        return R"json({"error":"Failed to initialize download"})json";
    }

    DownloadProgress progress = {};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);  // 1KB/s minimum
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);      // for 60 seconds
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Llamaste/0.1");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Resume if partial file exists
    if (existing_size > 0) {
        curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)existing_size);
    }

    CURLcode res = curl_easy_perform(curl);

    double speed = 0;
    curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &speed);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        json out;
        out["status"] = "error";
        out["error"] = curl_easy_strerror(res);
        out["partial_file"] = part_path;
        out["downloaded_bytes"] = progress.downloaded_bytes + existing_size;
        out["message"] = "Download failed. The partial file is saved — retry will resume.";
        return out.dump(2);
    }

    if (http_code >= 400) {
        unlink(part_path.c_str());
        json out;
        out["status"] = "error";
        out["http_code"] = http_code;
        out["error"] = "HTTP error " + std::to_string(http_code);
        if (http_code == 404) out["message"] = "File not found in repository";
        return out.dump(2);
    }

    // Rename .part -> final
    if (rename(part_path.c_str(), dest_path.c_str()) != 0) {
        json out;
        out["status"] = "error";
        out["error"] = "Failed to rename downloaded file: " + std::string(strerror(errno));
        return out.dump(2);
    }

    // Get final size
    struct stat final_st;
    stat(dest_path.c_str(), &final_st);

    json out;
    out["status"] = "success";
    out["path"] = dest_path;
    out["filename"] = filename;
    out["size_bytes"] = (uint64_t)final_st.st_size;
    out["size_human"] = format_bytes(final_st.st_size);
    out["speed_human"] = format_bytes((uint64_t)speed) + "/s";
    out["message"] = "Model downloaded successfully. Restart to load it.";
    return out.dump(2);
#else
    json out;
    out["status"] = "error";
    out["error"] = "Download requires HTTPS support (libcurl). Not available in host builds.";
    return out.dump(2);
#endif
}
```

**Step 2: Register model.download**

Add to `register_model_download_tools`:

```cpp
    reg.register_tool({
        .name = "model.download",
        .description = "Download a GGUF model file from Hugging Face to /data/models/. "
                       "Supports resume for interrupted downloads. Use model.files first to see available files.",
        .parameters = R"json({"type":"object","properties":{
            "repo_id":{"type":"string","description":"HF repo, e.g. 'Qwen/Qwen2.5-3B-Instruct-GGUF'"},
            "filename":{"type":"string","description":"GGUF filename, e.g. 'qwen2.5-3b-instruct-q4_k_m.gguf'"}
        },"required":["repo_id","filename"]})json",
        .handler = handle_model_download
    });
```

**Step 3: Verify tests still pass, commit**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -o tests/build/test_model_download \
    tests/test_model_download.cpp src/llamaste/tools_model_download.cpp 2>&1 && \
    tests/build/test_model_download && \
git add src/llamaste/tools_model_download.cpp && \
git commit -m "feat: add model.download tool with resume support"
```

---

### Task 6: Implement model.usb_import

**Files:**
- Modify: `src/llamaste/tools_model_download.cpp`

**Step 1: Add USB scanning and import handler**

Add after `handle_model_download`:

```cpp
// =========================================================================
// Tool: model.usb_import
// =========================================================================

#ifndef _WIN32
#include <sys/mount.h>

static std::vector<std::string> find_usb_partitions() {
    std::vector<std::string> partitions;
    std::ifstream proc_parts("/proc/partitions");
    std::string line;
    while (std::getline(proc_parts, line)) {
        // Skip header lines
        if (line.find("major") != std::string::npos) continue;
        if (line.empty()) continue;

        // Parse: "   8       16   15728640 sdb"
        char name[64] = {};
        if (sscanf(line.c_str(), " %*d %*d %*d %63s", name) == 1) {
            std::string dev(name);
            // Skip sda (system disk), loop devices, ram devices
            if (dev.find("sda") == 0) continue;
            if (dev.find("loop") == 0) continue;
            if (dev.find("ram") == 0) continue;
            // Only partitions (sdb1, sdc1, etc.) not whole disks
            if (dev.size() >= 4 && dev.back() >= '1' && dev.back() <= '9') {
                partitions.push_back("/dev/" + dev);
            }
        }
    }
    return partitions;
}

static bool try_mount(const std::string& device, const std::string& mountpoint) {
    mkdir(mountpoint.c_str(), 0755);
    // Try common filesystems in order
    const char* fstypes[] = {"vfat", "ext4", "ntfs3", "exfat", nullptr};
    for (int i = 0; fstypes[i]; i++) {
        if (mount(device.c_str(), mountpoint.c_str(), fstypes[i],
                  MS_RDONLY | MS_NOEXEC | MS_NOSUID, nullptr) == 0) {
            return true;
        }
    }
    return false;
}

static std::vector<std::pair<std::string, uint64_t>> scan_gguf_files(
    const std::string& dir, int depth = 0) {
    std::vector<std::pair<std::string, uint64_t>> files;
    if (depth > 3) return files;

    DIR* d = opendir(dir.c_str());
    if (!d) return files;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string path = dir + "/" + ent->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            auto sub = scan_gguf_files(path, depth + 1);
            files.insert(files.end(), sub.begin(), sub.end());
        } else if (S_ISREG(st.st_mode)) {
            std::string name(ent->d_name);
            if (name.size() > 5 && name.substr(name.size() - 5) == ".gguf") {
                files.push_back({path, (uint64_t)st.st_size});
            }
        }
    }
    closedir(d);
    return files;
}
#endif // _WIN32

static std::string handle_model_usb_import(const std::string& args_json) {
#ifndef _WIN32
    json args = json::parse(args_json, nullptr, false);
    std::string action = args.is_object() ? args.value("action", "scan") : "scan";

    const std::string mountpoint = "/mnt/usb";

    if (action == "scan") {
        auto partitions = find_usb_partitions();
        if (partitions.empty()) {
            return R"json({"error":"No USB drives detected. Insert a USB drive and try again."})json";
        }

        json all_files = json::array();
        for (auto& dev : partitions) {
            if (try_mount(dev, mountpoint)) {
                auto files = scan_gguf_files(mountpoint);
                for (auto& [path, size] : files) {
                    json f;
                    f["device"] = dev;
                    // Strip mountpoint prefix to show relative path
                    f["usb_path"] = path.substr(mountpoint.size());
                    f["filename"] = path.substr(path.rfind('/') + 1);
                    f["size_bytes"] = size;
                    f["size_human"] = format_bytes(size);
                    all_files.push_back(f);
                }
                umount(mountpoint.c_str());
            }
        }

        json out;
        out["devices_found"] = partitions.size();
        out["gguf_files"] = all_files;
        out["count"] = all_files.size();
        if (all_files.empty()) {
            out["message"] = "No GGUF files found on USB drives.";
        } else {
            out["message"] = "Found " + std::to_string(all_files.size())
                           + " GGUF file(s). Use action='import' with device and filename to copy.";
        }
        return out.dump(2);

    } else if (action == "import") {
        std::string device = args.value("device", "");
        std::string filename = args.value("filename", "");

        if (device.empty() || filename.empty()) {
            return R"json({"error":"Import requires 'device' and 'filename' parameters"})json";
        }
        if (!validate_gguf_filename(filename)) {
            return R"json({"error":"Invalid GGUF filename"})json";
        }

        if (!try_mount(device, mountpoint)) {
            return R"json({"error":"Failed to mount USB device"})json";
        }

        // Find the file on USB
        auto files = scan_gguf_files(mountpoint);
        std::string source_path;
        uint64_t source_size = 0;
        for (auto& [path, size] : files) {
            if (path.find(filename) != std::string::npos) {
                source_path = path;
                source_size = size;
                break;
            }
        }

        if (source_path.empty()) {
            umount(mountpoint.c_str());
            json err;
            err["error"] = "File '" + filename + "' not found on " + device;
            return err.dump();
        }

        // Check disk space
        uint64_t free_mb = get_free_disk_mb("/data");
        uint64_t need_mb = source_size / (1024 * 1024) + 100;  // +100MB margin
        if (free_mb < need_mb) {
            umount(mountpoint.c_str());
            json err;
            err["error"] = "Not enough disk space. Need " + std::to_string(need_mb)
                         + " MB, have " + std::to_string(free_mb) + " MB";
            return err.dump();
        }

        // Copy file
        mkdir("/data/models", 0755);
        std::string dest = std::string("/data/models/") + filename;
        std::ifstream src(source_path, std::ios::binary);
        std::ofstream dst(dest, std::ios::binary);

        if (!src || !dst) {
            umount(mountpoint.c_str());
            return R"json({"error":"Failed to open source or destination file"})json";
        }

        char buf[65536];
        uint64_t copied = 0;
        while (src.read(buf, sizeof(buf)) || src.gcount() > 0) {
            dst.write(buf, src.gcount());
            copied += src.gcount();
        }

        src.close();
        dst.close();
        umount(mountpoint.c_str());

        json out;
        out["status"] = "success";
        out["path"] = dest;
        out["filename"] = filename;
        out["size_bytes"] = copied;
        out["size_human"] = format_bytes(copied);
        out["message"] = "Model imported from USB. Restart to load it.";
        return out.dump(2);
    }

    return R"json({"error":"Unknown action. Use 'scan' or 'import'."})json";
#else
    return R"json({"error":"USB import not available on this platform"})json";
#endif
}
```

**Step 2: Register model.usb_import**

Add to `register_model_download_tools`:

```cpp
    reg.register_tool({
        .name = "model.usb_import",
        .description = "Scan USB drives for GGUF model files and import them to /data/models/. "
                       "Use action='scan' to list files, action='import' with device and filename to copy.",
        .parameters = R"json({"type":"object","properties":{
            "action":{"type":"string","enum":["scan","import"],"description":"scan=list files, import=copy file"},
            "device":{"type":"string","description":"Device path for import, e.g. '/dev/sdb1'"},
            "filename":{"type":"string","description":"GGUF filename to import"}
        },"required":["action"]})json",
        .handler = handle_model_usb_import
    });
```

**Step 3: Verify tests still pass, commit**

```bash
cd /mnt/d/Llamaste && g++ -std=c++17 -I src/llamaste -o tests/build/test_model_download \
    tests/test_model_download.cpp src/llamaste/tools_model_download.cpp 2>&1 && \
    tests/build/test_model_download && \
git add src/llamaste/tools_model_download.cpp && \
git commit -m "feat: add model.usb_import tool (scan + import)"
```

---

### Task 7: CMakeLists.txt + host-test.sh + Full Test Run

**Files:**
- Modify: `src/llamaste/CMakeLists.txt`
- Modify: `scripts/host-test.sh`

**Step 1: Add libcurl to CMakeLists.txt**

After the liblzma section (line 58), add:

```cmake
# libcurl for HTTPS downloads (model download from Hugging Face)
if(LLAMASTE_STATIC)
    find_library(CURL_LIB NAMES libcurl.a)
else()
    find_library(CURL_LIB NAMES curl)
endif()
if(CURL_LIB)
    message(STATUS "Found libcurl: ${CURL_LIB}")
    target_compile_definitions(llamaste PRIVATE HAVE_LIBCURL)
    target_link_libraries(llamaste PRIVATE ${CURL_LIB})
    # libcurl with OpenSSL needs these when linking statically
    if(LLAMASTE_STATIC)
        find_library(SSL_LIB NAMES libssl.a)
        find_library(CRYPTO_LIB NAMES libcrypto.a)
        if(SSL_LIB AND CRYPTO_LIB)
            target_link_libraries(llamaste PRIVATE ${SSL_LIB} ${CRYPTO_LIB})
        endif()
    endif()
else()
    message(STATUS "libcurl not found - model download disabled, search/browse only with pre-placed models")
endif()
```

**Step 2: Add tools_model_download.cpp to LLAMASTE_SOURCES**

In the `set(LLAMASTE_SOURCES` block (around line 7), add after `tools_model.cpp`:
```
    tools_model_download.cpp
```

**Step 3: Add Suite 9 to host-test.sh**

After Suite 8 (Inference Integration) section, add:

```bash
# ---------------------------------------------------------------
# Suite 9: Model Download
# ---------------------------------------------------------------
echo -e "${BOLD}--- [9/9] Model Download ---${NC}"
if g++ -std=c++17 -I "${SRC}" -o "${BUILD_DIR}/test_model_download" \
    "${TESTS}/test_model_download.cpp" \
    "${SRC}/tools_model_download.cpp" 2>&1; then
    if "${BUILD_DIR}/test_model_download"; then
        suite_pass "Model Download"
    else
        suite_fail "Model Download (runtime)"
    fi
else
    suite_fail "Model Download (compile)"
fi
echo ""
```

Also update all `[N/8]` references to `[N/9]` in the existing suite headers.

**Step 4: Run full test suite**

```bash
cd /mnt/d/Llamaste && bash scripts/host-test.sh
```
Expected: ALL 9 TEST SUITES PASSED

**Step 5: Commit**

```bash
git add src/llamaste/CMakeLists.txt scripts/host-test.sh
git commit -m "build: add libcurl linking, model download test suite (9 suites)"
```

---

### Task 8: Dashboard Download Button + REST Endpoint

**Files:**
- Modify: `src/llamaste/web/dashboard.js`
- Modify: `src/llamaste/child_main.cpp`

**Step 1: Add download button to dashboard.js**

In the Dashboard tab's model section, add a "Download Recommended Model" button that appears when no model is loaded. When clicked, POST to `/llamaste/model/download-recommended`. Show progress via SSE notifications.

**Step 2: Add REST endpoint in child_main.cpp**

Add `POST /llamaste/model/download-recommended` handler that:
1. Calls `model.recommended` tool to get the best model
2. Calls `model.download` with the recommended repo_id + filename
3. Returns progress/result as JSON
4. Sends SSE notification on completion

**Step 3: Test via browser, commit**

```bash
git add src/llamaste/web/dashboard.js src/llamaste/child_main.cpp
git commit -m "feat: dashboard download button + REST endpoint"
```

---

### Task 9: Buildroot Build + QEMU Verification

**Step 1: Apply defconfig and rebuild**

```bash
cd /root/llamaste-build/output && \
make llamaste_x86_64_defconfig && \
make llamaste-dirclean && make llamaste && make
```

**Step 2: Verify build artifacts**

- Check `llamaste` binary links libcurl: `ldd output/target/opt/llamaste/llamaste | grep curl`
- Check squashfs size (should be ~75-80 MB with openssl+libcurl added)

**Step 3: QEMU E2E test**

```bash
bash /mnt/d/Llamaste/scripts/qemu-boot-test.sh /root/llamaste-build/output/images/llamaste.img
```
Expected: 5/5 E2E tests pass, tool count increases to 37

**Step 4: Update docs**

Update SESSION-STATUS.md with new tool count and test suite count.
