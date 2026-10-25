#pragma once

#include <functional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r::AgentControl {

// The handler runs on the transport worker. It must only enqueue/poll GUI work,
// never wait for the GUI thread; stop() is called before GUI teardown.
class RpcServer {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json &)>;
    explicit RpcServer(Handler handler);
    ~RpcServer();
    RpcServer(const RpcServer &) = delete;
    RpcServer &operator=(const RpcServer &) = delete;

    // Lifecycle methods are called by the owning GUI thread, never by Handler.
    bool start(const std::string &runtime_dir, const std::string &session_id,
               const std::string &window_id);
    void stop();
    nlohmann::json descriptor() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Slic3r::AgentControl
