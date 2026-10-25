#include "Commands.hpp"
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <set>
#include <algorithm>
#include <vector>

namespace Slic3r::AgentControl {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_request_bytes = 64 * 1024;
// Serialized size accounts for both retained copies. Parsed JSON allocator
// overhead is larger, so bounding only the number of jobs is insufficient.
constexpr std::size_t max_retained_request_bytes = 4 * 1024 * 1024;
// Images and geometry diagnostics make job count alone a poor memory bound.
// Include the whole status (errors/progress as well as successful results).
constexpr std::size_t max_status_bytes = 3 * 1024 * 1024;
constexpr std::size_t max_retained_status_bytes = 16 * 1024 * 1024;
std::string progress_text(const std::string& message)
{
    std::size_t end = std::min<std::size_t>(message.size(), 4096);
    // Do not split a valid UTF-8 code point at the byte budget boundary.
    if (end < message.size())
        while (end > 0 && (static_cast<unsigned char>(message[end]) & 0xc0) == 0x80) --end;
    return message.substr(0, end);
}
Json error(const Json& id, int code, const std::string& kind, const std::string& message)
{
    return {{"jsonrpc", "2.0"}, {"id", id},
            {"error", {{"code", code}, {"message", message}, {"data", {{"kind", kind}}}}}};
}
Json result(const Json& id, Json value)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(value)}};
}
bool terminal(const Json& status)
{
    const auto state = status.value("state", "");
    return state == "succeeded" || state == "failed" || state == "cancelled";
}
}

Commands::Commands(std::string session, std::string window)
    : m_session(std::move(session)), m_window(std::move(window)),
      m_methods{"system.describe", "scene.describe", "settings.describe", "settings.get",
                "settings.patch", "slicing.start", "slicing.result", "jobs.cancel"} {}

void Commands::register_methods(const std::set<std::string>& methods)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_methods.insert(methods.begin(), methods.end());
}

Commands::Json Commands::target() const
{
    return {{"session_id", m_session}, {"window_id", m_window}};
}

Commands::Json Commands::receive(const Json& request)
{
    Json id = request.is_object() ? request.value("id", Json()) : Json();
    if (!request.is_object() || request.value("jsonrpc", Json()) != "2.0" ||
        !(id.is_string() || id.is_number_integer()) || !request.contains("method") ||
        !request["method"].is_string() || !request.value("params", Json::object()).is_object())
        return error(id, -32600, "invalid_request", "A JSON-RPC request with id and object params is required");
    if (id.is_string() && (id.get_ref<const std::string&>().empty() || id.get_ref<const std::string&>().size() > 128))
        return error(nullptr, -32600, "invalid_request", "Request id must contain 1 to 128 characters");
    const std::size_t request_bytes = request.dump().size();
    if (request_bytes > max_request_bytes)
        return error(id, -32600, "request_too_large", "Requests may contain at most 64 KiB of serialized JSON");
    const std::string method = request["method"];
    const Json params = request.value("params", Json::object());
    if (method != "system.describe" && params.value("target", Json()) != target())
        return error(id, -32001, "stale_target", "Select this session and window explicitly");

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopped)
        return error(id, -32000, "closing", "This window is closing");
    if (method == "jobs.get") {
        if (!params.contains("job_id") || !params["job_id"].is_string())
            return error(id, -32602, "invalid_params", "job_id is required");
        auto found = m_jobs.find(params["job_id"].get<std::string>());
        if (found == m_jobs.end())
            return error(id, -32004, "unknown_job", "The job is unknown or its retained result expired");
        Json value = found->second.status;
        value["target"] = target();
        return result(id, std::move(value));
    }
    if (m_methods.count(method) == 0)
        return error(id, -32601, "unknown_method", "Unknown method");
    const std::string request_key = id.dump();
    auto previous = m_requests.find(request_key);
    if (previous != m_requests.end()) {
        const auto& entry = m_jobs.at(previous->second);
        if (entry.request != request)
            return error(id, -32600, "request_conflict", "This request id was already used with another payload");
        return result(id, {{"job_id", entry.work.job_id}, {"target", target()}});
    }
    const std::size_t retained_bytes = request_bytes + params.dump().size();
    const std::string job = m_window + ":" + std::to_string(m_serial + 1);
    Json initial_status {{"job_id", job}, {"state", "queued"}};
    const std::size_t status_bytes = initial_status.dump().size();
    trim(retained_bytes, status_bytes, true);
    if (m_jobs.size() >= 256 || retained_bytes > max_retained_request_bytes - m_retained_request_bytes ||
        status_bytes > max_retained_status_bytes - m_retained_status_bytes)
        return error(id, -32000, "busy", "The bounded job history or memory budget is full; finish existing jobs first");
    ++m_serial;
    Work work {job, method, params,
               std::chrono::steady_clock::now()};
    m_jobs.emplace(job, Entry{std::move(work), std::move(initial_status), request, retained_bytes, status_bytes});
    m_retained_request_bytes += retained_bytes;
    m_retained_status_bytes += status_bytes;
    m_requests.emplace(request_key, job);
    // A queued cancellation must be examined before its target, not after it executes.
    if (method == "jobs.cancel") m_pending.push_front(job);
    else m_pending.push_back(job);
    m_order.push_back(job);
    return result(id, {{"job_id", job}, {"target", target()}});
}

