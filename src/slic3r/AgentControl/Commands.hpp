#pragma once

#include <chrono>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <set>
#include <nlohmann/json.hpp>

namespace Slic3r::AgentControl {

// Only copied JSON crosses this boundary. The transport never waits for GUI work.
class Commands
{
public:
    using Json = nlohmann::json;
    struct Work {
        std::string job_id;
        std::string method;
        Json params;
        std::chrono::steady_clock::time_point submitted;
    };

    Commands(std::string session, std::string window);
    Json receive(const Json& request);
    bool take(Work& work, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    void finish(const std::string& job, const Json& result);
    void fail(const std::string& job, const std::string& code, const std::string& message, const Json& details = Json::object());
    void progress(const std::string& job, int percent, const std::string& message);
    void waiting_for_user(const std::string& job, bool waiting);
    bool cancel_queued(const std::string& job);
    void stop();
    Json target() const;
    void register_methods(const std::set<std::string>& methods);

private:
    struct Entry { Work work; Json status; Json request; std::size_t request_bytes; std::size_t status_bytes; };
    void trim(std::size_t incoming_bytes, std::size_t incoming_status_bytes, bool admission, const std::string& protected_job = {});
    void store_status(const std::string& job, Json status);
    std::string m_session, m_window;
    mutable std::mutex m_mutex;
    bool m_stopped = false;
    unsigned long long m_serial = 0;
    std::size_t m_retained_request_bytes = 0;
    std::size_t m_retained_status_bytes = 0;
    std::deque<std::string> m_pending, m_order;
    std::map<std::string, Entry> m_jobs;
    std::map<std::string, std::string> m_requests;
    std::set<std::string> m_methods;
};

} // namespace Slic3r::AgentControl
