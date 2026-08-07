// tools_fs.cpp — Filesystem tools for Llamaste LLM-OS
//
// All filesystem operations are restricted to the /data partition.
// Path traversal attacks (../) are rejected. This is the user-writable
// filesystem; the root filesystem is read-only squashfs.

#include "tools.h"
#include "json.hpp"
#include <string>
#include <cstring>
#include <cstdlib>   // realpath, free
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>
#include <fnmatch.h>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Path security: all paths must resolve within /data/
// ---------------------------------------------------------------------------

// Canonicalize a path without requiring the file to exist.
// Resolves ".." and "." components manually.
static std::string canonicalize_path(const std::string& path) {
    std::vector<std::string> parts;
    std::istringstream ss(path);
    std::string segment;

    while (std::getline(ss, segment, '/')) {
        if (segment.empty() || segment == ".") continue;
        if (segment == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(segment);
        }
    }

    std::string result = "/";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) result += "/";
        result += parts[i];
    }
    return result;
}

static bool validate_data_path(const std::string& path) {
    // Must start with /data or /data/
    if (path == "/data") return true;
    if (path.substr(0, 6) != "/data/") return false;
    // Canonicalize to prevent traversal
    std::string canon = canonicalize_path(path);
    if (canon != "/data" && canon.substr(0, 6) != "/data/") return false;
    // Must not contain .. (belt-and-suspenders with canonicalization)
    if (path.find("..") != std::string::npos) return false;
#ifndef _WIN32
    // Follow symlinks: resolve the longest EXISTING ancestor with realpath and
    // require it to stay under /data. The lexical checks above don't catch a
    // symlink under /data (e.g. -> /etc). The remaining non-existent suffix
    // can't contain a symlink and is already ".."-free per the check above.
    {
        std::string probe = path;
        for (;;) {
            char* rp = realpath(probe.c_str(), nullptr);
            if (rp) {
                std::string r(rp);
                free(rp);
                return (r == "/data" || r.compare(0, 6, "/data/") == 0);
            }
            size_t slash = probe.find_last_of('/');
            if (slash == std::string::npos || slash == 0) return false;
            probe.resize(slash);
        }
    }
#endif
    return true;
}

static std::string path_error(const std::string& path) {
    json err;
    err["error"] = "path not allowed: must be under /data/";
    err["path"] = path;
    return err.dump();
}

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

// ---------------------------------------------------------------------------
// fs.list_directory — list files in a directory
// ---------------------------------------------------------------------------
static std::string handle_fs_list_directory(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string path = args.value("path", "/data");
    if (!validate_data_path(path)) return path_error(path);

    DIR* dir = opendir(path.c_str());
    if (!dir) {
        return json_error("cannot open directory: " + path + " (" + strerror(errno) + ")");
    }

    json entries = json::array();
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        std::string full_path = path + "/" + name;
        struct stat st;
        json entry;
        entry["name"] = name;

        if (stat(full_path.c_str(), &st) == 0) {
            entry["size"] = st.st_size;
            entry["type"] = S_ISDIR(st.st_mode) ? "directory" : "file";
            entry["modified"] = static_cast<int64_t>(st.st_mtime);
        } else {
            entry["type"] = (ent->d_type == DT_DIR) ? "directory" : "file";
        }
        entries.push_back(entry);
    }
    closedir(dir);

    json result;
    result["path"] = path;
    result["entries"] = entries;
    result["count"] = entries.size();
    return result.dump();
}

// ---------------------------------------------------------------------------
// fs.read_file — read file contents
// ---------------------------------------------------------------------------
static std::string handle_fs_read_file(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string path = args.value("path", "");
    if (path.empty()) return json_error("path is required");
    if (!validate_data_path(path)) return path_error(path);

    // Optional: max_bytes to limit read size (default 64KB)
    int max_bytes = args.value("max_bytes", 65536);
    if (max_bytes > 1048576) max_bytes = 1048576;  // Hard cap at 1MB

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return json_error("cannot open file: " + path + " (" + strerror(errno) + ")");
    }

    std::string content(max_bytes, '\0');
    file.read(&content[0], max_bytes);
    auto bytes_read = file.gcount();
    content.resize(static_cast<size_t>(bytes_read));

    bool truncated = false;
    if (file.peek() != EOF) {
        truncated = true;
    }

    json result;
    result["path"] = path;
    result["content"] = content;
    result["size"] = bytes_read;
    result["truncated"] = truncated;
    return result.dump();
}

// ---------------------------------------------------------------------------
// fs.write_file — write file contents
// ---------------------------------------------------------------------------
static std::string handle_fs_write_file(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string path = args.value("path", "");
    std::string content = args.value("content", "");
    if (path.empty()) return json_error("path is required");
    if (!validate_data_path(path)) return path_error(path);

    bool append = args.value("append", false);

    auto mode = append ? (std::ios::app | std::ios::binary) : (std::ios::binary);
    std::ofstream file(path, mode);
    if (!file.is_open()) {
        return json_error("cannot write file: " + path + " (" + strerror(errno) + ")");
    }

    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    file.close();

    json result;
    result["path"] = path;
    result["bytes_written"] = content.size();
    result["append"] = append;
    return result.dump();
}

// ---------------------------------------------------------------------------
// fs.delete_file — delete a file (requires confirmation)
// ---------------------------------------------------------------------------
static std::string handle_fs_delete_file(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string path = args.value("path", "");
    if (path.empty()) return json_error("path is required");
    if (!validate_data_path(path)) return path_error(path);

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return json_error("file not found: " + path);
    }

    if (S_ISDIR(st.st_mode)) {
        return json_error("cannot delete directory with fs.delete_file: " + path);
    }

    if (unlink(path.c_str()) != 0) {
        return json_error("delete failed: " + path + " (" + strerror(errno) + ")");
    }

    json result;
    result["path"] = path;
    result["deleted"] = true;
    return result.dump();
}

