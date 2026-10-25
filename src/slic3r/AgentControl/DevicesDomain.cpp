#include "Domain.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <future>
#include <iomanip>
#include <ios>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <wx/event.h>
#include <wx/string.h>

namespace Slic3r::AgentControl {
namespace {
Json text_schema() { return {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}; }
DeviceManager& manager() {
    auto* devices = GUI::wxGetApp().getDeviceManager();
    ensure(devices, "not_ready", "Device manager is unavailable");
    return *devices;
}
MachineObject& machine(const std::string& backend, const std::string& device) {
    auto& devices = manager();
    ensure(devices.get_current_printer_agent_id() == backend, "stale_device", "The explicit printer backend is no longer active");
    const auto known = devices.get_my_machine_list(backend);
    const auto found = known.find(device);
    ensure(found != known.end() && found->second, "not_found", "Device is not known to the selected backend");
    return *found->second;
}
Json tray_info(const DevAmsTray& tray) {
    return {{"slot_id", tray.id}, {"present", tray.is_exists}, {"placeholder", tray.is_slot_placeholder},
        {"material_type", tray.m_fila_type}, {"name", tray.sub_brands}, {"color", tray.color}, {"colors", tray.cols},
        {"filament_id", tray.setting_id}, {"preset_id", tray.filament_setting_id},
        {"remaining_percent", tray.remain >= 0 && tray.remain <= 100 ? Json(tray.remain) : Json()},
        {"nozzle_temperature_min", tray.nozzle_temp_min}, {"nozzle_temperature_max", tray.nozzle_temp_max}};
}
Json inventory(const std::string& backend, MachineObject& device) {
    Json units = Json::array(), external = Json::array(), nozzles = Json::array();
    auto system = device.GetFilaSystem();
    if (system) {
        // The device owns this mapping: AMS Lite mixed and single-slot AMS units
        // do not follow the ordinary ams_id * 4 + slot_id formula.
        const auto indices = system->GetTrayIndexMap();
        for (const auto& entry : system->GetAmsList()) {
            const auto* ams = entry.second;
            if (!ams) continue;
            Json trays = Json::array();
            for (const auto& slot : ams->GetTrays()) {
                if (!slot.second) continue;
                Json tray = tray_info(*slot.second);
                tray["tray_index"] = nullptr;
                for (const auto& index : indices)
                    if (std::to_string(index.second.first) == ams->GetAmsId() && std::to_string(index.second.second) == slot.first)
                        tray["tray_index"] = index.first;
                trays.push_back(std::move(tray));
            }
            units.push_back({{"ams_id", ams->GetAmsId()}, {"name", ams->GetDisplayName().ToStdString()},
                {"type", static_cast<int>(ams->GetAmsType())}, {"present", ams->IsExist()},
                {"extruder_id", ams->GetExtruderId()}, {"bound_extruder_ids", ams->GetBindedExtruderSet()},
                {"humidity_percent", ams->GetHumidityPercent()}, {"slots", std::move(trays)}});
        }
    }
    for (const auto& tray : device.vt_slot) external.push_back(tray_info(tray));
    if (auto* system_nozzle = device.GetNozzleSystem())
        for (const auto& entry : system_nozzle->GetExtNozzles()) {
            const auto& nozzle = entry.second;
            nozzles.push_back({{"extruder_id", entry.first}, {"diameter_mm", nozzle.GetNozzleDiameter()},
                {"type", static_cast<int>(nozzle.GetNozzleType())}, {"flow", DevNozzle::GetNozzleFlowTypeString(nozzle.GetNozzleFlowType())}});
        }
    const auto now = std::chrono::system_clock::now();
    const auto updated = device.last_update_time;
    const bool has_time = updated.time_since_epoch().count() > 0;
    const bool ready = device.is_connected() && device.is_info_ready();
    auto* selected = manager().get_selected_machine();
    return {{"backend_id", backend}, {"device_id", device.get_dev_id()}, {"name", device.get_dev_name()},
        {"selected", selected && selected->get_dev_id() == device.get_dev_id()},
        {"selection_scope", "application_process"}, {"connected", device.is_connected()}, {"telemetry_ready", ready},
        {"telemetry_age_seconds", has_time ? Json(std::chrono::duration_cast<std::chrono::seconds>(now - updated).count()) : Json()},
        {"ams_units", std::move(units)}, {"external_spools", std::move(external)}, {"nozzles", std::move(nozzles)},
        {"inventory_note", "Reported device state; empty/placeholder slots are not usable material. Revalidated when preparing and submitting."}};
}
Json sync_nozzle(DomainContext& ctx, MachineObject& device) {
    ensure(device.is_connected() && device.is_info_ready(), "not_ready", "Wait for connected printer telemetry before synchronizing");
    auto* bundle = GUI::wxGetApp().preset_bundle;
    ensure(bundle, "not_ready", "Preset bundle is unavailable");
    auto& preset = bundle->printers.get_edited_preset();
    ensure(!device.printer_type.empty() && preset.get_printer_type(bundle) == device.get_show_printer_type(),
        "incompatible_model", "Select the matching printer profile before synchronizing its nozzle");
    auto* nozzles = device.GetNozzleSystem();
    auto* extruders = device.GetExtderSystem();
    // Rack choices and left/right mixed diameters require an explicit nozzle
    // selection workflow. Do not enter the native modal chooser implicitly.
    ensure(bundle->get_printer_extruder_count() == 1 && extruders && extruders->GetTotalExtderSize() == 1 &&
        nozzles && nozzles->GetExtNozzles().size() == 1 && nozzles->GetRackNozzles().empty(),
        "unsupported_nozzle_layout", "Automatic nozzle sync currently supports a single installed nozzle without a rack");
    const auto nozzle = nozzles->GetExtNozzles().begin()->second;
    ensure(nozzle.GetNozzleDiameter() > 0 && std::isfinite(nozzle.GetNozzleDiameter()), "not_ready", "Installed nozzle diameter is unknown");
    ensure(ctx.plater.check_printer_initialized(&device, false, false), "not_ready", "Configure the installed nozzle flow type on the printer first");
    ensure(!device.is_nozzle_flow_type_supported() || nozzle.GetNozzleFlowType() != NozzleFlowType::NONE_FLOWTYPE,
        "not_ready", "Installed nozzle flow type is unknown");
    ensure(DevPrinterConfigUtil::is_optional_printer_model_id(device.printer_type) || GUI::get_printer_preset(&device),
        "incompatible_preset", "No installed native printer preset matches this device nozzle");
    std::ostringstream diameter;
    diameter << std::fixed << std::setprecision(1) << nozzle.GetNozzleDiameter();
    const auto* configured = preset.config.option<ConfigOptionFloats>("nozzle_diameter");
    ensure(configured && configured->values.size() == 1, "incompatible_preset", "The current profile has no single nozzle diameter");
    const auto variant = preset.config.opt_string("printer_variant");
    const bool keep_preset = variant == diameter.str() || (variant.empty() && std::abs(configured->values[0] - nozzle.GetNozzleDiameter()) < 1e-5);
    if (!keep_preset) {
        const auto* candidate = bundle->get_similar_printer_preset({}, diameter.str());
        ensure(candidate, "incompatible_preset", "No native nozzle variant matches the installed diameter");
        // The ordinary profile switch may ask to discard edits. Keep automation
        // nonmodal and leave those edits intact; save/select explicitly first.
        ensure(!bundle->printers.get_edited_preset().is_dirty && !bundle->prints.get_edited_preset().is_dirty &&
            !bundle->filaments.get_edited_preset().is_dirty, "preset_change_required", "Save edited presets before synchronizing a different nozzle variant");
    }
    auto& sidebar = ctx.plater.sidebar();
    sidebar.set_need_auto_sync_after_connect_printer(false);
    ensure(sidebar.sync_extruder_list(), "sync_failed", "Native nozzle synchronization did not complete");
    const auto* after = bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    ensure(after && after->values.size() == 1 && std::abs(after->values[0] - nozzle.GetNozzleDiameter()) < 1e-5,
        "sync_failed", "Native profile did not adopt the reported nozzle diameter");
    ctx.invalidate(false);
    return {{"state", "synchronized"}, {"diameter_mm", nozzle.GetNozzleDiameter()},
        {"flow", DevNozzle::GetNozzleFlowTypeString(nozzle.GetNozzleFlowType())},
        {"printer_preset", bundle->printers.get_edited_preset().name}, {"project_filaments_replaced", false}};
}
struct Selection {
    std::atomic<bool> cancelled{false};
    bool ready = false;
    std::exception_ptr error;
};
Json synchronize_when_ready(DomainContext& ctx, const Json& params) {
    const auto backend = params.at("backend_id").get<std::string>();
    const auto device_id = params.at("device_id").get<std::string>();
    auto state = std::make_shared<Selection>();
    auto context = std::make_shared<DomainContext>(ctx);
    const auto expected_state = ctx.current_state();
    const auto timeout = params.value("timeout_seconds", 30);
    ctx.register_cancel([state] { state->cancelled.store(true); });
    ensure(GUI::queue_job(ctx.plater.get_ui_job_worker(),
        [state, context, backend, device_id, timeout](GUI::Job::Ctl& ctl) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
            while (!state->ready) {
                if (ctl.was_canceled() || state->cancelled.load() || context->lifetime.expired()) { state->cancelled.store(true); throw DomainError("cancelled", "Nozzle synchronization cancelled; device selection is retained"); }
                auto pending = ctl.call_on_main_thread([state, context, backend, device_id] {
                    if (state->cancelled.load() || context->lifetime.expired()) return;
                    try {
                    auto& device = machine(backend, device_id);
                    auto* selected = manager().get_selected_machine();
                    ensure(selected && selected->get_dev_id() == device_id, "stale_device", "Selected device changed while waiting for nozzle sync");
                    state->ready = device.is_connected() && device.is_info_ready();
                    } catch (...) { state->error = std::current_exception(); state->ready = true; }
                });
                // Do not retain a worker waiting for a GUI callback during teardown.
                while (pending.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
                    if (ctl.was_canceled() || state->cancelled.load()) { state->cancelled.store(true); throw DomainError("cancelled", "Nozzle synchronization cancelled; device selection is retained"); }
                }
                pending.get();
                if (state->ready) break;
                ensure(std::chrono::steady_clock::now() < deadline, "not_ready", "Printer telemetry did not become ready; selection is retained. Retry printers.sync_nozzle after connection completes");
                if (ctl.was_canceled() || state->cancelled.load()) throw DomainError("cancelled", "Nozzle synchronization cancelled; device selection is retained");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        },
        [state, context, backend, device_id, expected_state](bool cancelled, std::exception_ptr& exception) {
            if (context->lifetime.expired()) { exception = nullptr; return; }
            try {
                if (exception) { const auto error = exception; exception = nullptr; std::rethrow_exception(error); }
                if (state->error) std::rethrow_exception(state->error);
                ensure(!cancelled && !state->cancelled.load(), "cancelled", "Nozzle synchronization cancelled; device selection is retained");
                ensure(context->current_state() == expected_state, "conflict", "Project changed before nozzle sync; selection is retained");
                auto& device = machine(backend, device_id);
                auto* selected = manager().get_selected_machine();
                ensure(selected && selected->get_dev_id() == device_id, "stale_device", "Device selection changed before nozzle sync");
                auto result = inventory(backend, device);
                try { result["nozzle_sync"] = sync_nozzle(*context, device); }
                catch (const DomainError& error) { result["nozzle_sync"] = {{"state", "blocked"}, {"error", {{"kind", error.kind}, {"message", error.what()}}}}; }
                context->finish(result);
            } catch (const DomainError& error) { context->fail(error.kind, error.what()); }
              catch (const std::exception& error) { context->fail("device_error", error.what()); }
        }), "busy", "Printer synchronization worker is unavailable");
    return nullptr;
}
} // namespace

CommandSpecs devices_commands() {
    CommandSpecs commands;
    const Json identity = {{"backend_id", text_schema()}, {"device_id", text_schema()}};
    commands.emplace("printers.filaments", command_spec(false, identity, {"backend_id", "device_id"}));
    Json props = identity;
    props["timeout_seconds"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 60}};
    commands.emplace("printers.sync_nozzle", command_spec(true, props, {"backend_id", "device_id"}));
    props["sync_nozzle"] = {{"type", "boolean"}, {"default", true}};
    commands.emplace("printers.select", command_spec(true, props, {"backend_id", "device_id"}));
    return commands;
}
Json execute_devices(DomainContext& ctx, const std::string& method, const Json& params) {
    const auto backend = params.at("backend_id").get<std::string>();
    auto& device = machine(backend, params.at("device_id").get<std::string>());
    if (method == "printers.filaments") return inventory(backend, device);
    if (method == "printers.select") {
        auto& devices = manager();
        auto* current = devices.get_selected_machine();
        if (!current || current->get_dev_id() != device.get_dev_id()) {
            // Use the Device tab's complete synchronous selection path. The
            // public select_machine queues an event, too late for this result.
            if (auto* monitor = GUI::MonitorPanel::if_built()) {
                wxCommandEvent event(wxEVT_COMMAND_CHOICE_SELECTED);
                event.SetString(wxString::FromUTF8(device.get_dev_id()));
                monitor->on_select_printer(event);
            } else {
                ensure(devices.set_selected_machine(device.get_dev_id()), "selection_failed", "Native device selection failed");
                GUI::MonitorPanel::on_machine_selected(devices.get_selected_machine());
            }
        }
        current = devices.get_selected_machine();
        ensure(current && current->get_dev_id() == device.get_dev_id(), "selection_failed", "Native device selection did not select the requested printer");
        ctx.plater.sidebar().set_need_auto_sync_after_connect_printer(false);
        ctx.invalidate(false);
        if (!params.value("sync_nozzle", true)) {
            auto result = inventory(backend, device);
            result["nozzle_sync"] = {{"state", "not_requested"}};
            return result;
        }
    } else ensure(method == "printers.sync_nozzle", "unsupported", "Unknown printer selection action");
    return synchronize_when_ready(ctx, params);
}
} // namespace Slic3r::AgentControl
