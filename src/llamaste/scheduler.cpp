// scheduler.cpp — Scheduled task and alert management for Llamaste LLM-OS
//
// Manages recurring and one-shot tasks that trigger LLM inference at
// specific times, handles system health alerts (RAM/disk/temp), and
// notifies clients via SSE.  Runs its own background thread.
//
// Persistence: tasks are stored as JSON in {data_dir}/schedules.json.
// No shell commands are used — this runs as PID 1 with no /bin/sh.

#include "scheduler.h"
#include "hwdetect.h"
#include "json.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <thread>
#include <algorithm>
#include <random>

#ifndef _WIN32
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// localtime_r is POSIX (musl/glibc); provide shim for MSVC host builds
#ifdef _WIN32
static struct tm* localtime_r(const time_t* timep, struct tm* result) {
    if (localtime_s(result, timep) == 0) return result;
    return nullptr;
}
#endif

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string generate_id() {
    // Thread-local RNG seeded from hardware entropy
    thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, 15);

    char buf[9];
    for (int i = 0; i < 8; i++) {
        buf[i] = "0123456789abcdef"[dist(rng)];
    }
    buf[8] = '\0';
    return std::string(buf);
}

static std::string json_error(const std::string& msg) {
    json err;
    err["error"] = msg;
    return err.dump();
}

// Push a notification and optionally call notify_fn.
// Acquires notif_mutex_ internally; caller must NOT hold it.
void Scheduler::fire_task(const ScheduledTask& task) {
    fprintf(stderr, "[scheduler] Firing task '%s': %s\n",
            task.id.c_str(), task.prompt.c_str());

    // Copy inference_fn under lock (it may be set from another thread)
    SchedulerInferenceFn local_inference;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local_inference = inference_fn_;
    }

    std::string result;
    if (local_inference) {
        try {
            result = local_inference(task.prompt);
        } catch (const std::exception& e) {
            result = std::string("Error: ") + e.what();
            fprintf(stderr, "[scheduler] Inference error for task '%s': %s\n",
                    task.id.c_str(), e.what());
        }
    } else {
        result = "(no inference function configured)";
    }

    // Truncate result for notification body
    std::string body = result;
    if (body.size() > 200) {
        body = body.substr(0, 197) + "...";
    }

    Notification notif;
    notif.id    = generate_id();
    notif.type  = "scheduled";
    notif.title = "Scheduled: " + task.id;
    notif.body  = body;
    notif.time  = time(nullptr);

    // Push to queue
    {
        std::lock_guard<std::mutex> nlock(notif_mutex_);
        pending_notifications_.push_back(notif);
    }

    // Call notify callback outside any lock (using local copy of notif)
    SchedulerNotifyFn local_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local_notify = notify_fn_;
    }
    if (local_notify) {
        try { local_notify(notif); } catch (...) {}
    }
}

// ---------------------------------------------------------------------------
// Configuration setters
// ---------------------------------------------------------------------------

void Scheduler::set_data_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_dir_ = dir;
    load_tasks();
}

void Scheduler::set_inference_fn(SchedulerInferenceFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    inference_fn_ = std::move(fn);
}

void Scheduler::set_notify_fn(SchedulerNotifyFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_fn_ = std::move(fn);
}

void Scheduler::set_model_check_fn(SchedulerModelCheckFn fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    model_check_fn_ = std::move(fn);
}

void Scheduler::push_notification(Notification notif) {
    {
        std::lock_guard<std::mutex> lock(notif_mutex_);
        pending_notifications_.push_back(notif);
    }
    SchedulerNotifyFn local_notify;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local_notify = notify_fn_;
    }
    if (local_notify) {
        try { local_notify(notif); } catch (...) {}
    }
}

void Scheduler::set_alert_config(const AlertConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    alerts_ = config;
}

// ---------------------------------------------------------------------------
// Persistence: load_tasks / save_tasks
// ---------------------------------------------------------------------------