void Commands::trim(std::size_t incoming_bytes, std::size_t incoming_status_bytes, bool admission, const std::string& protected_job)
{
    // Preserve live work and the status being replaced. Removing a terminal job
    // also releases its request-ID dedup entry, exactly like count-based eviction.
    for (auto it = m_order.begin(); it != m_order.end() &&
         ((admission && m_jobs.size() >= 256) || incoming_bytes > max_retained_request_bytes - m_retained_request_bytes ||
          incoming_status_bytes > max_retained_status_bytes - m_retained_status_bytes);) {
        auto found = m_jobs.find(*it);
        if (*it == protected_job || !terminal(found->second.status)) { ++it; continue; }
        m_requests.erase(found->second.request["id"].dump());
        m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), *it), m_pending.end());
        m_retained_request_bytes -= found->second.request_bytes;
        m_retained_status_bytes -= found->second.status_bytes;
        m_jobs.erase(found);
        it = m_order.erase(it);
    }
}

void Commands::store_status(const std::string& job, Json status)
{
    auto found = m_jobs.find(job);
    if (found == m_jobs.end()) return;
    std::size_t bytes = status.dump().size();
    if (bytes > max_status_bytes) {
        // The operation has already run. Never imply that oversized delivery
        // rolled it back or invite a new mutation after a response-size failure.
        status = {{"job_id", job}, {"state", "failed"}, {"operation_may_have_completed", true},
            {"error", {{"kind", "result_too_large"},
                {"message", "The operation may have completed, but its status exceeded 3 MiB; inspect current state or receipt and do not repeat a mutation"}}}};
        bytes = status.dump().size();
    }
    const std::size_t previous = found->second.status_bytes;
    m_retained_status_bytes -= previous;
    trim(0, bytes, false, job);
    // Live statuses are small (bounded progress text); older terminal results
    // are evictable, so the per-status limit always fits the aggregate budget.
    found->second.status = std::move(status);
    found->second.status_bytes = bytes;
    m_retained_status_bytes += bytes;
}

bool Commands::take(Work& work, std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_stopped && !m_pending.empty()) {
        const std::string job = m_pending.front();
        m_pending.pop_front();
        auto it = m_jobs.find(job);
        if (it == m_jobs.end() || terminal(it->second.status)) continue;
        if (now - it->second.work.submitted > std::chrono::seconds(15)) {
            Json status = it->second.status;
            status.update({{"state", "failed"}, {"error", {{"kind", "queue_timeout"},
                {"message", "The command expired in the queue and was not executed"}}}});
            store_status(job, std::move(status));
            continue;
        }
        Json status = it->second.status; status["state"] = "running";
        store_status(job, std::move(status));
        work = it->second.work;
        return true;
    }
    return false;
}

void Commands::finish(const std::string& job, const Json& result_value)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(job);
    if (it != m_jobs.end() && !terminal(it->second.status)) {
        Json status = it->second.status;
        status.erase("waiting_for_user");
        status.update({{"state", "succeeded"}, {"result", result_value}});
        store_status(job, std::move(status));
    }
}

void Commands::fail(const std::string& job, const std::string& code, const std::string& message, const Json& details)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(job);
    if (it != m_jobs.end() && !terminal(it->second.status)) {
        Json status = it->second.status;
        status.erase("waiting_for_user");
        status.update({{"state", code == "cancelled" ? "cancelled" : "failed"},
                       {"error", {{"kind", code}, {"message", message}}}});
        if (!details.empty()) status["error"]["details"] = details;
        store_status(job, std::move(status));
    }
}

bool Commands::cancel_queued(const std::string& job)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(job);
    if (it == m_jobs.end() || it->second.status["state"] != "queued") return false;
    Json status = it->second.status; status["state"] = "cancelled";
    store_status(job, std::move(status));
    return true;
}

void Commands::progress(const std::string& job, int percent, const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(job);
    if (it != m_jobs.end() && it->second.status["state"] == "running") {
        Json status = it->second.status;
        if (percent >= 0) status["progress_percent"] = std::min(100, percent);
        status["message"] = progress_text(message);
        store_status(job, std::move(status));
    }
}

void Commands::waiting_for_user(const std::string& job, bool waiting)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_jobs.find(job);
    if (it != m_jobs.end() && !terminal(it->second.status)) {
        Json status = it->second.status; status["waiting_for_user"] = waiting;
        store_status(job, std::move(status));
    }
}

void Commands::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stopped = true;
    m_pending.clear();
    std::vector<std::string> active;
    for (const auto& entry : m_jobs) if (!terminal(entry.second.status)) active.push_back(entry.first);
    for (const auto& job : active) {
        Json status = m_jobs.at(job).status;
        status.update({{"state", "failed"}, {"error", {{"kind", "closing"},
                {"message", "The window closed; a running operation may have completed"}}}});
        store_status(job, std::move(status));
    }
}

} // namespace Slic3r::AgentControl
