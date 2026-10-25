#include "Domain.hpp"

#include "libslic3r/Config.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/BackgroundSlicingProcess.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <boost/beast/core/detail/base64.hpp>
#include <igl/Hit.h>

namespace Slic3r::AgentControl {
namespace {
const std::set<std::string> legacy_keys {
    "layer_height", "wall_loops", "top_shell_layers", "bottom_shell_layers", "sparse_infill_density",
    "sparse_infill_pattern", "outer_wall_speed", "inner_wall_speed", "sparse_infill_speed",
    "internal_solid_infill_speed", "top_surface_speed", "enable_support", "support_on_build_plate_only",
    "support_threshold_angle", "support_type"
};

Json text() { return {{"type", "string"}, {"maxLength", 256}}; }
Json bounded_integer(int maximum) { return {{"type", "integer"}, {"minimum", 0}, {"maximum", maximum}}; }
Json page_properties()
{
    return {{"offset", bounded_integer(1000000)}, {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}},
        {"filter", text()}, {"keys", {{"type", "array"}, {"maxItems", 100}, {"uniqueItems", true}, {"items", text()}}}};
}
Json owner_properties()
{
    return {{"scope", {{"type", "string"}, {"enum", {"process", "plate", "object", "volume", "height_range"}}}},
        {"plate_index", bounded_integer(1000000)}, {"plate_id", text()}, {"object_id", text()}, {"volume_id", text()},
        {"range", {{"type", "array"}, {"minItems", 2}, {"maxItems", 2},
                   {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 100000}}}}}};
}

bool ordinary_key(const std::string& key)
{
    const auto* def = print_config_def.get(key);
    if (!def || def->is_code || def->readonly || def->is_plugin_backed()) return false;
    // These names and native metadata describe executable content, connectivity or
    // preset bookkeeping. Neither the general settings nor preset editor expose it.
    for (const char* word : {"gcode", "script", "post_process", "host", "network", "password", "token", "apikey",
                             "api_key", "credential", "plugin", "printer_agent", "cloud", "certificate", "proxy"})
        if (key.find(word) != std::string::npos) return false;
    static const std::set<std::string> metadata {
        "inherits", "compatible_printers", "compatible_prints", "compatible_printers_condition", "compatible_prints_condition",
        "printer_technology", "printer_settings_id", "print_settings_id", "filament_settings_id", "filament_ids",
        "default_print_profile", "default_filament_profile", "default_filament_colour", "printer_model", "printer_variant",
        "print_extruder_id", "print_extruder_variant", "printer_extruder_id", "printer_extruder_variant",
        "filament_extruder_variant", "bed_custom_model", "bed_custom_texture"
    };
    return metadata.count(key) == 0;
}

std::set<std::string> key_set(const std::vector<std::string>& input)
{
    std::set<std::string> out;
    for (const auto& key : input) if (ordinary_key(key)) out.insert(key);
    return out;
}
const std::set<std::string>& scope_keys(const std::string& scope)
{
    static const std::map<std::string, std::set<std::string>> scopes = [] {
        std::map<std::string, std::set<std::string>> result;
        result["process"] = key_set(Preset::print_options());
        result["plate"] = key_set(GUI::TabPrintPlate::agent_plate_keys());
        result["volume"] = key_set(PrintRegionConfig().keys());
        result["height_range"] = result["volume"]; result["height_range"].insert("layer_height");
        result["object"] = key_set(PrintObjectConfig().keys());
        result["object"].insert(result["volume"].begin(), result["volume"].end());
        return result;
    }();
    const auto found = scopes.find(scope);
    if (found != scopes.end()) return found->second;
    throw DomainError("invalid_scope", "Use process, plate, object, volume or height_range; instances have no independent print settings");
}

struct Owner {
    std::string scope;
    GUI::PartPlate* plate = nullptr;
    ModelObject* object = nullptr;
    ModelVolume* volume = nullptr;
    t_layer_height_range range {0, 0};
    ModelConfig* model_config = nullptr;
    const DynamicPrintConfig* config = nullptr;
};

Owner owner(DomainContext& ctx, const Json& params, bool create_range = false)
{
    Owner out;
    out.scope = params.value("scope", "process");
    scope_keys(out.scope); // Reject unknown scopes even when no owner is requested.
    if (params.contains("plate_id")) {
        out.plate = &resolve_plate(ctx, params);
        ensure(!params.contains("plate_index") || params.at("plate_index").get<int>() == out.plate->get_index(),
               "stale_target", "plate_id and plate_index must identify the same plate");
    } else {
        auto& plates = ctx.plater.get_partplate_list();
        const int index = params.value("plate_index", plates.get_curr_plate_index());
        ensure(index >= 0 && index < plates.get_plate_count(), "stale_target", "The requested plate does not exist");
        out.plate = plates.get_plate(index);
    }
    if (out.scope == "process") {
        ensure(!params.contains("object_id") && !params.contains("volume_id") && !params.contains("range"), "invalid_scope", "Process settings do not accept model owners");
        out.config = &GUI::wxGetApp().preset_bundle->prints.get_edited_preset().config;
        return out;
    }
    if (out.scope == "plate") {
        ensure(!params.contains("object_id") && !params.contains("volume_id") && !params.contains("range"), "invalid_scope", "Plate settings do not accept model owners");
        out.config = out.plate->config(); return out;
    }
    ensure(params.contains("object_id"), "invalid_params", "object_id is required for model settings");
    for (auto* obj : ctx.plater.model().objects)
        if (std::to_string(obj->id().id) == params.at("object_id").get<std::string>()) out.object = obj;
    ensure(out.object != nullptr, "stale_target", "The object is no longer in this document");
    const auto objects = out.plate->get_objects_on_this_plate();
    ensure(std::find(objects.begin(), objects.end(), out.object) != objects.end(), "invalid_scope", "The object has no instance on the explicit plate");
    if (out.scope == "volume") {
        ensure(params.contains("volume_id") && !params.contains("range"), "invalid_params", "Volume scope requires volume_id and no range");
        for (auto* vol : out.object->volumes)
            if (std::to_string(vol->id().id) == params.at("volume_id").get<std::string>()) out.volume = vol;
        ensure(out.volume != nullptr, "stale_target", "The volume is no longer part of this object");
        ensure(out.volume->is_model_part() || out.volume->is_modifier(), "invalid_scope", "Only model parts and parameter modifiers own region settings");
        out.model_config = &out.volume->config;
    } else if (out.scope == "height_range") {
        ensure(params.contains("range") && !params.contains("volume_id"), "invalid_params", "Height scope requires an explicit [lower,upper) range and no volume_id");
        out.range = {params.at("range").at(0).get<double>(), params.at("range").at(1).get<double>()};
        ensure(std::isfinite(out.range.first) && std::isfinite(out.range.second) && out.range.first >= 0 && out.range.second > out.range.first,
               "invalid_range", "Height range must have finite nonnegative lower and greater upper bounds");
        auto found = out.object->layer_config_ranges.find(out.range);
        if (found != out.object->layer_config_ranges.end()) out.model_config = &found->second;
        else {
            ensure(create_range, "stale_target", "The height range does not exist; use settings.patch with values to create it");
            for (const auto& entry : out.object->layer_config_ranges)
                ensure(out.range.second <= entry.first.first || out.range.first >= entry.first.second,
                       "invalid_range", "Creating overlapping height ranges is not supported");
        }
    } else {
        ensure(!params.contains("volume_id") && !params.contains("range"), "invalid_scope", "Object settings do not accept volume or range owners");
        out.model_config = &out.object->config;
    }
    if (out.model_config) out.config = &out.model_config->get();
    return out;
}

