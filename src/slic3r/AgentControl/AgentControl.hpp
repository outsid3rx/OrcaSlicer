#include <vector>
#include <cstddef>
#include <map>
#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <functional>
#include <utility>
#include <wx/event.h>
#include <wx/timer.h>
#include "Commands.hpp"
#include "RpcServer.hpp"
#include "Domain.hpp"

namespace Slic3r::GUI { class MainFrame; class Plater; }

namespace Slic3r::AgentControl {

// Owned by MainFrame; all methods other than Commands/RpcServer run on the UI thread.
class Controller : public wxEvtHandler
{
public:
    Controller(GUI::MainFrame& frame, GUI::Plater& plater);
    ~Controller();
    void stop();
    bool executing() const { return m_executing || m_critical != 0 || !m_domain_cancels.empty(); }
    void request_close() { m_close_requested = true; }
    void request_recreate(std::function<void()> recreate) { m_recreate = std::move(recreate); }
    void enter() { ++m_critical; }
    void leave() { --m_critical; }
    void invalidate(bool document);
    void on_slice_started(std::uint64_t run_id);
    void on_slice_finished(std::uint64_t run_id, bool success, bool cancelled);
    void on_slice_progress(std::uint64_t run_id, int percent, const std::string& text, bool warning);

private:
    using Json = nlohmann::json;
    void on_timer(wxTimerEvent&);
    bool busy(bool include_slicing = true) const;
    Json execute(const Commands::Work& work);
    std::string state_token() const;
    void check_precondition(const Json& params) const;
    Json slice_result() const;
    DomainContext domain_context(const std::string& job);
    Json start_slice_batch(const Commands::Work& work);
    void advance_slice_batch();
    struct SliceBatch {
        std::string job, child, token;
        std::vector<int> plates;
        std::size_t next = 0;
        Json children = Json::array();
        bool cancel = false;
    };
    std::unique_ptr<SliceBatch> m_slice_batch;

    GUI::MainFrame& m_frame;
    GUI::Plater& m_plater;
    Commands m_commands;
    RpcServer m_server;
    wxTimer m_timer;
    bool m_stopped = false, m_executing = false, m_close_requested = false;
    unsigned m_critical = 0;
    unsigned long long m_document_epoch = 1, m_input_epoch = 1, m_slice_generation = 0;
    std::string m_slice_job, m_slice_token;
    unsigned long long m_owned_slice_generation = 0;
    bool m_launching_slice = false;
    Json m_last_slice;
    Json m_warnings = Json::array();
    std::string m_running_job;
    std::function<void()> m_recreate;
    CommandSpecs m_domain_specs;
    std::map<std::string, std::function<void()>> m_domain_cancels;
    std::shared_ptr<void> m_domain_lifetime = std::make_shared<int>(0);
};

} // namespace Slic3r::AgentControl
