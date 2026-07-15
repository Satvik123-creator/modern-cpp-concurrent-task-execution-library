#include "ctee/server/route_handler.h"
#include "ctee/common/logger.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ctee {

RouteHandler::RouteHandler(ThreadPool& pool, Scheduler& scheduler)
    : pool_(pool), scheduler_(scheduler) {}

namespace {

TaskPriority parse_priority(const json& body, int& error_status, std::string& error_msg) {
    TaskPriority priority = TaskPriority::Normal;
    if (body.contains("priority")) {
        std::string p = body["priority"].get<std::string>();
        if (p == "low") priority = TaskPriority::Low;
        else if (p == "normal") priority = TaskPriority::Normal;
        else if (p == "high") priority = TaskPriority::High;
        else if (p == "critical") priority = TaskPriority::Critical;
        else {
            error_status = 400;
            error_msg = "Invalid priority: " + p;
        }
    }
    return priority;
}

TaskPriority require_priority(const json& body, int& error_status, std::string& error_msg) {
    if (!body.contains("priority")) {
        error_status = 400;
        error_msg = "Missing required field: priority";
        return TaskPriority::Normal;
    }
    return parse_priority(body, error_status, error_msg);
}

Task::FuncType make_task_func(const json& body, const std::string& task_type,
                               int& error_status, std::string& error_msg) {
    if (task_type == "noop") {
        return []() {};
    } else if (task_type == "compute") {
        int iterations = body.value("iterations", 1000000);
        return [iterations]() {
            volatile double x = 0.0;
            for (int i = 0; i < iterations; ++i) {
                x += 1.0 / (i + 1);
            }
        };
    } else if (task_type == "sleep") {
        int ms = body.value("duration_ms", 100);
        return [ms]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        };
    }
    error_status = 400;
    error_msg = "Unknown task_type: " + task_type;
    return nullptr;
}

void send_error(httplib::Response& res, int status, const std::string& msg) {
    json err;
    err["error"] = msg;
    res.status = status;
    res.set_content(err.dump(), "application/json");
}

}  // namespace

// ── POST /tasks ────────────────────────────────────────────────────────