const DynamicPrintConfig* material_config(const ModelVolume& volume)
{
    if (volume.material_id().empty()) return nullptr;
    const auto* material = volume.material();
    ensure(material != nullptr, "invalid_config", "Volume refers to a material missing from the current document");
    return &material->config.get();
}

DynamicPrintConfig effective(const Owner& target)
{
    DynamicPrintConfig config = GUI::wxGetApp().preset_bundle->full_config();
    config.apply(*target.plate->config());
    if (target.object) config.apply(target.object->config.get());
    if (target.scope == "height_range" && target.config) config.apply(*target.config);
    if (target.volume) {
        config.apply(target.volume->config.get());
        if (const auto* material = material_config(*target.volume)) config.apply(*material);
    }
    return config;
}

std::vector<std::string> page(const std::set<std::string>& available, const Json& params, Json& pagination, bool legacy)
{
    std::vector<std::string> filtered;
    const auto filter = params.value("filter", std::string());
    const auto requested = params.value("keys", Json::array());
    for (const auto& key : requested)
        ensure(available.count(key.get<std::string>()) != 0, "forbidden_setting", "A requested key is not advertised in this scope");
    for (const auto& key : available) {
        if (!requested.empty() && std::find(requested.begin(), requested.end(), Json(key)) == requested.end()) continue;
        const auto* def = print_config_def.get(key);
        if (!filter.empty() && key.find(filter) == std::string::npos && def->category.find(filter) == std::string::npos && def->label.find(filter) == std::string::npos) continue;
        filtered.push_back(key);
    }
    const auto offset = params.value("offset", std::size_t(0));
    const auto limit = params.value("limit", std::size_t(80));
    const auto begin = std::min(offset, filtered.size()), end = std::min(begin + limit, filtered.size());
    std::vector<std::string> result(filtered.begin() + begin, filtered.begin() + end);
    pagination = {{"offset", begin}, {"limit", limit}, {"total", filtered.size()},
                  {"next_offset", end < filtered.size() ? Json(end) : Json()}};
    // v1 clients expect the original fifteen keys on an unfiltered first read.
    // They remain in the legacy map, alongside the wider paginated surface.
    if (legacy && offset == 0 && filter.empty() && requested.empty()) {
        for (const auto& key : legacy_keys)
            if (available.count(key) && std::find(result.begin(), result.end(), key) == result.end()) result.push_back(key);
        pagination["legacy_keys_included"] = true;
    }
    return result;
}

Json describe_settings(const Json& params)
{
    const auto scope = params.value("scope", "process");
    Json pagination, values = Json::object();
    for (const auto& key : page(scope_keys(scope), params, pagination, scope == "process" || scope == "object")) {
        const auto* def = print_config_def.get(key);
        Json scopes = Json::array();
        for (const auto& name : {"process", "plate", "object", "volume", "height_range"})
            if (scope_keys(name).count(key)) scopes.push_back(name);
        values[key] = {{"type", int(def->type)}, {"min", std::isfinite(def->min) ? Json(def->min) : Json()},
            {"max", std::isfinite(def->max) ? Json(def->max) : Json()}, {"enum_values", def->enum_values}, {"enum_labels", def->enum_labels},
            {"label", def->label}, {"description", def->tooltip}, {"category", def->category}, {"units", def->sidetext},
            {"nullable", def->nullable}, {"scopes", scopes}, {"writable", true}, {"object_override", scope_keys("object").count(key) != 0}};
    }
    return {{"settings", values}, {"scope", scope}, {"pagination", pagination}, {"serialization", "Use native serialized strings; percent includes %, booleans are 0 or 1"}};
}

Json read_settings(DomainContext& ctx, const Json& params)
{
    const Owner target = owner(ctx, params);
    const auto config = effective(target);
    Json pagination, values = Json::object();
    for (const auto& key : page(scope_keys(target.scope), params, pagination, true)) {
        const auto* opt = config.option(key);
        if (!opt) continue;
        const auto* own = target.config ? target.config->option(key) : nullptr;
        std::string source = "process";
        Json chain = Json::array({{{"scope", "process"}, {"present", GUI::wxGetApp().preset_bundle->prints.get_edited_preset().config.has(key)}}});
        const auto add = [&](const char* scope, const ConfigBase& level) {
            const bool present = level.has(key); chain.push_back({{"scope", scope}, {"present", present}}); if (present) source = scope;
        };
        add("plate", *target.plate->config());
        if (target.object) add("object", target.object->config.get());
        if (target.scope == "height_range" && target.config) add("height_range", *target.config);
        if (target.volume) {
            add("volume", target.volume->config.get());
            if (const auto* material = material_config(*target.volume)) add("material", *material);
        }
        values[key] = {{"value", opt->serialize()}, {"own_value", own ? Json(own->serialize()) : Json()},
            {"overridden", target.scope != "process" && own != nullptr}, {"source", source}, {"inheritance", chain}};
    }
    Json result {{"scope", target.scope}, {"object_id", target.object ? Json(std::to_string(target.object->id().id)) : Json()},
        {"plate_index", target.plate->get_index()}, {"plate_id", plate_identity(ctx, *target.plate)},
        {"volume_id", target.volume ? Json(std::to_string(target.volume->id().id)) : Json()},
        {"preset_name", GUI::wxGetApp().preset_bundle->prints.get_edited_preset().name}, {"values", values},
        {"pagination", pagination}, {"state_token", ctx.state_token}};
    if (target.scope == "height_range") result["range"] = {target.range.first, target.range.second};
    return result;
}

