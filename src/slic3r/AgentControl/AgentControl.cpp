#include <slic3r/AgentControl/Domain.hpp>
#include "AgentControl.hpp"
#include "Hooks.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <boost/log/trivial.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <openssl/evp.h>
#include <wx/modalhook.h>
#include <wx/timer.h>
#include <wx/filename.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintBase.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r::AgentControl {
namespace {
using Json = nlohmann::json;
std::map<GUI::Plater*, Controller*> controllers; // UI-thread only, never used by transport.
Controller* find(GUI::Plater* plater)
{
    auto it = controllers.find(plater);
    return it == controllers.end() ? nullptr : it->second;
}
std::string uuid() { return boost::uuids::to_string(boost::uuids::random_generator()()); }
const std::string& process_session() { static const std::string id = uuid(); return id; }

struct CommandError : std::runtime_error {
    std::string kind;
    CommandError(std::string kind, const std::string& message)
        : std::runtime_error(message), kind(std::move(kind)) {}
};
void require(bool condition, const char* kind, const char* message)
{
    if (!condition) throw CommandError(kind, message);
}

Json config_json(const ConfigBase& config)
{
    Json out = Json::object();
    for (const auto& key : config.keys())
        if (const auto* option = config.option(key)) out[key] = option->serialize();
    return out;
}
Json plate_input_config(GUI::PartPlate& plate)
{
    Json config = config_json(*plate.config());
    const auto mode = plate.get_real_filament_map_mode(GUI::wxGetApp().preset_bundle->project_config);
    // Automatic assignment writes these caches after slicing. Manual mappings
    // remain true inputs and must still invalidate an operation on change.
    if (mode < FilamentMapMode::fmmManual) {
        config.erase("filament_map");
        config.erase("filament_volume_map");
    }
    if (mode != FilamentMapMode::fmmNozzleManual) config.erase("filament_nozzle_map");
    return config;
}
bool modal_open()
{
    // Native OS file pickers are not necessarily present in wxTopLevelWindows.
    return wxModalDialogHook::GetOpenCount() != 0;
}

}

void enter_critical(GUI::Plater* plater) { if (auto* c = find(plater)) c->enter(); }
void leave_critical(GUI::Plater* plater) { if (auto* c = find(plater)) c->leave(); }
void document_changed(GUI::Plater* plater) { if (auto* c = find(plater)) c->invalidate(true); }
void inputs_changed(GUI::Plater* plater) { if (auto* c = find(plater)) c->invalidate(false); }
void slice_started(GUI::Plater* plater, std::uint64_t run_id) { if (auto* c = find(plater)) c->on_slice_started(run_id); }
void slice_finished(GUI::Plater* plater, std::uint64_t run_id, bool success, bool cancelled)
{ if (auto* c = find(plater)) c->on_slice_finished(run_id, success, cancelled); }
void slice_progress(GUI::Plater* plater, std::uint64_t run_id, int percent, const std::string& text, bool warning)
{ if (auto* c = find(plater)) c->on_slice_progress(run_id, percent, text, warning); }
bool defer_close(GUI::Plater* plater)
{
    if (auto* c = find(plater); c && c->executing()) { c->request_close(); return true; }
    return false;
}
bool defer_recreate(GUI::Plater* plater, std::function<void()> recreate)
{
    if (auto* c = find(plater); c && c->executing()) { c->request_recreate(std::move(recreate)); return true; }
    return false;
}

