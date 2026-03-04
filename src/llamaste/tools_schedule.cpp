// tools_schedule.cpp — Schedule management tools for Llamaste LLM-OS
//
// Provides schedule.create, schedule.list, schedule.delete, schedule.update
// tools that the LLM agent uses to manage recurring and one-shot tasks.

#include "tools.h"
#include "scheduler.h"
#include "json.hpp"

using json = nlohmann::json;

// IMPORTANT: `sched` is captured by reference in lambdas stored in the registry.
// It must outlive the ToolRegistry. Currently satisfied because g_scheduler
// is a file-static in child_main.cpp with program lifetime.
void register_schedule_tools(ToolRegistry& reg, Scheduler& sched) {

    reg.register_tool({
        .name = "schedule.create",
        .description = "Create a scheduled task that triggers LLM inference at a "
                       "specific time or interval. Provide a prompt describing what "
                       "the agent should do when triggered. Schedule with one of: "
                       "cron (5-field cron expression), at (Unix timestamp for one-shot), "
                       "or every_seconds (recurring interval).",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "prompt": {
                    "type": "string",
                    "description": "What the agent should do when this task fires"
                },
                "cron": {
                    "type": "string",
                    "description": "5-field cron expression: minute hour day month weekday (* for any)"
                },
                "at": {
                    "type": "integer",
                    "description": "Unix timestamp for one-shot execution"
                },
                "every_seconds": {
                    "type": "integer",
                    "description": "Recurring interval in seconds"
                },
                "once": {
                    "type": "boolean",
                    "description": "If true, auto-delete after firing (default: false)"
                },
                "enabled": {
                    "type": "boolean",
                    "description": "Whether the task is active (default: true)"
                }
            },
            "required": ["prompt"]
        })json",
        .handler = [&sched](const std::string& args_json) -> std::string {
            auto params = json::parse(args_json, nullptr, false);
            if (params.is_discarded()) {
                json err;
                err["error"] = "Invalid JSON arguments";
                return err.dump();
            }
            return sched.create_task(params);
        }
    });

    reg.register_tool({
        .name = "schedule.list",
        .description = "List all scheduled tasks with their next run times, "
                       "status, and configuration.",
        .parameters = R"json({
            "type": "object",
            "properties": {}
        })json",
        .handler = [&sched](const std::string&) -> std::string {
            return sched.list_tasks();
        }
    });

    reg.register_tool({
        .name = "schedule.delete",
        .description = "Delete a scheduled task by its ID.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "id": {
                    "type": "string",
                    "description": "Task ID to delete"
                }
            },
            "required": ["id"]
        })json",
        .handler = [&sched](const std::string& args_json) -> std::string {
            auto params = json::parse(args_json, nullptr, false);
            if (params.is_discarded()) {
                json err;
                err["error"] = "Invalid JSON arguments";
                return err.dump();
            }
            std::string id = params.value("id", "");
            if (id.empty()) {
                json err;
                err["error"] = "id is required";
                return err.dump();
            }
            return sched.delete_task(id);
        }
    });

    reg.register_tool({
        .name = "schedule.update",
        .description = "Update an existing scheduled task. Only provided fields "
                       "are changed; others remain unchanged.",
        .parameters = R"json({
            "type": "object",
            "properties": {
                "id": {
                    "type": "string",
                    "description": "Task ID to update"
                },
                "prompt": {
                    "type": "string",
                    "description": "New prompt text"
                },
                "enabled": {
                    "type": "boolean",
                    "description": "Enable or disable the task"
                },
                "cron": {
                    "type": "string",
                    "description": "New cron expression"
                },
                "at": {
                    "type": "integer",
                    "description": "New one-shot timestamp"
                },
                "every_seconds": {
                    "type": "integer",
                    "description": "New interval in seconds"
                },
                "once": {
                    "type": "boolean",
                    "description": "Auto-delete after next fire"
                }
            },
            "required": ["id"]
        })json",
        .handler = [&sched](const std::string& args_json) -> std::string {
            auto params = json::parse(args_json, nullptr, false);
            if (params.is_discarded()) {
                json err;
                err["error"] = "Invalid JSON arguments";
                return err.dump();
            }
            std::string id = params.value("id", "");
            if (id.empty()) {
                json err;
                err["error"] = "id is required";
                return err.dump();
            }
            return sched.update_task(id, params);
        }
    });
}
