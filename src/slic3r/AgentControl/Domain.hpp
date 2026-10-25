#include <utility>
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace Slic3r::GUI { class Plater; }
namespace Slic3r::GUI { class PartPlate; }
namespace Slic3r { class ModelVolume; struct GCodeProcessorResult; }

namespace Slic3r::AgentControl {
using Json = nlohmann::json;

struct DomainError : std::runtime_error {
    std::string kind;
    DomainError(std::string code, const std::string& message) : std::runtime_error(message), kind(std::move(code)) {}
};
inline void ensure(bool condition, const char* kind, const char* message)
{ if (!condition) throw DomainError(kind, message); }

struct CommandSpec {
    Json schema;
    bool mutating = false;
};
using CommandSpecs = std::map<std::string, CommandSpec>;
CommandSpec command_spec(bool mutating, Json properties, std::vector<std::string> required = {});
void validate_parameters(const Json& params, const CommandSpec& spec);

// All callbacks are used on the GUI thread. A Job worker computes on copies and
// calls these only from finalize()/call_on_main_thread, never directly in process().
struct DomainContext {
    GUI::Plater& plater;
    Json target;
    std::uint64_t document_epoch;
    std::string state_token;
    std::string operation_id;
    std::weak_ptr<void> lifetime;
    std::function<std::string()> current_state;
    std::function<void(bool)> invalidate;
    std::function<void(Json)> finish;
    std::function<void(std::string, std::string)> fail;
    std::function<void(std::function<void()>)> register_cancel;
};

// Filesystem access is opt-in and enforced in the native endpoint as well as MCP.
// Roots are JSON arrays in ORCA_AGENT_INPUT_ROOTS / ORCA_AGENT_OUTPUT_ROOTS.
std::filesystem::path input_path(const Json& path);
std::filesystem::path output_path(const Json& path, bool overwrite = false);
std::string file_sha256(const std::filesystem::path& path);
std::string mesh_revision(const ModelVolume& volume);

CommandSpecs scene_commands();
Json execute_scene(DomainContext& context, const std::string& method, const Json& params);
Json describe_scene(DomainContext& context);
std::string plate_identity(DomainContext& context, const GUI::PartPlate& plate);
GUI::PartPlate& resolve_plate(DomainContext& context, const Json& params);
CommandSpecs settings_commands();
Json execute_settings(DomainContext& context, const std::string& method, const Json& params);
CommandSpecs geometry_commands();
Json execute_geometry(DomainContext& context, const std::string& method, const Json& params);
Json slice_estimates(const GCodeProcessorResult& result);
Json print_report(DomainContext& context, GUI::PartPlate& plate);
CommandSpecs devices_commands();
Json execute_devices(DomainContext& context, const std::string& method, const Json& params);
CommandSpecs printing_commands();
Json execute_printing(DomainContext& context, const std::string& method, const Json& params);
} // namespace Slic3r::AgentControl