Controller::Controller(GUI::MainFrame& frame, GUI::Plater& plater)
    : m_frame(frame), m_plater(plater), m_commands(process_session(), uuid()),
      m_server([this](const Json& request) { return m_commands.receive(request); }), m_timer(this)
{
    std::set<std::string> extra_methods;
    for (const auto& group : {scene_commands(), settings_commands(), geometry_commands(), printing_commands(), devices_commands()})
        for (const auto& item : group) { m_domain_specs[item.first] = item.second; extra_methods.insert(item.first); }
    m_domain_specs["slicing.start_many"] = command_spec(true,
        {{"plate_ids", {{"type", "array"}, {"minItems", 1}, {"maxItems", 128}, {"uniqueItems", true},
            {"items", {{"type", "string"}}}}}}, {"plate_ids"});
    extra_methods.insert("slicing.start_many");
    m_commands.register_methods(extra_methods);
    const auto target = m_commands.target();
    if (!m_server.start(data_dir() + "/cache", target["session_id"], target["window_id"])) {
        m_stopped = true;
        BOOST_LOG_TRIVIAL(error) << "Agent control could not start; no endpoint was published";
        return;
    }
    controllers.emplace(&m_plater, this);
    Bind(wxEVT_TIMER, &Controller::on_timer, this);
    m_timer.Start(100);
}

Controller::~Controller() { stop(); }
void Controller::stop()
{
    if (m_stopped) return;
    m_stopped = true;
    m_domain_lifetime.reset();
    m_timer.Stop();
    m_commands.stop(); // No wait for UI callbacks: the server only enqueues/polls copied data.
    m_server.stop();
    controllers.erase(&m_plater);
}

DomainContext Controller::domain_context(const std::string& job)
{
    const std::weak_ptr<void> alive = m_domain_lifetime;
    return {m_plater, m_commands.target(), m_document_epoch, state_token(), job, alive,
        [this, alive] { return alive.expired() ? std::string() : state_token(); },
        [this, alive](bool document) { if (!alive.expired()) invalidate(document); },
        [this, alive, job](Json result) {
            if (alive.expired()) return;
            m_domain_cancels.erase(job);
            if (result.is_object()) result["state_token"] = state_token();
            m_commands.finish(job, result);
        },
        [this, alive, job](std::string kind, std::string message) {
            if (alive.expired()) return;
            m_domain_cancels.erase(job);
            m_commands.fail(job, kind, message);
        },
        [this, alive, job](std::function<void()> cancel) {
            if (!alive.expired()) m_domain_cancels[job] = std::move(cancel);
        }};
}

void Controller::invalidate(bool document)
{
    ++m_input_epoch;
    if (document) ++m_document_epoch;
    m_last_slice = nullptr;
    if (!m_slice_job.empty() && !m_launching_slice) {
        m_commands.fail(m_slice_job, "superseded", "The document or settings changed during slicing");
        m_slice_job.clear(); // Never cancel a replacement job started by the user.
    }
}

bool Controller::busy(bool include_slicing) const
{
    return m_critical != 0 || GUI::wxGetApp().is_closing() || modal_open() ||
           m_plater.is_loading_project() || m_plater.inside_snapshot_capture() ||
           !m_plater.get_ui_job_worker().is_idle() || m_plater.is_export_gcode_scheduled() ||
           m_plater.background_process().is_upload_scheduled() ||
           (include_slicing && m_plater.background_process().running()) ||
           m_plater.get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(false);
}

