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
#include <cstdio>
#include <cstdint>

#ifndef _WIN32
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <unistd.h>
#else
// Minimal stubs for Windows host compilation (tests only)
#include <sys/stat.h>
#include <direct.h>
#define access _access
#define R_OK 4
static int statvfs(const char*, struct { uint64_t f_bavail; uint64_t f_frsize; }*) { return -1; }
#endif

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

#ifndef _WIN32
static uint64_t get_free_disk_mb(const char* path) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) return 0;
    return (uint64_t)st.f_bavail * st.f_frsize / (1024 * 1024);
}
#else
static uint64_t get_free_disk_mb(const char*) { return 0; }
#endif

// =========================================================================
// Tool: model.recommended
// =========================================================================

static std::string handle_model_recommended(const std::string& /*args_json*/) {
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
    (void)query;
    (void)limit;
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
    (void)repo_id;
    return R"json({"error":"File listing requires HTTPS support (libcurl). Not available in host builds."})json";
#endif
}

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
    (void)dest_path;
    (void)part_path;
    json out;
    out["status"] = "error";
    out["error"] = "Download requires HTTPS support (libcurl). Not available in host builds.";
    return out.dump(2);
#endif
}

// =========================================================================
// Tool: model.usb_import
// =========================================================================

#ifndef _WIN32
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
    (void)args_json;
    return R"json({"error":"USB import not available on this platform"})json";
#endif
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
}