void validate_option(const ConfigOptionDef& def, const ConfigOption& option)
{
    if (option.type() == coInt && (def.opt_key == "extruder" || def.opt_key == "support_filament" ||
        def.opt_key == "support_interface_filament" || def.opt_key.find("_filament_id") != std::string::npos)) {
        ensure(option.getInt() >= 0 && std::size_t(option.getInt()) <= GUI::wxGetApp().preset_bundle->filament_presets.size(),
               "invalid_value", "Material assignments must reference an existing 1-based filament slot, or zero for inheritance");
    }
    const auto number = [&](double value, bool percent = false) {
        ensure(std::isfinite(value), "invalid_value", "Numeric settings must be finite");
        // Native ranges for FloatOrPercent describe the absolute representation;
        // percentages are resolved and checked by native cross-setting validation.
        ensure(percent ? value >= 0 : value >= def.min && value <= def.max,
               "invalid_value", "A setting value is outside its native range");
    };
    if (option.is_vector()) {
        const auto& vec = dynamic_cast<const ConfigOptionVectorBase&>(option);
        const auto* default_vec = dynamic_cast<const ConfigOptionVectorBase*>(def.default_value.get());
        ensure((vec.size() > 0 || (default_vec && default_vec->empty())) && vec.size() <= 4096,
               "invalid_value", "Vector settings require bounded values; only natively optional vectors may be empty");
        if (const auto* floats = dynamic_cast<const ConfigOptionVector<double>*>(&option))
            for (std::size_t i = 0; i < vec.size(); ++i) { if (!vec.is_nil(i)) number(floats->values[i]); }
        else if (const auto* ints = dynamic_cast<const ConfigOptionVector<int>*>(&option))
            for (std::size_t i = 0; i < vec.size(); ++i) { if (!vec.is_nil(i)) number(ints->values[i]); }
        else if (const auto* ratios = dynamic_cast<const ConfigOptionVector<FloatOrPercent>*>(&option))
            for (std::size_t i = 0; i < vec.size(); ++i) { if (!vec.is_nil(i)) number(ratios->values[i].value, ratios->values[i].percent); }
        else if (const auto* points = dynamic_cast<const ConfigOptionPoints*>(&option))
            for (const auto& point : points->values) ensure(point.allFinite(), "invalid_value", "Point coordinates must be finite");
        else if (const auto* groups = dynamic_cast<const ConfigOptionPointsGroups*>(&option))
            for (const auto& group : groups->values) {
                ensure(group.size() <= 4096, "invalid_value", "Point groups must be bounded");
                for (const auto& point : group) ensure(point.allFinite(), "invalid_value", "Point coordinates must be finite");
            }
        else if (const auto* groups = dynamic_cast<const ConfigOptionIntsGroups*>(&option))
            for (const auto& group : groups->values) {
                ensure(group.size() <= 4096, "invalid_value", "Integer groups must be bounded");
                for (int value : group) number(value);
            }
    } else if (!option.is_nil()) {
        if (option.type() == coFloatOrPercent) {
            const auto& ratio = dynamic_cast<const ConfigOptionFloatOrPercent&>(option); number(ratio.value, ratio.percent);
        } else if (option.type() == coFloat || option.type() == coPercent) number(option.getFloat());
        else if (option.type() == coInt) number(option.getInt());
        else if (option.type() == coPoint) ensure(dynamic_cast<const ConfigOptionPoint&>(option).value.allFinite(), "invalid_value", "Point coordinates must be finite");
        else if (option.type() == coPoint3) ensure(dynamic_cast<const ConfigOptionPoint3&>(option).value.allFinite(), "invalid_value", "Point coordinates must be finite");
    }
}

void validate_config(DynamicPrintConfig& config, const std::vector<std::string>& changed)
{
    for (const auto& key : changed) {
        const auto* opt = config.option(key); const auto* def = print_config_def.get(key);
        if (opt && def) validate_option(*def, *opt);
    }
    const auto errors = config.validate(true);
    if (!errors.empty()) throw DomainError("invalid_config", errors.begin()->first + ": " + errors.begin()->second);
}

// Recompute every affected inheritance chain before touching live owners. Each
// object can have instances on several plates, and volume/range combinations.
void validate_chains(DomainContext& ctx, const Owner& target, const DynamicPrintConfig& replacement,
                     const std::vector<std::string>& changed)
{
    DynamicPrintConfig base = GUI::wxGetApp().preset_bundle->full_config();
    if (target.scope == "process" || target.scope == "preset") base.apply(replacement);
    validate_config(base, changed);
    for (auto* plate : ctx.plater.get_partplate_list().get_plate_list()) {
        if (target.scope == "plate" && plate != target.plate) continue;
        DynamicPrintConfig plate_config(base);
        plate_config.apply(target.scope == "plate" ? replacement : *plate->config());
        validate_config(plate_config, changed);
        for (auto* object : plate->get_objects_on_this_plate()) {
            if (target.object && object != target.object) continue;
            DynamicPrintConfig object_config(plate_config);
            object_config.apply(target.scope == "object" ? replacement : object->config.get());
            validate_config(object_config, changed);
            for (auto* volume : object->volumes) {
                if (!volume->is_model_part() && !volume->is_modifier()) continue;
                DynamicPrintConfig volume_config(object_config);
                volume_config.apply(target.scope == "volume" && volume == target.volume ? replacement : volume->config.get());
                if (const auto* material = material_config(*volume)) volume_config.apply(*material);
                validate_config(volume_config, changed);
                for (const auto& range : object->layer_config_ranges) {
                    DynamicPrintConfig range_config(volume_config);
                    range_config.apply(target.scope == "height_range" && range.first == target.range ? replacement : range.second.get());
                    validate_config(range_config, changed);
                }
                if (target.scope == "height_range" && target.model_config == nullptr) {
                    DynamicPrintConfig range_config(volume_config); range_config.apply(replacement); validate_config(range_config, changed);
                }
            }
        }
    }
}

void refresh_model(DomainContext& ctx, ModelObject* object, ModelVolume* volume = nullptr, bool ranges = false)
{
    if (object && GUI::wxGetApp().obj_list()) {
        auto* list = GUI::wxGetApp().obj_list();
        list->object_config_options_changed({object, volume});
        if (ranges) {
            auto* model = list->GetModel();
            const auto item = model->GetObjectItem(object);
            const auto layers = model->GetLayerRootItem(item);
            if (layers.IsOk()) model->Delete(layers);
            list->add_layer_root_item(item);
        }
    }
    if (auto* tab = dynamic_cast<GUI::TabPrintModel*>(GUI::wxGetApp().get_model_tab(false))) tab->update_model_config();
    if (auto* tab = dynamic_cast<GUI::TabPrintModel*>(GUI::wxGetApp().get_model_tab(true))) tab->update_model_config();
    auto full = GUI::wxGetApp().preset_bundle->full_config();
    GUI::wxGetApp().mainframe->on_config_changed(&full);
    for (auto* plate : ctx.plater.get_partplate_list().get_plate_list()) plate->update_slice_result_valid_state(false);
    if (GUI::wxGetApp().params_panel()) GUI::wxGetApp().params_panel()->notify_object_config_changed();
}