std::string Controller::state_token() const
{
    // Include manual edits and undo/redo, not just the remote command counter. Configs,
    // transforms and raw mesh bytes are read only on the main thread; no live pointer escapes.
    auto* context = EVP_MD_CTX_new();
    if (!context) throw std::runtime_error("Cannot allocate state digest");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> owner(context, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1)
        throw std::runtime_error("Cannot initialize state digest");
    auto add = [context](const void* bytes, size_t size) {
        if (size && EVP_DigestUpdate(context, bytes, size) != 1) throw std::runtime_error("Cannot hash state");
    };
    auto string = [&](const std::string& value) { const size_t size = value.size(); add(&size, sizeof(size)); add(value.data(), size); };
    string(std::to_string(m_document_epoch));
    string(std::to_string(m_input_epoch));
    string(std::to_string(m_plater.model().id().id));
    auto& plates = m_plater.get_partplate_list();
    string(std::to_string(plates.get_curr_plate_index()));
    string(config_json(GUI::wxGetApp().preset_bundle->full_config()).dump());
    string(GUI::wxGetApp().preset_bundle->prints.get_edited_preset().name);
    for (int i = 0; i < plates.get_plate_count(); ++i) {
        auto* plate = plates.get_plate(i);
        int print_index = -1;
        PrintBase* print = nullptr;
        GUI::GCodeResult* result = nullptr;
        plate->get_print(&print, &result, &print_index);
        add(&print_index, sizeof(print_index));
        string(plate_input_config(*plate).dump());
        string(plate->get_plate_name());
        const bool locked = plate->is_locked(); add(&locked, sizeof(locked));
        const auto origin = plates.get_plate(i)->get_origin(); add(origin.data(), sizeof(double) * 3);
    }
    for (const auto& material : m_plater.model().materials) {
        string(material.first);
        string(material.second ? config_json(material.second->config.get()).dump() : std::string());
    }
    for (const auto* object : m_plater.model().objects) {
        string(std::to_string(object->id().id));
        string(object->name);
        add(&object->auto_drop, sizeof(object->auto_drop));
        string(config_json(object->config.get()).dump());
        add(&object->printable, sizeof(object->printable));
        string(std::to_string(object->layer_height_profile.timestamp()));
        for (const auto& range : object->layer_config_ranges) {
            add(&range.first.first, sizeof(double)); add(&range.first.second, sizeof(double));
            string(config_json(range.second.get()).dump());
        }
        for (const auto* instance : object->instances) {
            string(std::to_string(instance->id().id));
            const auto matrix = instance->get_matrix().matrix();
            add(matrix.data(), sizeof(double) * 16);
            add(&instance->printable, sizeof(instance->printable));
        }
        for (const auto* volume : object->volumes) {
            string(std::to_string(volume->id().id));
            string(volume->name);
            string(volume->material_id());
            string(config_json(volume->config.get()).dump());
            string(std::to_string(volume->supported_facets.timestamp()));
            string(std::to_string(volume->seam_facets.timestamp()));
            string(std::to_string(volume->mmu_segmentation_facets.timestamp()));
            string(std::to_string(volume->fuzzy_skin_facets.timestamp()));
            string(std::to_string(static_cast<int>(volume->type())));
            const auto matrix = volume->get_matrix().matrix();
            add(matrix.data(), sizeof(double) * 16);
            const auto& mesh = volume->mesh().its;
            string(std::to_string(mesh.vertices.size()));
            string(std::to_string(mesh.indices.size()));
            add(mesh.vertices.data(), mesh.vertices.size() * sizeof(mesh.vertices.front()));
            add(mesh.indices.data(), mesh.indices.size() * sizeof(mesh.indices.front()));
        }
    }
    unsigned char digest[EVP_MAX_MD_SIZE]; unsigned length = 0;
    if (EVP_DigestFinal_ex(context, digest, &length) != 1) throw std::runtime_error("Cannot finish digest");
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned i = 0; i < length; ++i) { out += hex[digest[i] >> 4]; out += hex[digest[i] & 15]; }
    return out;
}

void Controller::check_precondition(const Json& params) const
{
    require(params.contains("state_token") && params["state_token"].is_string(),
            "invalid_params", "Read scene/settings and supply state_token before modifying or slicing");
    require(params["state_token"].get<std::string>() == state_token(), "conflict",
            "The project, selection or settings changed; read the state again");
}