void Scheduler::load_tasks() {
    // Caller must hold mutex_
    tasks_.clear();

    if (data_dir_.empty()) return;

    std::string path = data_dir_ + "/schedules.json";
    std::ifstream f(path);
    if (!f.is_open()) {
        fprintf(stderr, "[scheduler] No schedules file at %s (starting fresh)\n",
                path.c_str());
        return;
    }

    std::stringstream ss;
    ss << f.rdbuf();
    f.close();

    auto doc = json::parse(ss.str(), nullptr, false);
    if (doc.is_discarded() || !doc.is_array()) {
        fprintf(stderr, "[scheduler] Warning: corrupt schedules.json, starting fresh\n");
        return;
    }

    for (auto& jtask : doc) {
        ScheduledTask t;
        t.id             = jtask.value("id", "");
        t.enabled        = jtask.value("enabled", true);
        t.prompt         = jtask.value("prompt", "");
        t.cron           = jtask.value("cron", "");
        t.at             = static_cast<time_t>(jtask.value("at", 0));
        t.every_seconds  = jtask.value("every_seconds", 0);
        t.once           = jtask.value("once", false);
        t.created        = static_cast<time_t>(jtask.value("created", 0));
        t.last_run       = static_cast<time_t>(jtask.value("last_run", 0));

        if (t.id.empty() || t.prompt.empty()) continue;

        // Compute next_run from schedule
        time_t now = time(nullptr);
        if (!t.cron.empty()) {
            t.next_run = compute_next_cron(t.cron, now);
        } else if (t.at > 0) {
            t.next_run = t.at;
        } else if (t.every_seconds > 0) {
            if (t.last_run > 0) {
                t.next_run = t.last_run + t.every_seconds;
                if (t.next_run <= now) t.next_run = now + 1;
            } else {
                t.next_run = now + t.every_seconds;
            }
        }

        tasks_.push_back(std::move(t));
    }

    fprintf(stderr, "[scheduler] Loaded %zu tasks from %s\n",
            tasks_.size(), path.c_str());
}

void Scheduler::save_tasks() {
    // Caller must hold mutex_
    if (data_dir_.empty()) return;

    std::string path = data_dir_ + "/schedules.json";
    std::string tmp_path = path + ".tmp";

#ifndef _WIN32
    // Ensure data directory exists (mkdir -p equivalent, one level)
    struct stat st;
    if (stat(data_dir_.c_str(), &st) != 0) {
        mkdir(data_dir_.c_str(), 0755);
    }
#endif

    json arr = json::array();
    for (auto& t : tasks_) {
        json jtask;
        jtask["id"]            = t.id;
        jtask["enabled"]       = t.enabled;
        jtask["prompt"]        = t.prompt;
        jtask["cron"]          = t.cron;
        jtask["at"]            = static_cast<int64_t>(t.at);
        jtask["every_seconds"] = t.every_seconds;
        jtask["once"]          = t.once;
        jtask["created"]       = static_cast<int64_t>(t.created);
        jtask["last_run"]      = static_cast<int64_t>(t.last_run);
        arr.push_back(jtask);
    }

    // Atomic write: write to .tmp, then rename over the real file
    std::ofstream f(tmp_path, std::ios::binary);
    if (!f.is_open()) {
        fprintf(stderr, "[scheduler] Error: cannot write %s\n", tmp_path.c_str());
        return;
    }
    f << arr.dump(2);
    f.close();

    if (f.fail()) {
        fprintf(stderr, "[scheduler] Error: write to %s failed\n", tmp_path.c_str());
        return;
    }

#ifndef _WIN32
    // POSIX rename is atomic on same filesystem
    if (rename(tmp_path.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "[scheduler] Error: rename %s -> %s failed\n",
                tmp_path.c_str(), path.c_str());
    }
#else
    // Windows fallback: remove + rename (not atomic, but only used for host testing)
    std::remove(path.c_str());
    std::rename(tmp_path.c_str(), path.c_str());
#endif
}

// ---------------------------------------------------------------------------
// Cron parser: compute_next_cron
// ---------------------------------------------------------------------------
//
// Simple 5-field cron: minute hour day month weekday
// Supports only * (any) and single integer values.
// Returns the next time_t after `after` matching the expression, or 0 on error.

