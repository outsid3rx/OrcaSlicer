#pragma once
#include <cstdint>
#include <string>
#include <functional>

namespace Slic3r::GUI { class Plater; }

namespace Slic3r::AgentControl {

#ifdef SLIC3R_AGENT_CONTROL
void enter_critical(GUI::Plater* plater);
void leave_critical(GUI::Plater* plater);
void document_changed(GUI::Plater* plater);
void inputs_changed(GUI::Plater* plater);
void slice_started(GUI::Plater* plater, std::uint64_t run_id);
void slice_finished(GUI::Plater* plater, std::uint64_t run_id, bool success, bool cancelled);
void slice_progress(GUI::Plater* plater, std::uint64_t run_id, int percent, const std::string& text, bool warning);
bool defer_close(GUI::Plater* plater);
bool defer_recreate(GUI::Plater* plater, std::function<void()> recreate);
#else
inline void enter_critical(GUI::Plater*) {}
inline void leave_critical(GUI::Plater*) {}
inline void document_changed(GUI::Plater*) {}
inline void inputs_changed(GUI::Plater*) {}
inline void slice_started(GUI::Plater*, std::uint64_t) {}
inline void slice_finished(GUI::Plater*, std::uint64_t, bool, bool) {}
inline void slice_progress(GUI::Plater*, std::uint64_t, int, const std::string&, bool) {}
inline bool defer_close(GUI::Plater*) { return false; }
inline bool defer_recreate(GUI::Plater*, std::function<void()>) { return false; }
#endif

// Guards serialization/import and nested wx event loops, including manual GUI operations.
class OperationBlock
{
public:
    explicit OperationBlock(GUI::Plater* plater) : m_plater(plater) { enter_critical(plater); }
    ~OperationBlock() { leave_critical(m_plater); }
    OperationBlock(const OperationBlock&) = delete;
    OperationBlock& operator=(const OperationBlock&) = delete;
private:
    GUI::Plater* m_plater;
};

} // namespace Slic3r::AgentControl