// ---------------------------------------------------------------------------
// fs.disk_usage — show disk usage of /data
// ---------------------------------------------------------------------------
static std::string handle_fs_disk_usage(const std::string& args_json) {
    (void)args_json;

    struct statvfs vfs;
    if (statvfs("/data", &vfs) != 0) {
        return json_error("cannot stat /data: " + std::string(strerror(errno)));
    }

    uint64_t total = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
    uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bfree) * vfs.f_frsize;
    uint64_t avail = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    uint64_t used = total - free_bytes;

    json result;
    result["path"] = "/data";
    result["total_bytes"] = total;
    result["used_bytes"] = used;
    result["free_bytes"] = free_bytes;
    result["available_bytes"] = avail;
    if (total > 0) {
        result["used_percent"] = static_cast<int>((used * 100) / total);
    } else {
        result["used_percent"] = 0;
    }
    return result.dump();
}

// ---------------------------------------------------------------------------
// fs.search — search for files by name pattern
// ---------------------------------------------------------------------------
static void search_recursive(const std::string& dir, const std::string& pattern,
                             json& results, int max_results, int& count, int depth = 0) {
    if (count >= max_results) return;
    if (depth > 20) return;   // guard against symlink cycles / runaway recursion

    DIR* d = opendir(dir.c_str());
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr && count < max_results) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;

        std::string full_path = dir + "/" + name;

        // Check if name matches pattern (case-insensitive)
        if (fnmatch(pattern.c_str(), name.c_str(), FNM_CASEFOLD) == 0) {
            struct stat st;
            json entry;
            entry["path"] = full_path;
            entry["name"] = name;
            if (stat(full_path.c_str(), &st) == 0) {
                entry["size"] = st.st_size;
                entry["type"] = S_ISDIR(st.st_mode) ? "directory" : "file";
            }
            results.push_back(entry);
            count++;
        }

        // Recurse into directories — use lstat so symlinked dirs are NOT followed
        // (a symlink cycle under /data would otherwise recurse until the stack blows).
        struct stat st;
        if (lstat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            search_recursive(full_path, pattern, results, max_results, count, depth + 1);
        }
    }
    closedir(d);
}

static std::string handle_fs_search(const std::string& args_json) {
    auto args = json::parse(args_json, nullptr, false);
    if (args.is_discarded()) return json_error("invalid JSON arguments");

    std::string pattern = args.value("pattern", "*");
    std::string path = args.value("path", "/data");
    int max_results = args.value("max_results", 50);
    if (max_results > 200) max_results = 200;

    if (!validate_data_path(path)) return path_error(path);

    json matches = json::array();
    int count = 0;
    search_recursive(path, pattern, matches, max_results, count);

    json result;
    result["pattern"] = pattern;
    result["path"] = path;
    result["matches"] = matches;
    result["count"] = count;
    result["truncated"] = (count >= max_results);
    return result.dump();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void register_fs_tools(ToolRegistry& reg) {
    reg.register_tool({
        .name = "fs.list_directory",
        .description = "List files and directories at the given path under /data. "
                       "Returns name, type, size, and modification time for each entry.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "Directory path to list (must be under /data/)",
                    "default": "/data"
                }
            }
        })json",
        .handler = handle_fs_list_directory
    });

    reg.register_tool({
        .name = "fs.read_file",
        .description = "Read the contents of a file under /data. Returns up to max_bytes "
                       "(default 64KB, max 1MB). Indicates if content was truncated.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "File path to read (must be under /data/)"
                },
                "max_bytes": {
                    "type": "integer",
                    "description": "Maximum bytes to read (default 65536, max 1048576)",
                    "default": 65536
                }
            },
            "required": ["path"]
        })json",
        .handler = handle_fs_read_file
    });

    reg.register_tool({
        .name = "fs.write_file",
        .description = "Write content to a file under /data. Creates the file if it "
                       "doesn't exist. Set append=true to append instead of overwrite.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "File path to write (must be under /data/)"
                },
                "content": {
                    "type": "string",
                    "description": "Content to write to the file"
                },
                "append": {
                    "type": "boolean",
                    "description": "Append to file instead of overwriting",
                    "default": false
                }
            },
            "required": ["path", "content"]
        })json",
        .handler = handle_fs_write_file
    });

    reg.register_tool({
        .name = "fs.delete_file",
        .description = "Delete a file under /data. Cannot delete directories. "
                       "This action requires user confirmation.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "path": {
                    "type": "string",
                    "description": "File path to delete (must be under /data/)"
                }
            },
            "required": ["path"]
        })json",
        .handler = handle_fs_delete_file,
        .requires_confirmation = true
    });

    reg.register_tool({
        .name = "fs.disk_usage",
        .description = "Show disk usage statistics for the /data partition, including "
                       "total, used, free, and available space in bytes.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = handle_fs_disk_usage
    });

    reg.register_tool({
        .name = "fs.search",
        .description = "Search for files by name pattern under /data. Supports glob "
                       "patterns like *.txt or config*. Searches recursively.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "pattern": {
                    "type": "string",
                    "description": "Glob pattern to match filenames (e.g., '*.txt', 'config*')",
                    "default": "*"
                },
                "path": {
                    "type": "string",
                    "description": "Directory to search in (must be under /data/)",
                    "default": "/data"
                },
                "max_results": {
                    "type": "integer",
                    "description": "Maximum results to return (default 50, max 200)",
                    "default": 50
                }
            }
        })json",
        .handler = handle_fs_search
    });
}