Controller::Json Controller::execute(const Commands::Work& work)
{
    const auto& params = work.params;
    if (work.method == "jobs.cancel") {
        validate_parameters(params, command_spec(false, {{"job_id", {{"type", "string"}}}}, {"job_id"}));
        const auto owned = m_domain_cancels.find(params.value("job_id", std::string()));
        if (owned != m_domain_cancels.end()) {
            const auto cancel = owned->second;
            cancel();
            return {{"cancel_requested", true}, {"job_id", params.at("job_id")}};
        }
        const std::string job = params.at("job_id").get<std::string>();
        if (m_commands.cancel_queued(job)) return {{"cancelled", true}};
        require(job == m_slice_job && m_owned_slice_generation == m_slice_generation,
                "unknown_job", "Only this module's queued commands or active slice can be cancelled");
        m_plater.background_process().stop();
        m_commands.fail(m_slice_job, "cancelled", "Cancelled by client");
        m_slice_job.clear();
        return {{"cancelled", true}};
    }
    const auto domain = m_domain_specs.find(work.method);
    if (domain != m_domain_specs.end()) {
        validate_parameters(params, domain->second);
        const bool printing_observation = !domain->second.mutating && (printing_commands().count(work.method) || devices_commands().count(work.method));
        require(printing_observation || !busy(domain->second.mutating), "busy", "Finish the active edit, slice or export before this operation");
        require(!domain->second.mutating || !m_slice_batch, "busy", "An explicit plate slice batch is active");
        require(m_plater.printer_technology() == ptFFF, "unsupported", "Agent control currently supports FFF projects");
        if (domain->second.mutating) check_precondition(params);
        auto context = domain_context(work.job_id);
        Json result;
        if (work.method == "slicing.start_many") return start_slice_batch(work);
        if (settings_commands().count(work.method)) result = execute_settings(context, work.method, params);
        else if (scene_commands().count(work.method)) result = execute_scene(context, work.method, params);
        else if (geometry_commands().count(work.method)) result = execute_geometry(context, work.method, params);
        else if (devices_commands().count(work.method)) result = execute_devices(context, work.method, params);
        else result = execute_printing(context, work.method, params);
        if (!result.is_null()) {
            if (domain->second.mutating && !printing_commands().count(work.method))
                invalidate(scene_commands().count(work.method) != 0);
            result["state_token"] = state_token();
            if (work.method == "scene.describe") {
                result["target"] = m_commands.target();
                result["document_epoch"] = m_document_epoch;
                result["project"] = GUI::into_u8(m_plater.get_project_filename(".3mf").AfterLast(wxFileName::GetPathSeparator()));
                result["window_title"] = GUI::into_u8(m_frame.GetTitle());
                result["dirty"] = m_plater.is_project_dirty();
            }
        }
        return result;
    }
    static const std::map<std::string, std::set<std::string>> parameters {
        {"system.describe", {"target"}}, {"scene.describe", {"target"}},
        {"settings.describe", {"target"}},
        {"settings.get", {"target", "scope", "object_id"}},
        {"settings.patch", {"target", "scope", "object_id", "state_token", "values", "unset"}},
        {"slicing.start", {"target", "state_token", "plate_index"}},
        {"slicing.result", {"target", "job_id"}}, {"jobs.cancel", {"target", "job_id"}}
    };
    const auto spec = parameters.find(work.method);
    require(spec != parameters.end(), "unknown_method", "Unknown method");
    for (const auto& item : params.items())
        require(spec->second.count(item.key()) != 0, "invalid_params", "Unknown parameter; check the command schema");
    if (params.contains("object_id"))
        require(params.value("scope", Json()) == "object", "invalid_scope", "object_id requires explicit object scope");
    if (work.method == "system.describe") {
        const Json fields {
            {"target", {{"type", "object"}, {"additionalProperties", false},
                {"properties", {{"session_id", {{"type", "string"}, {"format", "uuid"}}},
                                {"window_id", {{"type", "string"}, {"format", "uuid"}}}}},
                {"required", {"session_id", "window_id"}}}},
            {"scope", {{"type", "string"}, {"enum", {"process", "object"}}}},
            {"object_id", {{"type", "string"}}},
            {"state_token", {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}},
            {"values", {{"type", "object"}, {"additionalProperties", {{"type", "string"}}}}},
            {"unset", {{"type", "array"}, {"items", {{"type", "string"}}}, {"uniqueItems", true}}},
            {"plate_index", {{"type", "integer"}, {"minimum", 0}}},
            {"job_id", {{"type", "string"}}}
        };
        Json schemas = Json::object();
        auto all_parameters = parameters;
        all_parameters["jobs.get"] = {"target", "job_id"};
        for (const auto& method : all_parameters) {
            Json properties = Json::object(), required = Json::array();
            for (const auto& key : method.second) properties[key] = fields.at(key);
            if (method.first != "system.describe") required.push_back("target");
            if (method.first == "settings.patch") { required.push_back("scope"); required.push_back("state_token"); }
            if (method.first == "slicing.start") { required.push_back("plate_index"); required.push_back("state_token"); }
            if (method.first == "jobs.get" || method.first == "jobs.cancel" || method.first == "slicing.result") required.push_back("job_id");
            schemas[method.first] = {{"type", "object"}, {"properties", properties},
                                   {"additionalProperties", false}, {"required", required}};
        }
        Json capabilities = {"scene.describe", "settings.describe", "settings.get", "settings.patch",
                             "slicing.start", "slicing.result", "jobs.get", "jobs.cancel"};
        Json effects = Json::object();
        for (const auto& item : m_domain_specs) {
            schemas[item.first] = item.second.schema;
            if (std::find(capabilities.begin(), capabilities.end(), item.first) == capabilities.end()) capabilities.push_back(item.first);
            effects[item.first] = {{"mutating", item.second.mutating}};
        }
        return {{"protocol_version", 1}, {"api_revision", 3}, {"app_version", SoftFever_VERSION}, {"target", m_commands.target()},
                {"capabilities", capabilities}, {"operation_effects", effects},
                {"method_params_schemas", schemas},
                {"scopes", {"process", "plate", "object", "volume", "height_range"}}, {"values_format", "Orca serialized strings"},
                {"jobs", "Every command returns job_id; poll jobs.get for its outcome. Unique request IDs deduplicate retained jobs."}};
    }
    require(!busy(false), "busy", "The scene is currently being modified or exported");
    require(m_plater.printer_technology() == ptFFF, "unsupported", "Agent control currently supports FFF projects");
    if (work.method == "slicing.start") {
        require(!m_slice_batch || work.job_id == m_slice_batch->child, "busy", "A plate slice batch is active");
        require(!busy(), "busy", "Another slice or operation is active");
        check_precondition(params);
        require(params.contains("plate_index") && params["plate_index"].is_number_integer() &&
                params["plate_index"] == m_plater.get_partplate_list().get_curr_plate_index(),
                "stale_target", "Only the explicitly selected current plate can be sliced");
        require(!m_plater.model().objects.empty(), "invalid_model", "No model is loaded");
        ++m_input_epoch; // Prevent re-execution after bounded request history eviction.
        m_last_slice = nullptr;
        m_slice_job = work.job_id;
        m_slice_token = state_token();
        m_owned_slice_generation = 0;
        m_warnings = Json::array();
        m_launching_slice = true;
        try { m_plater.slice_current_plate(); } catch (...) { m_launching_slice = false; m_slice_job.clear(); throw; }
        m_launching_slice = false;
        require(m_owned_slice_generation != 0 && !m_slice_job.empty(), "slice_not_started", "Orca did not start slicing this plate");
        m_slice_token = state_token();
        return nullptr; // Completed only by the native process completion hook.
    }
    if (work.method == "slicing.result") {
        require(!m_last_slice.is_null() && params.value("job_id", std::string()) == m_last_slice.value("job_id", std::string()),
                "stale_result", "Supply a completed job_id for the current unchanged inputs");
        require(m_last_slice["state_token"] == state_token(), "stale_result", "The inputs changed after this slice");
        return m_last_slice;
    }
    throw CommandError("unknown_method", "Unknown method");
}