time_t Scheduler::compute_next_cron(const std::string& expr, time_t after) const {
    // Parse the five fields
    std::istringstream ss(expr);
    std::string f_min, f_hour, f_day, f_month, f_wday;
    if (!(ss >> f_min >> f_hour >> f_day >> f_month >> f_wday)) {
        fprintf(stderr, "[scheduler] Invalid cron expression: '%s'\n", expr.c_str());
        return 0;
    }

    auto parse_field = [](const std::string& field, int& value) -> bool {
        // Returns true if field is a wildcard (*), false if it's a specific value.
        // Sets value only if it's a specific integer.
        if (field == "*") return true;
        char* end = nullptr;
        long v = strtol(field.c_str(), &end, 10);
        if (end == field.c_str()) return true;  // Parse failure -> treat as *
        value = static_cast<int>(v);
        return false;
    };

    int want_min = -1, want_hour = -1, want_day = -1, want_month = -1, want_wday = -1;
    bool any_min   = parse_field(f_min,   want_min);
    bool any_hour  = parse_field(f_hour,  want_hour);
    bool any_day   = parse_field(f_day,   want_day);
    bool any_month = parse_field(f_month, want_month);
    bool any_wday  = parse_field(f_wday,  want_wday);

    // Validate field ranges
    if (!any_min && (want_min < 0 || want_min > 59)) {
        fprintf(stderr, "[scheduler] Cron minute out of range (0-59): %d\n", want_min);
        return 0;
    }
    if (!any_hour && (want_hour < 0 || want_hour > 23)) {
        fprintf(stderr, "[scheduler] Cron hour out of range (0-23): %d\n", want_hour);
        return 0;
    }
    if (!any_day && (want_day < 1 || want_day > 31)) {
        fprintf(stderr, "[scheduler] Cron day out of range (1-31): %d\n", want_day);
        return 0;
    }
    if (!any_month && (want_month < 1 || want_month > 12)) {
        fprintf(stderr, "[scheduler] Cron month out of range (1-12): %d\n", want_month);
        return 0;
    }
    if (!any_wday && (want_wday < 0 || want_wday > 6)) {
        fprintf(stderr, "[scheduler] Cron weekday out of range (0-6): %d\n", want_wday);
        return 0;
    }

    // Start searching from one minute after `after`
    struct tm start_tm;
    time_t candidate = after + 60;
    localtime_r(&candidate, &start_tm);
    start_tm.tm_sec = 0;
    candidate = mktime(&start_tm);

    // Search up to ~2 years of minutes (prevent infinite loop)
    int max_iterations = 366 * 24 * 60;

    for (int i = 0; i < max_iterations; i++) {
        struct tm ctm;
        localtime_r(&candidate, &ctm);

        // Check month (1-12 in cron, 0-11 in struct tm)
        if (!any_month && (ctm.tm_mon + 1) != want_month) {
            // Jump to next month
            ctm.tm_mon++;
            ctm.tm_mday = 1;
            ctm.tm_hour = 0;
            ctm.tm_min = 0;
            ctm.tm_sec = 0;
            candidate = mktime(&ctm);
            continue;
        }

        // Check day of month (1-31)
        if (!any_day && ctm.tm_mday != want_day) {
            // Jump to next day
            ctm.tm_mday++;
            ctm.tm_hour = 0;
            ctm.tm_min = 0;
            ctm.tm_sec = 0;
            candidate = mktime(&ctm);
            continue;
        }

        // Check weekday (0=Sunday in both cron and struct tm)
        if (!any_wday && ctm.tm_wday != want_wday) {
            // Jump to next day
            ctm.tm_mday++;
            ctm.tm_hour = 0;
            ctm.tm_min = 0;
            ctm.tm_sec = 0;
            candidate = mktime(&ctm);
            continue;
        }

        // Check hour (0-23)
        if (!any_hour && ctm.tm_hour != want_hour) {
            // Jump to next hour
            ctm.tm_hour++;
            ctm.tm_min = 0;
            ctm.tm_sec = 0;
            candidate = mktime(&ctm);
            continue;
        }

        // Check minute (0-59)
        if (!any_min && ctm.tm_min != want_min) {
            // Jump to next minute
            ctm.tm_min++;
            ctm.tm_sec = 0;
            candidate = mktime(&ctm);
            continue;
        }

        // All fields match
        return candidate;
    }

    fprintf(stderr, "[scheduler] Cron expression '%s' did not match within search window\n",
            expr.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Main loop: start / stop / run_loop / destructor
// ---------------------------------------------------------------------------

void Scheduler::start() {
    if (running_) return;
    start_time_ = time(nullptr);
    running_ = true;
    thread_ = std::thread(&Scheduler::run_loop, this);
    fprintf(stderr, "[scheduler] Started background thread\n");
}

void Scheduler::stop() {
    if (!running_) return;
    running_ = false;
    fprintf(stderr, "[scheduler] Stopping...\n");
    if (thread_.joinable()) {
        thread_.join();
    }
    fprintf(stderr, "[scheduler] Background thread joined\n");
}

Scheduler::~Scheduler() {
    stop();
}

void Scheduler::run_loop() {
    while (running_) {
        check_tasks();
        check_alerts();

        // Sleep 10 seconds in 1-second increments for responsive shutdown
        for (int i = 0; i < 10 && running_; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    fprintf(stderr, "[scheduler] Background thread exited\n");
}

// ---------------------------------------------------------------------------
// Task checking
// ---------------------------------------------------------------------------

void Scheduler::check_tasks() {
    // Phase 1: collect due tasks under lock, then release
    struct DueTask {
        std::string id;
        ScheduledTask snapshot;
    };
    std::vector<DueTask> due;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        time_t now = time(nullptr);
        for (auto& task : tasks_) {
            if (!task.enabled) continue;
            if (task.next_run <= 0) continue;
            if (task.next_run > now) continue;
            due.push_back({task.id, task});
        }
    }

    if (due.empty()) return;

    // Phase 2: fire tasks WITHOUT holding mutex_ (inference can take minutes)
    for (auto& dt : due) {
        fire_task(dt.snapshot);
    }

    // Phase 3: update task state under lock
    {
        std::lock_guard<std::mutex> lock(mutex_);
        time_t now = time(nullptr);
        bool changed = false;
        std::vector<std::string> remove_ids;

        for (auto& dt : due) {
            auto it = std::find_if(tasks_.begin(), tasks_.end(),
                [&dt](const ScheduledTask& t) { return t.id == dt.id; });
            if (it == tasks_.end()) continue;  // deleted while we were firing

            it->last_run = now;
            changed = true;

            if (it->once) {
                remove_ids.push_back(it->id);
            } else {
                // Recompute next_run
                if (!it->cron.empty()) {
                    it->next_run = compute_next_cron(it->cron, now);
                } else if (it->every_seconds > 0) {
                    it->next_run = now + it->every_seconds;
                } else {
                    // One-shot with `at` — no more runs
                    it->next_run = 0;
                    it->enabled = false;
                }
            }
        }

        // Remove once-tasks
        for (auto& rid : remove_ids) {
            auto it = std::find_if(tasks_.begin(), tasks_.end(),
                [&rid](const ScheduledTask& t) { return t.id == rid; });
            if (it != tasks_.end()) {
                tasks_.erase(it);
            }
        }

        if (changed) {
            save_tasks();
        }
    }
}

// ---------------------------------------------------------------------------
// Alert checking
// ---------------------------------------------------------------------------

void Scheduler::check_alerts() {
#ifndef _WIN32
    // Copy alert config under lock so we don't hold it during system calls
    AlertConfig cfg;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cfg = alerts_;
    }

    time_t now = time(nullptr);

    // Helper: push notification and call notify_fn outside locks
    auto push_alert = [this](Notification notif) {
        {
            std::lock_guard<std::mutex> nlock(notif_mutex_);
            pending_notifications_.push_back(notif);
        }
        SchedulerNotifyFn local_notify;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            local_notify = notify_fn_;
        }
        if (local_notify) {
            try { local_notify(notif); } catch (...) {}
        }
    };

    // --- RAM check ---
    if (cfg.ram_threshold_percent > 0) {
        int total_kb = read_meminfo_kb("MemTotal");
        int avail_kb = read_meminfo_kb("MemAvailable");
        if (total_kb > 0 && avail_kb >= 0) {
            int used_kb = total_kb - avail_kb;
            int used_pct = (used_kb * 100) / total_kb;
            if (used_pct >= cfg.ram_threshold_percent) {
                if (now - last_ram_alert_ >= cfg.cooldown_seconds) {
                    last_ram_alert_ = now;

                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "RAM usage at %d%% (%d MB / %d MB used)",
                             used_pct, used_kb / 1024, total_kb / 1024);

                    Notification notif;
                    notif.id    = generate_id();
                    notif.type  = "alert";
                    notif.title = "High RAM Usage";
                    notif.body  = buf;
                    notif.time  = now;

                    fprintf(stderr, "[scheduler] ALERT: %s\n", buf);
                    push_alert(std::move(notif));
                }
            }
        }
    }

    // --- Disk check ---
    if (cfg.disk_threshold_percent > 0) {
        struct statvfs vfs;
        if (statvfs("/data", &vfs) == 0 && vfs.f_blocks > 0) {
            uint64_t total = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
            uint64_t free_bytes = static_cast<uint64_t>(vfs.f_bfree) * vfs.f_frsize;
            uint64_t used = total - free_bytes;
            int used_pct = static_cast<int>((used * 100) / total);

            if (used_pct >= cfg.disk_threshold_percent) {
                if (now - last_disk_alert_ >= cfg.cooldown_seconds) {
                    last_disk_alert_ = now;

                    char buf[128];
                    snprintf(buf, sizeof(buf),
                             "Disk usage at %d%% (%llu MB / %llu MB used)",
                             used_pct,
                             (unsigned long long)(used / (1024 * 1024)),
                             (unsigned long long)(total / (1024 * 1024)));

                    Notification notif;
                    notif.id    = generate_id();
                    notif.type  = "alert";
                    notif.title = "High Disk Usage";
                    notif.body  = buf;
                    notif.time  = now;

                    fprintf(stderr, "[scheduler] ALERT: %s\n", buf);
                    push_alert(std::move(notif));
                }
            }
        }
    }

    // --- Model not loaded check ---
    // After a 60s grace period, alert every 30 minutes if no model is loaded.
    {
        SchedulerModelCheckFn model_fn;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            model_fn = model_check_fn_;
        }
        if (model_fn && start_time_ > 0 &&
            (now - start_time_) >= 60 &&          // 60s startup grace
            !model_fn() &&                          // model not loaded
            (now - last_model_alert_) >= 1800) {   // at most every 30 min

            last_model_alert_ = now;

            Notification notif;
            notif.id    = generate_id();
            notif.type  = "alert";
            notif.title = "No AI Model Loaded";
            notif.body  = "Chat is unavailable. Go to Dashboard \u2192 Download a model to enable AI.";
            notif.time  = now;

            fprintf(stderr, "[scheduler] ALERT: No AI model loaded\n");
            push_alert(std::move(notif));
        }
    }

    // --- Temperature check ---
    if (cfg.temp_threshold_c > 0) {
        std::string temp_str = read_sysfs_line("/sys/class/thermal/thermal_zone0/temp");
        if (!temp_str.empty()) {
            char* end = nullptr;
            long milli_c = strtol(temp_str.c_str(), &end, 10);
            if (end != temp_str.c_str()) {
                int temp_c = static_cast<int>(milli_c / 1000);
                if (temp_c >= cfg.temp_threshold_c) {
                    if (now - last_temp_alert_ >= cfg.cooldown_seconds) {
                        last_temp_alert_ = now;

                        char buf[128];
                        snprintf(buf, sizeof(buf),
                                 "CPU temperature at %d C (threshold: %d C)",
                                 temp_c, cfg.temp_threshold_c);

                        Notification notif;
                        notif.id    = generate_id();
                        notif.type  = "alert";
                        notif.title = "High CPU Temperature";
                        notif.body  = buf;
                        notif.time  = now;

                        fprintf(stderr, "[scheduler] ALERT: %s\n", buf);
                        push_alert(std::move(notif));
                    }
                }
            }
        }
    }
