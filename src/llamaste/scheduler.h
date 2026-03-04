// scheduler.h — Scheduled task and alert management for Llamaste LLM-OS
//
// Manages recurring and one-shot tasks that trigger LLM inference at specific times,
// handles system health alerts (RAM/disk/temp), and notifies clients via SSE.

#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <functional>
#include <thread>
#include <ctime>
#include "json.hpp"

struct ScheduledTask {
    std::string id;
    bool enabled = true;
    std::string prompt;            // what the agent should do
    std::string cron;              // cron expression (optional)
    time_t at = 0;                 // one-shot timestamp (optional)
    int every_seconds = 0;         // interval in seconds (optional)
    bool once = false;             // auto-delete after firing
    time_t created = 0;
    time_t last_run = 0;
    time_t next_run = 0;           // computed
};

struct AlertConfig {
    int ram_threshold_percent = 85;
    int disk_threshold_percent = 90;
    int temp_threshold_c = 80;
    int cooldown_seconds = 300;
};

struct Notification {
    std::string id;
    std::string type;              // "alert", "scheduled", "reminder"
    std::string title;
    std::string body;
    time_t time = 0;
};

// Callback type: called when a scheduled task fires
using SchedulerInferenceFn = std::function<std::string(const std::string& prompt)>;

// Callback type: called when a notification should be pushed to clients
using SchedulerNotifyFn = std::function<void(const Notification& notif)>;

class Scheduler {
public:
    void set_data_dir(const std::string& dir);
    void set_inference_fn(SchedulerInferenceFn fn);
    void set_notify_fn(SchedulerNotifyFn fn);

    // Task CRUD
    std::string create_task(const nlohmann::json& params);
    std::string list_tasks();
    std::string delete_task(const std::string& id);
    std::string update_task(const std::string& id, const nlohmann::json& params);

    // Alert config
    void set_alert_config(const AlertConfig& config);

    // Main loop (runs in its own thread)
    void start();
    void stop();
    ~Scheduler();

    // Get pending notifications (for SSE endpoint)
    std::vector<Notification> drain_notifications();

    // Current stats for /llamaste/system
    int active_task_count() const;
    time_t next_scheduled_time() const;

private:
    void run_loop();
    void check_tasks();
    void check_alerts();
    void fire_task(const ScheduledTask& task);
    void load_tasks();
    void save_tasks();
    time_t compute_next_cron(const std::string& expr, time_t after) const;

    std::string data_dir_;
    SchedulerInferenceFn inference_fn_;
    SchedulerNotifyFn notify_fn_;

    std::vector<ScheduledTask> tasks_;
    AlertConfig alerts_;
    mutable std::mutex mutex_;
    std::atomic<bool> running_{false};
    std::thread thread_;

    // Notification queue
    std::vector<Notification> pending_notifications_;
    std::mutex notif_mutex_;

    // Alert cooldown tracking
    time_t last_ram_alert_ = 0;
    time_t last_disk_alert_ = 0;
    time_t last_temp_alert_ = 0;
};