void Controller::on_timer(wxTimerEvent&)
{
    if (m_executing) {
        m_commands.waiting_for_user(m_running_job, modal_open());
        if (m_slice_batch && m_running_job == m_slice_batch->child)
            m_commands.waiting_for_user(m_slice_batch->job, modal_open());
        return;
    }
    if (m_stopped || m_critical || GUI::wxGetApp().is_closing()) return;
    if (m_close_requested) {
        if (!m_domain_cancels.empty()) {
            const auto cancels = m_domain_cancels;
            for (const auto& item : cancels) item.second();
            advance_slice_batch();
            return;
        }
        m_close_requested = false; m_frame.Close(); return;
    }
    if (m_recreate) {
        if (!m_domain_cancels.empty()) {
            const auto cancels = m_domain_cancels;
            for (const auto& item : cancels) item.second();
            advance_slice_batch();
            return;
        }
        auto recreate = std::move(m_recreate); recreate(); return;
    }
    advance_slice_batch();
    Commands::Work work;
    if (!m_commands.take(work)) return;
    m_executing = true;
    m_running_job = work.job_id;
    const auto fail_command = [&](const std::string& kind, const std::string& message) {
        // A domain only transfers asynchronous ownership when execute returns null.
        // Failed admission must not retain a cancellation entry or defer close forever.
        const auto found = m_domain_cancels.find(work.job_id);
        if (found != m_domain_cancels.end()) {
            const auto cancel = found->second;
            m_domain_cancels.erase(found);
            try { cancel(); } catch (...) {}
        }
        if (work.job_id == m_slice_job) m_slice_job.clear();
        m_commands.fail(work.job_id, kind, message);
    };
    try {
        Json result = execute(work);
        if (!result.is_null()) m_commands.finish(work.job_id, result);
    } catch (const DomainError& error) {
        fail_command(error.kind, error.what());
    } catch (const CommandError& error) {
        fail_command(error.kind, error.what());
    } catch (const std::exception& error) {
        fail_command("invalid_params", error.what());
    } catch (...) { fail_command("internal_error", "Unexpected operation failure"); }
    m_executing = false;
    m_running_job.clear();
}