#endif  // _WIN32
}

// ---------------------------------------------------------------------------
// Task CRUD
// ---------------------------------------------------------------------------

std::string Scheduler::create_task(const nlohmann::json& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string prompt = params.value("prompt", "");
    if (prompt.empty()) {
        return json_error("prompt is required");
    }

    std::string cron         = params.value("cron", "");
    int64_t at_val           = params.value("at", static_cast<int64_t>(0));
    int every_seconds        = params.value("every_seconds", 0);
    bool once                = params.value("once", false);

    if (cron.empty() && at_val == 0 && every_seconds == 0) {
        return json_error("one of cron, at, or every_seconds is required");
    }

    ScheduledTask task;
    task.id            = generate_id();
    task.enabled       = params.value("enabled", true);
    task.prompt        = prompt;
    task.cron          = cron;
    task.at            = static_cast<time_t>(at_val);
    task.every_seconds = every_seconds;
    task.once          = once;
    task.created       = time(nullptr);
    task.last_run      = 0;

    // Compute next_run
    time_t now = time(nullptr);
    if (!task.cron.empty()) {
        task.next_run = compute_next_cron(task.cron, now);
    } else if (task.at > 0) {
        task.next_run = task.at;
    } else if (task.every_seconds > 0) {
        task.next_run = now + task.every_seconds;
    }

    tasks_.push_back(task);
    save_tasks();

    fprintf(stderr, "[scheduler] Created task '%s': prompt='%s', next_run=%ld\n",
            task.id.c_str(), task.prompt.c_str(), static_cast<long>(task.next_run));

    json result;
    result["id"]            = task.id;
    result["enabled"]       = task.enabled;
    result["prompt"]        = task.prompt;
    result["cron"]          = task.cron;
    result["at"]            = static_cast<int64_t>(task.at);
    result["every_seconds"] = task.every_seconds;
    result["once"]          = task.once;
    result["created"]       = static_cast<int64_t>(task.created);
    result["next_run"]      = static_cast<int64_t>(task.next_run);
    return result.dump();
}