Json patch_settings(DomainContext& ctx, const Json& params)
{
    Owner target = owner(ctx, params, true);
    const auto allowed = scope_keys(target.scope);
    const Json values = params.value("values", Json::object()), unset = params.value("unset", Json::array());
    ensure(values.size() + unset.size() > 0 && values.size() + unset.size() <= 256,
           "invalid_params", "Supply between 1 and 256 settings in values/unset");
    ensure(target.scope != "process" || unset.empty(), "invalid_scope", "Process settings inherit from the selected preset; unset is for overrides");
    ensure(target.config || !values.empty(), "invalid_params", "Creating a height range requires values");
    DynamicPrintConfig candidate = target.config ? *target.config : DynamicPrintConfig();
    if (!target.config && target.scope == "height_range") candidate.apply_only(effective(target), {"layer_height"});
    // Native ObjectList only creates layer rows when this inheritance sentinel
    // exists. Match get_default_layer_config, including for older malformed ranges.
    if (target.scope == "height_range" && !candidate.has("extruder"))
        candidate.set_key_value("extruder", new ConfigOptionInt(0));
    std::vector<std::string> changed;
    std::set<std::string> seen;
    for (const auto& item : values.items()) {
        ensure(allowed.count(item.key()) && item.value().is_string(), "forbidden_setting", "Only advertised keys and Orca serialized string values are writable");
        try { candidate.set_deserialize_strict(item.key(), item.value().get<std::string>()); }
        catch (const ConfigurationError& error) { throw DomainError("invalid_value", error.what()); }
        validate_option(*print_config_def.get(item.key()), *candidate.option(item.key()));
        seen.insert(item.key()); changed.push_back(item.key());
    }
    for (const auto& value : unset) {
        const auto key = value.get<std::string>();
        ensure(allowed.count(key) && seen.insert(key).second, "invalid_params", "An unset key is forbidden, duplicated or also present in values");
        ensure(target.scope != "height_range" || (key != "layer_height" && key != "extruder"), "invalid_scope", "A height range must keep its layer_height and extruder inheritance sentinel");
        candidate.erase(key); changed.push_back(key);
    }
    validate_chains(ctx, target, candidate, changed);
    const bool changed_config = target.config == nullptr || candidate != *target.config;
    if (changed_config) {
        if (target.scope == "process") {
            auto* tab = GUI::wxGetApp().get_tab(Preset::TYPE_PRINT);
            ensure(tab != nullptr, "not_ready", "The process settings tab is not ready");
            tab->apply_agent_config(candidate);
        } else {
            const bool created_range = target.scope == "height_range" && target.model_config == nullptr;
            const DynamicPrintConfig before = target.config ? *target.config : DynamicPrintConfig();
            ctx.plater.take_snapshot("Apply settings");
            if (target.model_config) target.model_config->assign_config(std::move(candidate));
            else if (target.scope == "height_range") target.object->layer_config_ranges[target.range].assign_config(std::move(candidate));
            else *target.plate->config() = std::move(candidate);
            try { refresh_model(ctx, target.object, target.volume, target.scope == "height_range"); }
            catch (...) {
                if (created_range) target.object->layer_config_ranges.erase(target.range);
                else if (target.model_config) target.model_config->assign_config(before);
                else *target.plate->config() = before;
                refresh_model(ctx, target.object, target.volume, target.scope == "height_range");
                throw;
            }
        }
    }
    // Return the changed settings even when they fall outside the first schema page.
    Json query = params; query.erase("values"); query.erase("unset"); query["keys"] = changed; query["limit"] = 100;
    if (changed.size() > 100) query.erase("keys");
    Json result = read_settings(ctx, query); result["changed_keys"] = changed; result["changed"] = changed_config;
    return result;
}

Preset::Type preset_type(const Json& params)
{
    const auto type = params.at("preset_type").get<std::string>();
    return type == "process" ? Preset::TYPE_PRINT : type == "filament" ? Preset::TYPE_FILAMENT : Preset::TYPE_PRINTER;
}
PresetCollection& collection(Preset::Type type)
{
    auto& bundle = *GUI::wxGetApp().preset_bundle;
    if (type == Preset::TYPE_PRINT) return bundle.prints;
    if (type == Preset::TYPE_FILAMENT) return bundle.filaments;
    return bundle.printers;
}
Json preset_info(const Preset& preset)
{
    return {{"name", preset.name}, {"system", preset.is_system}, {"compatible", preset.is_compatible},
            {"visible", preset.is_visible}, {"dirty", preset.is_dirty}, {"inherits", preset.inherits()}};
}
Json presets(DomainContext& ctx, const std::string& method, const Json& params)
{
    const auto type = preset_type(params);
    auto& presets = collection(type);
    auto* tab = GUI::wxGetApp().get_tab(type);
    ensure(tab != nullptr, "not_ready", "The native preset tab is unavailable");
    if (method == "presets.list") {
        std::vector<const Preset*> available;
        const auto filter = params.value("filter", "");
        for (const auto& preset : presets.get_presets())
            if (preset.name.find(filter) != std::string::npos) available.push_back(&preset);
        const auto offset = std::min(params.value("offset", std::size_t(0)), available.size());
        const auto end = std::min(offset + params.value("limit", std::size_t(80)), available.size());
        Json list = Json::array(); for (auto i = offset; i < end; ++i) list.push_back(preset_info(*available[i]));
        return {{"presets", list}, {"selected", presets.get_edited_preset().name}, {"total", available.size()},
                {"next_offset", end < available.size() ? Json(end) : Json()}};
    }
    if (method == "presets.select") {
        const auto name = params.at("name").get<std::string>();
        auto* preset = presets.find_preset(name, false);
        ensure(preset && preset->name == name && preset->is_visible && preset->is_compatible,
               "incompatible_preset", "Choose an existing visible preset compatible with the current machine/process");
        // Use the normal Transfer/Discard/Save dialogs rather than silently discarding edits.
        ensure(tab->select_preset(name), "cancelled", "Preset selection was cancelled by the user");
        ensure(presets.get_edited_preset().name == name, "cancelled", "The requested preset was not selected");
        return preset_info(presets.get_edited_preset());
    }
    if (method == "presets.save_as") {
        const auto name = params.at("name").get<std::string>();
        ensure(!name.empty() && name.size() <= 120 && name != "." && name != ".." &&
               !std::any_of(name.begin(), name.end(), [](unsigned char byte) { return byte < 32; }) &&
               name.find_first_of("/\\:*?\"<>|") == std::string::npos && name.front() != ' ' && name.back() != ' ' && name.back() != '.',
               "invalid_name", "Use a simple nonempty preset name without path separators or reserved characters");
        ensure(!presets.find_preset(name, false), "already_exists", "Save-as never overwrites an existing or system preset");
        tab->save_preset(name, false, false, false, "", true);
        const auto* saved = presets.find_preset(name, false);
        ensure(saved && saved->name == name && !saved->is_system, "save_failed", "The native preset was not saved under the requested name");
        return preset_info(*saved);
    }
    auto& edited = presets.get_edited_preset();
    const std::vector<std::string>& keys = type == Preset::TYPE_PRINT ? Preset::print_options() :
        type == Preset::TYPE_FILAMENT ? Preset::filament_options() : Preset::printer_options();
    const auto allowed = key_set(keys);
    if (method == "presets.patch") {
        ensure(type != Preset::TYPE_FILAMENT || GUI::wxGetApp().preset_bundle->filament_presets.size() == 1,
               "invalid_scope", "Edited filament patch currently requires one filament slot; multi-slot assignment must be explicit");
        const auto& values = params.at("values");
        ensure(!values.empty() && values.size() <= 256, "invalid_params", "Supply between 1 and 256 edited-preset values");
        DynamicPrintConfig candidate(edited.config);
        std::vector<std::string> changed;
        for (const auto& item : values.items()) {
            ensure(allowed.count(item.key()), "forbidden_setting", "Executable/network/identity preset fields are not exposed");
            try { candidate.set_deserialize_strict(item.key(), item.value().get<std::string>()); }
            catch (const ConfigurationError& error) { throw DomainError("invalid_value", error.what()); }
            validate_option(*print_config_def.get(item.key()), *candidate.option(item.key()));
            // Changing tool count requires the native printer selection workflow, not
            // a partial vector edit that could leave dependent arrays inconsistent.
            if (candidate.option(item.key())->is_vector() && edited.config.has(item.key())) {
                const auto& before = dynamic_cast<const ConfigOptionVectorBase&>(*edited.config.option(item.key()));
                const auto& after = dynamic_cast<const ConfigOptionVectorBase&>(*candidate.option(item.key()));
                ensure(before.size() == after.size(), "invalid_value", "Preset vector edits must preserve their existing cardinality");
            }
            changed.push_back(item.key());
        }
        Owner preset; preset.scope = type == Preset::TYPE_PRINT ? "process" : "preset";
        validate_chains(ctx, preset, candidate, changed);
        tab->apply_agent_config(candidate);
        return {{"preset", preset_info(presets.get_edited_preset())}, {"changed_keys", changed}, {"saved", false}};
    }
    const auto name = params.value("name", edited.name);
    const auto* preset = name == edited.name ? &edited : presets.find_preset(name, false);
    ensure(preset && preset->name == name, "unknown_preset", "The exact preset name does not exist");
    Json pagination, settings = Json::object();
    for (const auto& key : page(allowed, params, pagination, false)) {
        if (const auto* option = preset->config.option(key)) {
            const auto* def = print_config_def.get(key);
            settings[key] = {{"value", option->serialize()}, {"type", int(def->type)}, {"label", def->label},
                {"min", std::isfinite(def->min) ? Json(def->min) : Json()}, {"max", std::isfinite(def->max) ? Json(def->max) : Json()},
                {"enum_values", def->enum_values}, {"category", def->category}, {"units", def->sidetext}};
        }
    }
    return {{"preset", preset_info(*preset)}, {"settings", settings}, {"pagination", pagination}};
}