Controller::Json Controller::start_slice_batch(const Commands::Work& work)
{
    auto batch = std::make_unique<SliceBatch>();
    batch->job = work.job_id;
    batch->token = state_token();
    auto context = domain_context(work.job_id);
    for (const auto& handle : work.params.at("plate_ids")) {
        auto& plate = resolve_plate(context, {{"plate_id", handle}});
        batch->plates.push_back(plate.get_index());
    }
    m_slice_batch = std::move(batch);
    m_domain_cancels[work.job_id] = [this] {
        if (!m_slice_batch) return;
        m_slice_batch->cancel = true;
        if (!m_slice_batch->child.empty()) {
            if (!m_commands.cancel_queued(m_slice_batch->child) &&
                m_slice_job == m_slice_batch->child && m_owned_slice_generation == m_slice_generation)
                m_plater.background_process().stop();
        }
    };
    return nullptr;
}

void Controller::advance_slice_batch()
{
    if (!m_slice_batch) return;
    auto& batch = *m_slice_batch;
    const auto finish = [this](const std::string& kind, const std::string& message) {
        const auto job = m_slice_batch->job;
        Json details {{"children", m_slice_batch->children}, {"pending_child_job_id", m_slice_batch->child}};
        m_domain_cancels.erase(job);
        m_slice_batch.reset();
        m_commands.fail(job, kind, message, details);
    };
    try {
        if (!batch.child.empty()) {
            const auto reply = m_commands.receive({{"jsonrpc", "2.0"}, {"id", "batch-observe"},
                {"method", "jobs.get"}, {"params", {{"target", m_commands.target()}, {"job_id", batch.child}}}});
            require(reply.contains("result"), "unknown_job", "A batch child receipt expired");
            const auto status = reply.at("result");
            const auto state = status.value("state", std::string());
            m_commands.waiting_for_user(batch.job, status.value("waiting_for_user", false));
            if (state == "queued" || state == "running") return;
            batch.children.push_back(status);
            batch.child.clear();
            if (state != "succeeded") {
                finish(state == "cancelled" ? "cancelled" : "slice_failed", "A child slice failed; inspect its job receipt");
                return;
            }
            batch.token = status.at("result").at("state_token");
        }
        if (batch.cancel) { finish("cancelled", "The plate slice batch was cancelled"); return; }
        require(batch.token == state_token(), "superseded", "The document changed between batch slices");
        if (batch.next == batch.plates.size()) {
            Json result {{"children", batch.children}, {"state_token", state_token()}, {"refresh_scene", true}};
            const auto job = batch.job;
            m_domain_cancels.erase(job);
            m_slice_batch.reset();
            m_commands.finish(job, result);
            return;
        }
        require(!busy(), "busy", "Another native operation interrupted the plate batch");
        const int index = batch.plates.at(batch.next++);
        require(index >= 0 && index < m_plater.get_partplate_list().get_plate_count(), "stale_target", "A requested plate no longer exists");
        m_plater.select_plate(index, false);
        m_plater.update();
        batch.token = state_token();
        const auto reply = m_commands.receive({{"jsonrpc", "2.0"},
            {"id", batch.job + ":plate:" + std::to_string(batch.next)}, {"method", "slicing.start"},
            {"params", {{"target", m_commands.target()}, {"plate_index", index}, {"state_token", batch.token}}}});
        require(reply.contains("result"), "queue_full", "Could not admit the next batch slice");
        batch.child = reply.at("result").at("job_id");
        m_commands.progress(batch.job, int(100 * (batch.next - 1) / batch.plates.size()), "Slicing explicit plate set");
    } catch (const CommandError& error) { finish(error.kind, error.what()); }
      catch (const std::exception& error) { finish("batch_failed", error.what()); }
}