std::string Scheduler::list_tasks() {
    std::lock_guard<std::mutex> lock(mutex_);

    json arr = json::array();
    for (auto& t : tasks_) {
        json jtask;
        jtask["id"]            = t.id;
        jtask["enabled"]       = t.enabled;
        jtask["prompt"]        = t.prompt;
        jtask["cron"]          = t.cron;
        jtask["at"]            = static_cast<int64_t>(t.at);
        jtask["every_seconds"] = t.every_seconds;
        jtask["once"]          = t.once;
        jtask["created"]       = static_cast<int64_t>(t.created);
        jtask["last_run"]      = static_cast<int64_t>(t.last_run);
        jtask["next_run"]      = static_cast<int64_t>(t.next_run);
        arr.push_back(jtask);
    }

    json result;
    result["tasks"] = arr;
    result["count"] = arr.size();
    return result.dump();
}

std::string Scheduler::delete_task(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(tasks_.begin(), tasks_.end(),
        [&id](const ScheduledTask& t) { return t.id == id; });

    if (it == tasks_.end()) {
        return json_error("task not found: " + id);
    }

    tasks_.erase(it);
    save_tasks();

    fprintf(stderr, "[scheduler] Deleted task '%s'\n", id.c_str());

    json result;
    result["deleted"] = true;
    result["id"]      = id;
    return result.dump();
}