FacetsAnnotation& annotation(ModelVolume& volume, const std::string& kind)
{
    if (kind == "support") return volume.supported_facets;
    if (kind == "seam") return volume.seam_facets;
    return volume.mmu_segmentation_facets;
}
void refresh_painting(DomainContext& ctx, ModelObject& object)
{
    const auto& objects = ctx.plater.model().objects;
    const auto index = std::find(objects.begin(), objects.end(), &object) - objects.begin();
    if (GUI::wxGetApp().obj_list()) GUI::wxGetApp().obj_list()->update_info_items(index);
    // changed_object() drops objects to the bed; annotations must never move the
    // model. Use the same scene/background invalidation as native paint gizmos.
    if (auto* canvas = ctx.plater.get_current_canvas3D(true)) { canvas->reload_scene(false); canvas->set_as_dirty(); }
    ctx.plater.schedule_background_process();
}
Vec3d vector3(const Json& values)
{
    Vec3d result;
    for (int axis = 0; axis < 3; ++axis) result[axis] = values.at(axis).get<double>();
    ensure(result.allFinite() && result.cwiseAbs().maxCoeff() <= 1e9, "invalid_value", "Mesh-space coordinates must be finite and bounded");
    return result;
}

Json preview(DomainContext& ctx, const Json& params)
{
    Json owner_params = params; owner_params["scope"] = "process";
    const auto selected = owner(ctx, owner_params);
    const auto width = params.value("width", 512u), height = params.value("height", 512u);
    const auto scope = params.value("scope", "plate");
    const auto view = params.value("view", scope == "bed" ? "top" : "isometric");
    ensure(scope != "bed" || view == "top", "invalid_params", "Bed placement is a top view; omit view or use top");
    GUI::Camera::ViewAngleType angle = GUI::Camera::ViewAngleType::Iso;
    if (view == "top") angle = GUI::Camera::ViewAngleType::Top;
    else if (view == "front") angle = GUI::Camera::ViewAngleType::Front;
    else if (view == "left") angle = GUI::Camera::ViewAngleType::Left;
    else if (view == "right") angle = GUI::Camera::ViewAngleType::Right;
    auto* canvas = ctx.plater.get_current_canvas3D(true);
    ensure(canvas != nullptr, "not_ready", "The native 3D canvas is unavailable");
    ThumbnailData data;
    Json slice_id;
    if (scope == "slice") {
        ensure(selected.plate == ctx.plater.get_partplate_list().get_curr_plate(), "invalid_scope", "Slice preview requires the selected current plate");
        ensure(!ctx.plater.background_process().running(), "busy", "Wait until slicing and its result handoff have completed");
        ensure(selected.plate->is_slice_result_valid() && !selected.plate->is_apply_result_invalid(), "slice_unavailable", "The current plate has no valid completed slicing result");
        const auto before = ctx.current_state();
        const auto* result = selected.plate->get_slice_result();
        if (result) slice_id = result->id;
        ensure(result && !result->moves.empty(), "slice_unavailable", "The completed slice has no G-code toolpaths");
        ensure(result->moves.size() <= 2000000, "preview_limit", "Slice preview supports at most two million G-code moves");
        ensure(canvas->render_slice_thumbnail(data, *result, ctx.plater.get_extruder_colors_from_plater_config(result), width, height, angle),
               "preview_unavailable", "Native G-code rendering requires an initialized OpenGL 3.2 canvas and framebuffer support; show the editor and retry");
        ensure(before == ctx.current_state() && result == selected.plate->get_slice_result() && selected.plate->is_slice_result_valid() &&
               !ctx.plater.background_process().running(), "stale_slice", "The slicing result changed during preview rendering");
    } else {
        ThumbnailsParams thumbnail {{{double(width), double(height)}}, false, true, true, true, selected.plate->get_index(), scope != "scene"};
        canvas->render_thumbnail(data, width, height, thumbnail, GUI::Camera::EType::Ortho,
            scope == "bed" ? GUI::Camera::ViewAngleType::Top_Plate : angle);
    }
    ensure(data.is_valid(), "preview_unavailable", "The native OpenGL canvas could not render a thumbnail; show the editor and retry");
    if (scope == "bed") {
        // Native thumbnails intentionally omit the bed. Top_Plate provides the
        // exact plate-sized camera; composite its transparent pixels over the
        // real plate polygon and a millimetre grid to preserve placement scale.
        auto box = selected.plate->get_build_volume();
        box.min.x() -= BuildVolume::SceneEpsilon; box.min.y() -= BuildVolume::SceneEpsilon;
        box.max.x() += BuildVolume::SceneEpsilon; box.max.y() += BuildVolume::SceneEpsilon;
        ensure(box.size().x() > 0 && box.size().y() > 0 && selected.plate->get_shape().size() <= 1024,
            "preview_limit", "Plate dimensions or polygon exceed the placement preview limits");
        const auto polygon = Polygon::new_scale(selected.plate->get_shape());
        const auto center = box.center();
        const double scale = std::min(double(width) / box.size().x(), double(height) / box.size().y());
        for (unsigned int row = 0; row < height; ++row) for (unsigned int col = 0; col < width; ++col) {
            const double x = center.x() + (double(col) + 0.5 - double(width) / 2) / scale;
            const double y = center.y() + (double(row) + 0.5 - double(height) / 2) / scale;
            const bool inside = polygon.contains(Point(scale_(x), scale_(y)));
            const bool grid = std::abs(x - std::round(x / 10) * 10) < 0.6 / scale ||
                              std::abs(y - std::round(y / 10) * 10) < 0.6 / scale;
            const unsigned char background = inside ? (grid ? 100 : 62) : 24;
            const std::size_t offset = 4 * (std::size_t(row) * width + col);
            const unsigned alpha = data.pixels[offset + 3];
            for (int channel = 0; channel < 3; ++channel)
                data.pixels[offset + channel] = (unsigned(data.pixels[offset + channel]) * alpha + unsigned(background) * (255 - alpha)) / 255;
            data.pixels[offset + 3] = 255;
        }
    }
    const auto compressed = GCodeThumbnails::compress_thumbnail(data, GCodeThumbnailsFormat::PNG);
    ensure(compressed && compressed->data && compressed->size > 0, "preview_failed", "Native PNG encoding failed");
    ensure(compressed->size <= 2 * 1024 * 1024, "preview_limit", "PNG exceeds 2 MiB; request a smaller preview");
    std::string encoded(boost::beast::detail::base64::encoded_size(compressed->size), '\0');
    encoded.resize(boost::beast::detail::base64::encode(encoded.data(), compressed->data, compressed->size));
    return {{"mime_type", "image/png"}, {"data", std::move(encoded)}, {"width", data.width}, {"height", data.height},
        {"state_token", ctx.state_token}, {"scope", scope}, {"view", view}, {"plate_id", plate_identity(ctx, *selected.plate)},
        {"slice_id", slice_id}, {"content_kind", scope == "slice" ? "sliced_toolpaths" : "model"},
        {"grid_mm", scope == "bed" ? Json(10) : Json()},
        {"content", scope == "bed" ? "Native top-view model on the actual plate polygon with a 10 mm grid; printable model parts only" : scope == "slice" ? "Native completed G-code toolpaths, all layers, filament colors, machine coordinates; no model fallback" :
            scope == "plate" ? "Native printable-parts thumbnail; outside, nonprintable and modifier volumes may be omitted" : "Native whole-scene thumbnail"}};
}