void Controller::on_slice_started(std::uint64_t run_id)
{
    m_slice_generation = run_id;
    if (m_launching_slice) m_owned_slice_generation = m_slice_generation;
    else if (!m_slice_job.empty()) {
        m_commands.fail(m_slice_job, "superseded", "A different slice replaced this job");
        m_slice_job.clear();
    }
}

void Controller::on_slice_progress(std::uint64_t run_id, int percent, const std::string& text, bool warning)
{
    if (m_slice_job.empty() || m_owned_slice_generation != run_id) return;
    m_commands.progress(m_slice_job, percent, text);
    if (warning && !text.empty() && m_warnings.size() < 128 &&
        std::none_of(m_warnings.begin(), m_warnings.end(), [&](const Json& value) { return value["message"] == text; }))
        m_warnings.push_back({{"message", text}, {"source", "slicing"}, {"level", 1}, {"code", ""}, {"params", Json::array()}});
}

Controller::Json Controller::slice_result() const
{
    const auto* result = m_plater.background_process().get_current_gcode_result();
    require(result != nullptr, "no_result", "The slicer did not produce a result");
    double volume = 0;
    for (const auto& entry : result->print_statistics.total_volumes_per_extruder) volume += entry.second;
    Json warnings = m_warnings;
    for (const auto& warning : result->warnings)
        warnings.push_back({{"message", warning.msg}, {"source", "gcode"}, {"level", warning.level},
                            {"code", warning.error_code}, {"params", warning.params}});
    return {{"time_seconds", result->print_statistics.modes[0].time}, {"filament_volume_mm3", volume}, {"estimates", slice_estimates(*result)},
            {"warnings", std::move(warnings)}};
}

void Controller::on_slice_finished(std::uint64_t run_id, bool success, bool cancelled)
{
    if (m_slice_job.empty() || m_owned_slice_generation != run_id) return;
    const std::string job = std::exchange(m_slice_job, {});
    try {
        if (cancelled) m_commands.fail(job, "cancelled", "Slicing was cancelled");
        else if (!success) m_commands.fail(job, "slice_failed", "Orca rejected or failed this slice; inspect its diagnostic");
        else if (m_slice_token != state_token()) m_commands.fail(job, "superseded", "The inputs changed during slicing");
        else {
            m_last_slice = slice_result();
            m_last_slice["job_id"] = job;
            m_last_slice["state_token"] = m_slice_token;
            m_last_slice["plate_index"] = m_plater.get_partplate_list().get_curr_plate_index();
            m_commands.finish(job, m_last_slice);
        }
    } catch (const std::exception& error) { m_commands.fail(job, "result_error", error.what()); }
}

} // namespace Slic3r::AgentControl