std::string Scheduler::update_task(const std::string& id, const nlohmann::json& params) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = std::find_if(tasks_.begin(), tasks_.end(),
        [&id](const ScheduledTask& t) { return t.id == id; });

    if (it == tasks_.end()) {
        return json_error("task not found: " + id);
    }

    bool schedule_changed = false;

    if (params.contains("prompt")) {
        it->prompt = params["prompt"].get<std::string>();
    }
    if (params.contains("enabled")) {
        it->enabled = params["enabled"].get<bool>();
    }
    if (params.contains("once")) {
        it->once = params["once"].get<bool>();
    }
    if (params.contains("cron")) {
        it->cron = params["cron"].get<std::string>();
        schedule_changed = true;
    }
    if (params.contains("at")) {
        it->at = static_cast<time_t>(params["at"].get<int64_t>());
        schedule_changed = true;
    }
    if (params.contains("every_seconds")) {
        it->every_seconds = params["every_seconds"].get<int>();
        schedule_changed = true;
    }

    if (schedule_changed) {
        time_t now = time(nullptr);
        if (!it->cron.empty()) {
            it->next_run = compute_next_cron(it->cron, now);
        } else if (it->at > 0) {
            it->next_run = it->at;
        } else if (it->every_seconds > 0) {
            it->next_run = now + it->every_seconds;
        } else {
            it->next_run = 0;
        }
    }

    save_tasks();

    fprintf(stderr, "[scheduler] Updated task '%s'\n", id.c_str());

    json result;
    result["id"]            = it->id;
    result["enabled"]       = it->enabled;
    result["prompt"]        = it->prompt;
    result["cron"]          = it->cron;
    result["at"]            = static_cast<int64_t>(it->at);
    result["every_seconds"] = it->every_seconds;
    result["once"]          = it->once;
    result["created"]       = static_cast<int64_t>(it->created);
    result["last_run"]      = static_cast<int64_t>(it->last_run);
    result["next_run"]      = static_cast<int64_t>(it->next_run);
    return result.dump();
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

int Scheduler::active_task_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (auto& t : tasks_) {
        if (t.enabled) count++;
    }
    return count;
}

time_t Scheduler::next_scheduled_time() const {
    std::lock_guard<std::mutex> lock(mutex_);
    time_t earliest = 0;
    for (auto& t : tasks_) {
        if (!t.enabled) continue;
        if (t.next_run <= 0) continue;
        if (earliest == 0 || t.next_run < earliest) {
            earliest = t.next_run;
        }
    }
    return earliest;
}

std::vector<Notification> Scheduler::drain_notifications() {
    std::lock_guard<std::mutex> lock(notif_mutex_);
    std::vector<Notification> out;
    out.swap(pending_notifications_);
    return out;
}