Json history(DomainContext& ctx, const std::string& method)
{
    const bool undo = method == "history.undo";
    if (method != "history.describe") {
        ensure(undo ? ctx.plater.can_undo() : ctx.plater.can_redo(), "history_unavailable",
               "Native document history is unavailable; show the Prepare editor, finish operations and inspect history.describe");
        const auto before = ctx.plater.undo_redo_stack_main().active_snapshot_time();
        if (undo) ctx.plater.undo(); else ctx.plater.redo();
        ensure(before != ctx.plater.undo_redo_stack_main().active_snapshot_time(), "history_unavailable",
               "Native history did not restore a document snapshot");
        // Undo may restore IDs that had previously been removed. Invalidate old
        // document/plate handles, then require a fresh scene read rather than
        // manufacturing handles from the context's earlier document epoch.
        ctx.invalidate(true);
    }
    return {{"can_undo", ctx.plater.can_undo()}, {"can_redo", ctx.plater.can_redo()},
            {"performed", method == "history.describe" ? Json() : Json(undo ? "undo" : "redo")},
            {"refresh_scene", method != "history.describe"}};
}

Json painting(DomainContext& ctx, const std::string& method, const Json& params)
{
    Json owner_params = params; owner_params["scope"] = "volume";
    const Owner target = owner(ctx, owner_params);
    ensure(target.volume->is_model_part(), "invalid_scope", "Painting targets a printable model part, not a modifier");
    const auto kind = params.at("kind").get<std::string>();
    auto& annotations = annotation(*target.volume, kind);
    const auto& mesh = target.volume->mesh();
    const auto revision = mesh_revision(*target.volume);
    const auto count = mesh.its.indices.size();
    Json result {{"object_id", std::to_string(target.object->id().id)}, {"volume_id", std::to_string(target.volume->id().id)},
        {"mesh_revision", revision}, {"kind", kind}, {"facet_count", count}, {"coordinate_frame", "volume_mesh"},
        {"region_rule", "original triangle centroid inside inclusive mesh-space AABB"},
        {"limits", {{"volume_triangles", 500000}, {"set_edited_facets", 10000}}}};
    if (method == "painting.describe") {
        const auto begin = std::min(params.value("offset", std::size_t(0)), count);
        const auto end = std::min(begin + params.value("limit", std::size_t(80)), count);
        Json facets = Json::array();
        for (auto index = begin; index < end; ++index)
            facets.push_back({{"facet_id", index}, {"annotation", annotations.get_triangle_as_string(int(index))}});
        result["facets"] = facets; result["next_offset"] = end < count ? Json(end) : Json();
        result["states"] = kind == "material" ? Json("0 = unpainted; 1..filament_count = 1-based filament slot") : Json("0 = automatic, 1 = enforce, 2 = block");
        result["encoding"] = "Native triangle annotation including subdivisions; edits replace the entire original facet";
        return result;
    }
    ensure(params.at("mesh_revision").get<std::string>() == revision, "stale_mesh", "The source mesh or volume transform changed; inspect painting.describe again");
    ensure(count <= 500000, "geometry_limit", "Deterministic painting currently supports at most 500000 source triangles per volume");
    if (method == "painting.clear") {
        if (!annotations.empty()) {
            ctx.plater.take_snapshot("Clear painted facets"); annotations.reset(); refresh_painting(ctx, *target.object);
        }
        result["cleared"] = true; return result;
    }
    Vec3d ray_origin, ray_direction;
    igl::Hit<float> hit;
    if (method == "painting.brush" || method == "painting.raycast") {
        ray_origin = vector3(params.at("ray_origin")); ray_direction = vector3(params.at("ray_direction"));
        ensure(ray_direction.norm() > 1e-9, "invalid_ray", "Ray direction must be nonzero");
        ray_direction.normalize();
        const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.its.vertices, mesh.its.indices);
        const bool intersects = AABBTreeIndirect::intersect_ray_first_hit(mesh.its.vertices, mesh.its.indices, tree, ray_origin, ray_direction, hit);
        result["hit"] = intersects;
        if (!intersects) { ensure(method == "painting.raycast", "ray_miss", "The ray did not hit this mesh"); return result; }
        const Vec3d position = ray_origin + double(hit.t) * ray_direction;
        result["facet_id"] = hit.id; result["position"] = {position.x(), position.y(), position.z()};
        if (method == "painting.raycast") return result;
    }
    const int state = params.at("state").get<int>();
    const auto max_state = kind == "material" ? std::min<std::size_t>(GUI::wxGetApp().preset_bundle->filament_presets.size(), int(EnforcerBlockerType::ExtruderMax)) : 2;
    ensure(state >= 0 && std::size_t(state) <= max_state, "invalid_value", "Paint state must identify an existing filament slot, or automatic/enforce/block");
    if (method == "painting.brush") {
        const Vec3f position = (ray_origin + double(hit.t) * ray_direction).cast<float>();
        const float radius = params.at("radius").get<float>();
        const auto shape = params.value("shape", "sphere") == "sphere" ? TriangleSelector::CursorType::SPHERE : TriangleSelector::CursorType::CIRCLE;
        TriangleSelector selector(mesh);
        selector.deserialize(annotations.get_data());
        auto cursor = TriangleSelector::SinglePointCursor::cursor_factory(position, ray_origin.cast<float>(), radius, shape,
                                                                         Transform3d::Identity(), TriangleSelector::ClippingPlane());
        // Work in the declared volume-mesh coordinate frame, without screen input
        // or UI selection. Whole-facet strokes keep subdivisions/memory bounded.
        selector.select_patch(hit.id, std::move(cursor), static_cast<EnforcerBlockerType>(state), Transform3d::Identity(), false);
        auto data = selector.serialize();
        ctx.plater.take_snapshot("Paint mesh brush stroke"); annotations.set_data(std::move(data)); refresh_painting(ctx, *target.object);
        result["state"] = state; result["shape"] = params.value("shape", "sphere"); result["radius"] = radius;
        result["triangle_splitting"] = false;
        return result;
    }
    ensure(params.contains("facets") != params.contains("region"), "invalid_params", "Supply exactly one of facets or region");
    std::set<int> facets;
    if (params.contains("facets")) {
        for (const auto& index : params.at("facets")) {
            const int facet = index.get<int>();
            ensure(facet >= 0 && std::size_t(facet) < count && facets.insert(facet).second,
                   "invalid_facet", "Facet IDs must be unique original mesh triangle indices");
        }
    } else {
        const auto& region = params.at("region");
        Vec3d lower, upper;
        for (int axis = 0; axis < 3; ++axis) {
            lower[axis] = region.at("min").at(axis).get<double>(); upper[axis] = region.at("max").at(axis).get<double>();
            ensure(std::isfinite(lower[axis]) && std::isfinite(upper[axis]) && upper[axis] >= lower[axis], "invalid_region", "Region bounds must be finite and ordered");
        }
        for (std::size_t i = 0; i < count; ++i) {
            const auto& face = mesh.its.indices[i];
            const Vec3d centroid = (mesh.its.vertices[face[0]].cast<double>() + mesh.its.vertices[face[1]].cast<double>() + mesh.its.vertices[face[2]].cast<double>()) / 3.;
            if ((centroid.array() >= lower.array()).all() && (centroid.array() <= upper.array()).all()) facets.insert(int(i));
        }
    }
    ensure(!facets.empty() && facets.size() <= 10000, "geometry_limit", "Paint edits require between 1 and 10000 source facets; narrow the region or batch explicit facets");
    TriangleSelector selector(mesh);
    selector.deserialize(annotations.get_data());
    for (int facet : facets) selector.set_facet(facet, static_cast<EnforcerBlockerType>(state));
    // Compute/allocate serialization before creating the native Undo snapshot.
    auto data = selector.serialize();
    ctx.plater.take_snapshot("Paint facets"); annotations.set_data(std::move(data)); refresh_painting(ctx, *target.object);
    result["edited_facets"] = facets.size(); result["state"] = state;
    return result;
}
} // namespace