void RouteHandler::submit_task(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const json::parse_error& e) {
        send_error(res, 400, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    if (!body.contains("task_type")) {
        send_error(res, 400, "Missing required field: task_type");
        return;
    }

    std::string task_type = body["task_type"].get<std::string>();
    std::string name = body.value("name", task_type);

    int error_status = 0;
    std::string error_msg;
    TaskPriority priority = parse_priority(body, error_status, error_msg);
    if (error_status) {
        send_error(res, error_status, error_msg);
        return;
    }

    auto func = make_task_func(body, task_type, error_status, error_msg);
    if (error_status) {
        send_error(res, error_status, error_msg);
        return;
    }

    Task task(std::move(func), name, priority);
    std::string task_id = task.id();

    TaskInfo info;
    info.id = task_id;
    info.name = name;
    info.task_type = task_type;
    info.priority = priority;
    info.status = TaskStatus::Queued;
    info.created_at = task.created_at();
    registry_.register_task(task_id, std::move(info));

    try {
        std::ignore = pool_.submit(std::move(task));
        registry_.update_status(task_id, TaskStatus::Queued);
    } catch (const std::exception& e) {
        registry_.update_status(task_id, TaskStatus::Failed);
        registry_.update_error(task_id, e.what());
        send_error(res, 503, e.what());
        return;
    }

    registry_.set_cancel_handler(task_id, [&pool = pool_, task_id]() {
        return pool.cancel(task_id);
    });

    pool_.set_completion_handler(task_id, [&registry = registry_, task_id](TaskStatus status) {
        registry.update_status_if_not_terminal(task_id, status);
    });

    json resp;
    resp["task_id"] = task_id;
    resp["name"] = name;
    resp["task_type"] = task_type;
    resp["status"] = to_string(TaskStatus::Queued);
    res.status = 201;
    res.set_content(resp.dump(), "application/json");
}

// ── POST /tasks/priority ───────────────────────────────────────────────

void RouteHandler::submit_task_priority(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const json::parse_error& e) {
        send_error(res, 400, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    if (!body.contains("task_type")) {
        send_error(res, 400, "Missing required field: task_type");
        return;
    }

    std::string task_type = body["task_type"].get<std::string>();
    std::string name = body.value("name", task_type);

    int error_status = 0;
    std::string error_msg;
    TaskPriority priority = require_priority(body, error_status, error_msg);
    if (error_status) {
        send_error(res, error_status, error_msg);
        return;
    }

    auto func = make_task_func(body, task_type, error_status, error_msg);
    if (error_status) {
        send_error(res, error_status, error_msg);
        return;
    }

    Task task(std::move(func), name, priority);
    std::string task_id = task.id();

    TaskInfo info;
    info.id = task_id;
    info.name = name;
    info.task_type = task_type;
    info.priority = priority;
    info.status = TaskStatus::Queued;
    info.created_at = task.created_at();
    registry_.register_task(task_id, std::move(info));

    try {
        std::ignore = pool_.submit(std::move(task));
        registry_.update_status(task_id, TaskStatus::Queued);
    } catch (const std::exception& e) {
        registry_.update_status(task_id, TaskStatus::Failed);
        registry_.update_error(task_id, e.what());
        send_error(res, 503, e.what());
        return;
    }

    registry_.set_cancel_handler(task_id, [&pool = pool_, task_id]() {
        return pool.cancel(task_id);
    });

    pool_.set_completion_handler(task_id, [&registry = registry_, task_id](TaskStatus status) {
        registry.update_status_if_not_terminal(task_id, status);
    });

    json resp;
    resp["task_id"] = task_id;
    resp["name"] = name;
    resp["task_type"] = task_type;
    resp["priority"] = to_string(priority);
    resp["status"] = to_string(TaskStatus::Queued);
    res.status = 201;
    res.set_content(resp.dump(), "application/json");
}

// ── POST /tasks/schedule ───────────────────────────────────────────────

void RouteHandler::schedule_task(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const json::parse_error& e) {
        send_error(res, 400, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    if (!body.contains("task_type") || !body.contains("delay_ms")) {
        send_error(res, 400, "Missing required fields: task_type, delay_ms");
        return;
    }

    std::string task_type = body["task_type"].get<std::string>();
    int delay_ms = body["delay_ms"].get<int>();
    std::string name = body.value("name", task_type);

    if (delay_ms < 0) {
        send_error(res, 400, "delay_ms must be non-negative");
        return;
    }

    int error_status = 0;
    std::string error_msg;
    TaskPriority priority = parse_priority(body, error_status, error_msg);
    if (error_status) {
        send_error(res, error_status, error_msg);
        return;
    }

    auto func = make_task_func(body, task_type, error_status, error_msg);
    if (error_status) {
        send_error(res, error_status, error_msg);
        return;
    }

    Task task(std::move(func), name, priority);
    std::string task_id = task.id();
    auto when = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms);

    TaskInfo info;
    info.id = task_id;
    info.name = name;
    info.task_type = task_type;
    info.priority = priority;
    info.status = TaskStatus::Pending;
    info.created_at = task.created_at();
    registry_.register_task(task_id, std::move(info));

    std::string scheduled_id = scheduler_.schedule(std::move(task), when);

    registry_.set_cancel_handler(scheduled_id, [&scheduler = scheduler_, scheduled_id]() {
        return scheduler.cancel(scheduled_id);
    });

    json resp;
    resp["task_id"] = scheduled_id;
    resp["name"] = name;
    resp["task_type"] = task_type;
    resp["delay_ms"] = delay_ms;
    resp["status"] = "scheduled";
    res.status = 201;
    res.set_content(resp.dump(), "application/json");
}

// ── DELETE /tasks/{id} ─────────────────────────────────────────────────

void RouteHandler::cancel_task(const httplib::Request& req, httplib::Response& res) {
    std::string task_id = req.matches[1];

    auto info = registry_.get(task_id);
    if (!info) {
        send_error(res, 404, "Task not found: " + task_id);
        return;
    }

    if (info->status == TaskStatus::Completed ||
        info->status == TaskStatus::Failed ||
        info->status == TaskStatus::Cancelled) {
        send_error(res, 409, "Task already finished with status: " +
                   std::string(to_string(info->status)));
        return;
    }

    bool cancelled = registry_.cancel(task_id);

    if (cancelled) {
        pool_.metrics()->record_cancel();
    }

    json resp;
    resp["task_id"] = task_id;
    resp["cancelled"] = cancelled;
    res.set_content(resp.dump(), "application/json");
}

// ── GET /workers ───────────────────────────────────────────────────────

void RouteHandler::get_workers(const httplib::Request& /*req*/, httplib::Response& res) {
    auto workers = pool_.get_worker_infos();
    auto pool_stats = pool_.stats();

    json arr = json::array();
    for (const auto& w : workers) {
        json item;
        item["id"] = w.id;
        item["state"] = w.state;
        item["marked_stop"] = w.marked_stop;
        arr.push_back(std::move(item));
    }

    json resp;
    resp["workers"] = std::move(arr);
    resp["total"] = workers.size();
    resp["min_threads"] = pool_stats.active_threads;
    res.set_content(resp.dump(), "application/json");
}

// ── GET /queue ─────────────────────────────────────────────────────────

void RouteHandler::get_queue(const httplib::Request& /*req*/, httplib::Response& res) {
    auto qi = pool_.queue_info();

    json resp;
    resp["size"] = qi.size;
    resp["capacity"] = qi.capacity;
    resp["is_full"] = qi.is_full;
    res.set_content(resp.dump(), "application/json");
}

// ── GET /metrics ───────────────────────────────────────────────────────

void RouteHandler::get_metrics(const httplib::Request& /*req*/, httplib::Response& res) {
    auto metrics = pool_.metrics();
    auto snap = metrics->snapshot();
    auto hist = metrics->history();

    json resp;
    resp["submitted_tasks"] = snap.submitted_tasks;
    resp["completed_tasks"] = snap.completed_tasks;
    resp["failed_tasks"] = snap.failed_tasks;
    resp["cancelled_tasks"] = snap.cancelled_tasks;
    resp["running_tasks"] = snap.running_tasks;
    resp["queue_length"] = snap.queue_length;
    resp["peak_queue_length"] = snap.peak_queue_length;
    resp["active_threads"] = snap.active_threads;
    resp["avg_wait_time_ms"] = snap.avg_wait_time_ms;
    resp["avg_exec_time_ms"] = snap.avg_exec_time_ms;
    resp["worker_utilization"] = snap.worker_utilization;
    resp["tasks_per_sec"] = snap.tasks_per_sec;
    resp["is_running"] = pool_.is_running();
    resp["is_paused"] = pool_.is_paused();

    json history_arr = json::array();
    for (const auto& h : hist) {
        json entry;
        entry["submitted_tasks"] = h.submitted_tasks;
        entry["completed_tasks"] = h.completed_tasks;
        entry["failed_tasks"] = h.failed_tasks;
        entry["cancelled_tasks"] = h.cancelled_tasks;
        entry["running_tasks"] = h.running_tasks;
        entry["queue_length"] = h.queue_length;
        entry["peak_queue_length"] = h.peak_queue_length;
        entry["active_threads"] = h.active_threads;
        entry["avg_wait_time_ms"] = h.avg_wait_time_ms;
        entry["avg_exec_time_ms"] = h.avg_exec_time_ms;
        entry["worker_utilization"] = h.worker_utilization;
        entry["tasks_per_sec"] = h.tasks_per_sec;
        history_arr.push_back(std::move(entry));
    }
    resp["history"] = std::move(history_arr);

    res.set_content(resp.dump(), "application/json");
}

// ── GET /health ────────────────────────────────────────────────────────

void RouteHandler::get_health(const httplib::Request& /*req*/, httplib::Response& res) {
    json resp;
    resp["status"] = "ok";
    resp["uptime"] = std::chrono::steady_clock::now().time_since_epoch().count();
    resp["pool_running"] = pool_.is_running();
    resp["scheduler_running"] = scheduler_.is_running();
    res.set_content(resp.dump(), "application/json");
}

// ── POST /tasks/batch ────────────────────────────────────────────────

void RouteHandler::submit_batch(const httplib::Request& req, httplib::Response& res) {
    json body;
    try {
        body = json::parse(req.body);
    } catch (const json::parse_error& e) {
        send_error(res, 400, "Invalid JSON: " + std::string(e.what()));
        return;
    }

    if (!body.is_array()) {
        send_error(res, 400, "Request body must be a JSON array");
        return;
    }

    std::vector<Task> tasks;
    std::vector<std::string> task_ids;
    std::vector<std::pair<std::string, std::string>> registrations; // id, name

    for (const auto& item : body) {
        if (!item.contains("task_type")) {
            send_error(res, 400, "Each task must have a task_type field");
            return;
        }

        std::string task_type = item["task_type"].get<std::string>();
        std::string name = item.value("name", task_type);

        int error_status = 0;
        std::string error_msg;
        TaskPriority priority = parse_priority(item, error_status, error_msg);
        if (error_status) {
            send_error(res, error_status, error_msg);
            return;
        }

        auto func = make_task_func(item, task_type, error_status, error_msg);
        if (error_status) {
            send_error(res, error_status, error_msg);
            return;
        }

        Task task(std::move(func), name, priority);
        std::string task_id = task.id();
        task_ids.push_back(task_id);
        registrations.emplace_back(task_id, name);

        TaskInfo info;
        info.id = task_id;
        info.name = name;
        info.task_type = task_type;
        info.priority = priority;
        info.status = TaskStatus::Queued;
        info.created_at = task.created_at();
        registry_.register_task(task_id, std::move(info));

        tasks.push_back(std::move(task));
    }

    std::vector<std::string> submitted_ids = pool_.submit_batch(std::move(tasks));

    for (const auto& tid : submitted_ids) {
        registry_.update_status(tid, TaskStatus::Queued);
        registry_.set_cancel_handler(tid, [&pool = pool_, tid]() {
            return pool.cancel(tid);
        });
        pool_.set_completion_handler(tid, [&registry = registry_, tid](TaskStatus status) {
            registry.update_status_if_not_terminal(tid, status);
        });
    }

    json resp;
    resp["task_ids"] = submitted_ids;
    resp["submitted"] = submitted_ids.size();
    res.status = 201;
    res.set_content(resp.dump(), "application/json");
}

// ── GET /tasks/history ────────────────────────────────────────────────

void RouteHandler::get_task_history(const httplib::Request& req, httplib::Response& res) {
    std::size_t count = 100;
    if (req.has_param("count")) {
        try {
            count = std::stoul(req.get_param_value("count"));
        } catch (...) {
            send_error(res, 400, "Invalid count parameter");
            return;
        }
    }

    auto history = pool_.task_history()->recent(count);

    json arr = json::array();
    for (const auto& rec : history) {
        json entry;
        entry["task_id"] = rec.task_id;
        entry["task_name"] = rec.task_name;
        entry["status"] = to_string(rec.status);
        entry["submitted_at"] = rec.submitted_at.time_since_epoch().count();
        entry["completed_at"] = rec.completed_at.time_since_epoch().count();
        entry["execution_time_ms"] = rec.execution_time.count();
        arr.push_back(std::move(entry));
    }

    json resp;
    resp["history"] = std::move(arr);
    resp["total"] = history.size();
    res.set_content(resp.dump(), "application/json");
}

// ── GET /reports/performance ──────────────────────────────────────────

void RouteHandler::get_performance_report(const httplib::Request& /*req*/, httplib::Response& res) {
    auto metrics = pool_.metrics();
    auto snap = metrics->snapshot();
    auto hist = metrics->history();

    std::size_t total_submitted = snap.submitted_tasks;
    std::size_t total_completed = snap.completed_tasks;
    std::size_t total_failed = snap.failed_tasks;

    double success_rate = 0.0;
    if (total_completed + total_failed > 0) {
        success_rate = static_cast<double>(total_completed) /
                       static_cast<double>(total_completed + total_failed);
    }

    double max_utilization = 0.0;
    double max_tasks_per_sec = 0.0;
    for (const auto& h : hist) {
        if (h.worker_utilization > max_utilization) max_utilization = h.worker_utilization;
        if (h.tasks_per_sec > max_tasks_per_sec) max_tasks_per_sec = h.tasks_per_sec;
    }

    auto now = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration<double>(now - snap.timestamp).count();

    json resp;
    resp["total_submitted"] = total_submitted;
    resp["total_completed"] = total_completed;
    resp["total_failed"] = total_failed;
    resp["success_rate"] = success_rate;
    resp["avg_exec_time_ms"] = snap.avg_exec_time_ms;
    resp["avg_wait_time_ms"] = snap.avg_wait_time_ms;
    resp["tasks_per_sec"] = max_tasks_per_sec;
    resp["uptime_seconds"] = uptime;
    resp["peak_queue_length"] = snap.peak_queue_length;
    resp["max_worker_utilization"] = max_utilization;
    res.set_content(resp.dump(), "application/json");
}

// ── POST /pool/pause ─────────────────────────────────────────────────

void RouteHandler::pause_pool(const httplib::Request& /*req*/, httplib::Response& res) {
    pool_.pause();

    json resp;
    resp["paused"] = pool_.is_paused();
    res.set_content(resp.dump(), "application/json");
}

// ── POST /pool/resume ────────────────────────────────────────────────

void RouteHandler::resume_pool(const httplib::Request& /*req*/, httplib::Response& res) {
    pool_.resume();

    json resp;
    resp["paused"] = pool_.is_paused();
    res.set_content(resp.dump(), "application/json");
}

// ── GET /config ──────────────────────────────────────────────────────

void RouteHandler::get_config(const httplib::Request& /*req*/, httplib::Response& res) {
    json resp;
    resp["pool"]["min_threads"] = 2;
    resp["pool"]["max_threads"] = 8;
    resp["pool"]["queue_capacity"] = 1024;
    resp["pool"]["scale_up_threshold"] = 4;
    resp["server"]["port"] = 8080;
    resp["server"]["host"] = "0.0.0.0";
    resp["default_max_retries"] = 0;
    resp["default_retry_delay_ms"] = 1000;
    resp["default_task_timeout_ms"] = 0;
    resp["max_history_size"] = 1000;
    resp["name_threads"] = true;
    res.set_content(resp.dump(), "application/json");
}

// ── POST /shutdown ─────────────────────────────────────────────────────

void RouteHandler::shutdown_pool(const httplib::Request& /*req*/, httplib::Response& res) {
    json resp;
    resp["status"] = "shutting_down";

    res.set_content(resp.dump(), "application/json");
}

}  // namespace ctee