CommandSpecs settings_commands()
{
    CommandSpecs specs;
    Json query = owner_properties(); query.update(page_properties());
    specs["settings.describe"] = command_spec(false, query);
    specs["settings.get"] = command_spec(false, query);
    Json patch = owner_properties();
    patch["values"] = {{"type", "object"}, {"additionalProperties", {{"type", "string"}, {"maxLength", 32768}}}};
    patch["unset"] = {{"type", "array"}, {"maxItems", 256}, {"uniqueItems", true}, {"items", text()}};
    specs["settings.patch"] = command_spec(true, patch, {"scope"});
    Json preset = page_properties();
    preset["preset_type"] = {{"type", "string"}, {"enum", {"process", "filament", "machine"}}}; preset["name"] = text();
    specs["presets.list"] = command_spec(false, preset, {"preset_type"});
    specs["presets.describe"] = command_spec(false, preset, {"preset_type"});
    specs["presets.select"] = command_spec(true, {{"preset_type", preset["preset_type"]}, {"name", text()}}, {"preset_type", "name"});
    specs["presets.save_as"] = command_spec(true, {{"preset_type", preset["preset_type"]}, {"name", text()}}, {"preset_type", "name"});
    specs["presets.patch"] = command_spec(true, {{"preset_type", preset["preset_type"]}, {"values", patch["values"]}}, {"preset_type", "values"});
    Json paint {{"object_id", text()}, {"volume_id", text()}, {"plate_index", bounded_integer(1000000)}, {"plate_id", text()},
        {"kind", {{"type", "string"}, {"enum", {"support", "seam", "material"}}}}};
    Json inspect = paint; inspect["offset"] = bounded_integer(10000000);
    inspect["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 100}};
    specs["painting.describe"] = command_spec(false, inspect, {"object_id", "volume_id", "kind"});
    paint["mesh_revision"] = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    specs["painting.clear"] = command_spec(true, paint, {"object_id", "volume_id", "kind", "mesh_revision"});
    paint["state"] = bounded_integer(int(EnforcerBlockerType::ExtruderMax));
    paint["facets"] = {{"type", "array"}, {"minItems", 1}, {"maxItems", 10000}, {"uniqueItems", true}, {"items", bounded_integer(10000000)}};
    Json triple {{"type", "array"}, {"minItems", 3}, {"maxItems", 3}, {"items", {{"type", "number"}}}};
    paint["region"] = {{"type", "object"}, {"additionalProperties", false}, {"properties", {{"min", triple}, {"max", triple}}}, {"required", {"min", "max"}}};
    specs["painting.set"] = command_spec(true, paint, {"object_id", "volume_id", "kind", "mesh_revision", "state"});
    Json ray = paint; ray.erase("facets"); ray.erase("region"); ray.erase("state");
    ray["ray_origin"] = triple; ray["ray_direction"] = triple;
    specs["painting.raycast"] = command_spec(false, ray, {"object_id", "volume_id", "kind", "mesh_revision", "ray_origin", "ray_direction"});
    ray["state"] = paint["state"];
    ray["radius"] = {{"type", "number"}, {"minimum", 0.1}, {"maximum", 1000}};
    ray["shape"] = {{"type", "string"}, {"enum", {"sphere", "circle"}}};
    specs["painting.brush"] = command_spec(true, ray, {"object_id", "volume_id", "kind", "mesh_revision", "ray_origin", "ray_direction", "state", "radius"});
    Json render {{"scope", {{"type", "string"}, {"enum", {"plate", "scene", "bed", "slice"}}}}, {"plate_id", text()}, {"plate_index", bounded_integer(1000000)},
        {"width", {{"type", "integer"}, {"minimum", 64}, {"maximum", 1024}}}, {"height", {{"type", "integer"}, {"minimum", 64}, {"maximum", 1024}}},
        {"view", {{"type", "string"}, {"enum", {"isometric", "top", "front", "left", "right"}}}}};
    specs["preview.render"] = command_spec(false, render);
    specs["history.describe"] = command_spec(false, Json::object());
    specs["history.undo"] = command_spec(true, Json::object());
    specs["history.redo"] = command_spec(true, Json::object());
    return specs;
}

Json execute_settings(DomainContext& ctx, const std::string& method, const Json& params)
{
    ensure(GUI::wxGetApp().preset_bundle != nullptr, "not_ready", "Preset bundle is not available");
    if (method == "settings.describe") return describe_settings(params);
    if (method == "settings.get") return read_settings(ctx, params);
    if (method == "settings.patch") return patch_settings(ctx, params);
    if (method == "preview.render") return preview(ctx, params);
    if (method.rfind("history.", 0) == 0) return history(ctx, method);
    if (method.rfind("presets.", 0) == 0) return presets(ctx, method, params);
    if (method.rfind("painting.", 0) == 0) return painting(ctx, method, params);
    throw DomainError("unknown_method", "Unknown settings/preset/painting command");
}
Json slice_estimates(const GCodeProcessorResult& result)
{
    Json used = Json::array();
    double grams = 0, meters = 0, volume = 0;
    bool mass_known = true, length_known = true;
    for (const auto& item : result.print_statistics.total_volumes_per_extruder) {
        const auto index = item.first;
        const double v = item.second;
        ensure(std::isfinite(v) && v >= 0, "invalid_result", "Sliced filament consumption is invalid");
        const bool density_known = index < result.filament_densities.size() && std::isfinite(result.filament_densities[index]) && result.filament_densities[index] > 0;
        const bool diameter_known = index < result.filament_diameters.size() && std::isfinite(result.filament_diameters[index]) && result.filament_diameters[index] > 0;
        const double mass = density_known ? v * result.filament_densities[index] / 1000.0 : 0;
        const double diameter = diameter_known ? result.filament_diameters[index] : 0;
        const double length = diameter_known ? v / (3.14159265358979323846 * diameter * diameter / 4) / 1000.0 : 0;
        grams += mass; meters += length; volume += v;
        mass_known &= density_known; length_known &= diameter_known;
        used.push_back({{"filament_id", index + 1}, {"volume_mm3", v},
            {"grams", density_known ? Json(mass) : Json()}, {"meters", diameter_known ? Json(length) : Json()}});
    }
    return {{"approximate", true}, {"time_seconds", result.print_statistics.modes[0].time},
        {"filament_volume_mm3", volume}, {"filament_grams", mass_known ? Json(grams) : Json()},
        {"filament_meters", length_known ? Json(meters) : Json()}, {"used_filaments", std::move(used)}};
}

Json print_report(DomainContext& ctx, GUI::PartPlate& plate)
{
    auto* bundle = GUI::wxGetApp().preset_bundle;
    const auto* result = plate.get_slice_result();
    ensure(bundle && result && plate.is_slice_result_valid(), "no_result", "A completed current slice is required for a print report");
    const auto estimates = slice_estimates(*result);
    const auto& used = estimates.at("used_filaments");
    Json changes = Json::array();
    std::size_t total = 0;
    const auto differences = [&](const std::string& scope, Json identity, const DynamicPrintConfig& baseline,
                                 const DynamicPrintConfig& applied, const std::vector<std::string>& keys,
                                 const std::string& reference) {
        for (const auto& key : keys) {
            if (!ordinary_key(key)) continue;
            const auto* value = applied.option(key);
            const auto* before = baseline.option(key);
            if (!value || (before && *before == *value)) continue;
            ++total;
            if (changes.size() >= 512) continue;
            const auto serialized = value->serialize();
            const auto old_value = before ? before->serialize() : std::string();
            changes.push_back({{"scope", scope}, {"owner", identity}, {"key", key},
                {"baseline", before ? Json(old_value.substr(0, 4096)) : Json()}, {"applied", serialized.substr(0, 4096)},
                {"baseline_source", reference}, {"value_truncated", serialized.size() > 4096 || old_value.size() > 4096}});
        }
    };
    differences("process", {{"preset", bundle->prints.get_selected_preset().name}}, bundle->prints.get_selected_preset().config,
        bundle->prints.get_edited_preset().config, Preset::print_options(), "saved_selected_preset");
    differences("printer", {{"preset", bundle->printers.get_selected_preset().name}}, bundle->printers.get_selected_preset().config,
        bundle->printers.get_edited_preset().config, Preset::printer_options(), "saved_selected_preset");
    for (const auto& material : used) {
        const auto index = material.at("filament_id").get<std::size_t>() - 1;
        if (index >= bundle->filament_presets.size()) continue;
        const auto& name = bundle->filament_presets[index];
        const auto* saved = bundle->filaments.find_preset(name, false);
        if (!saved) continue;
        const auto& edited = bundle->filaments.get_edited_preset();
        if (edited.name == name)
            differences("filament", {{"filament_id", index + 1}, {"preset", name}}, saved->config, edited.config,
                Preset::filament_options(), "saved_filament_preset");
    }
    DynamicPrintConfig global = bundle->full_config();
    differences("plate", {{"plate_id", plate_identity(ctx, plate)}}, global, *plate.config(), plate.config()->keys(), "inherited_project_configuration");
    global.apply(*plate.config());
    for (const auto* object : plate.get_objects_on_this_plate()) {
        differences("object", {{"object_id", std::to_string(object->id().id)}}, global, object->config.get(), object->config.keys(), "inherited_plate_configuration");
        auto inherited = global;
        inherited.apply(object->config.get());
        for (const auto& range : object->layer_config_ranges)
            differences("height_range", {{"object_id", std::to_string(object->id().id)}, {"range", {range.first.first, range.first.second}}},
                inherited, range.second.get(), range.second.keys(), "inherited_object_configuration");
        for (const auto* part : object->volumes) {
            const Json identity = {{"object_id", std::to_string(object->id().id)}, {"volume_id", std::to_string(part->id().id)}};
            differences("volume", identity, inherited, part->config.get(), part->config.keys(), "inherited_object_configuration");
            if (const auto* material = material_config(*part)) {
                auto part_config = inherited;
                part_config.apply(part->config.get());
                differences("volume_material", identity, part_config, *material, material->keys(), "inherited_volume_configuration");
            }
        }
    }
    // Project-level controls (for example nozzle flow) have a different baseline
    // from a saved preset. Label that baseline explicitly rather than inventing
    // a universal default printer/process.
    DynamicPrintConfig defaults;
    for (const auto& key : bundle->project_config.keys())
        if (ordinary_key(key)) if (const auto* def = print_config_def.get(key); def && def->default_value)
            defaults.set_key_value(key, def->default_value->clone());
    differences("project", Json::object(), defaults, bundle->project_config, bundle->project_config.keys(), "native_option_default");
    return {{"plate_id", plate_identity(ctx, plate)}, {"slice_id", result->id}, {"state_token", ctx.state_token},
        {"estimates", estimates},
        {"settings_changes", std::move(changes)}, {"settings_change_count", total}, {"settings_changes_truncated", total > 512},
        {"settings_baseline", "Saved selected presets plus explicitly labeled inherited overrides; not a universal factory default"}};
}
} // namespace Slic3r::AgentControl
