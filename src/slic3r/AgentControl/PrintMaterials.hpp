#pragma once

#include "Domain.hpp"
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::AgentControl {

// GCodeProcessor's historical *_per_extruder names contain zero-based project
// filament IDs, including support, painted regions and wipe-tower consumption.
inline std::set<std::size_t> sliced_filament_indices(const std::map<std::size_t, double>& volumes, std::size_t configured_count)
{
    std::set<std::size_t> used;
    for (const auto& entry : volumes) {
        ensure(entry.first < configured_count && std::isfinite(entry.second) && entry.second >= 0,
            "compatibility_unverified", "Sliced filament consumption is invalid or outside the configured filament list");
        used.insert(entry.first);
    }
    ensure(!used.empty(), "compatibility_unverified", "Completed slice has no verified filament consumption");
    return used;
}

inline std::map<std::size_t, int> explicit_ams_mapping(const Json& options, const std::set<std::size_t>& used)
{
    const bool use_ams = options.value("use_ams", false);
    ensure(use_ams || (!options.contains("ams_tray_index") && !options.contains("ams_mapping")),
        "invalid_params", "AMS tray selection requires use_ams=true");
    if (!use_ams) {
        ensure(used.size() == 1, "compatibility_unverified", "Multiple used filaments require explicit AMS mapping; manual spool changes are not verified");
        return {};
    }
    ensure(!(options.contains("ams_tray_index") && options.contains("ams_mapping")),
        "invalid_params", "Choose ams_mapping or ams_tray_index, not both");
    std::map<std::size_t, int> mapping;
    if (options.contains("ams_tray_index")) {
        ensure(used.size() == 1, "invalid_params", "ams_tray_index requires exactly one used filament");
        mapping.emplace(*used.begin(), options.at("ams_tray_index").get<int>());
    } else {
        ensure(options.contains("ams_mapping"), "invalid_params", "AMS submission requires explicit mapping for every used filament");
        for (const auto& item : options.at("ams_mapping")) {
            const int id = item.at("filament_id").get<int>();
            ensure(id > 0 && used.count(static_cast<std::size_t>(id - 1)) != 0,
                "invalid_params", "AMS mapping references a filament not used by this slice");
            ensure(mapping.emplace(static_cast<std::size_t>(id - 1), item.at("tray_index").get<int>()).second,
                "invalid_params", "AMS mapping repeats a filament ID");
        }
    }
    ensure(mapping.size() == used.size(), "invalid_params", "AMS mapping must cover every used filament, including supports");
    for (const auto& item : mapping)
        ensure(item.second >= 0 && item.second < 254, "invalid_params", "AMS tray index must identify a physical AMS slot");
    return mapping;
}

struct PrintMaterialMapping {
    std::size_t index;
    int tray_index, ams_id, slot_id;
    int task_nozzle_id;
    std::string type, filament_id, source_color, target_color;
};

inline Json native_ams_mapping(std::size_t configured_count, const std::vector<PrintMaterialMapping>& materials)
{
    Json old_map = Json::array(), new_map = Json::array(), info = Json::array();
    for (std::size_t i = 0; i < configured_count; ++i) {
        old_map.push_back(-1);
        new_map.push_back({{"ams_id", 255}, {"slot_id", 255}});
        info.push_back({{"ams", -1}, {"targetColor", ""}, {"filamentId", ""}, {"filamentType", ""}});
    }
    for (const auto& material : materials) {
        ensure(material.index < configured_count, "compatibility_unverified", "Mapped filament is outside the configured filament list");
        // Virtual spool IDs remain in metadata, but v0 uses -1 for external.
        old_map[material.index] = material.tray_index >= 254 ? -1 : material.tray_index;
        new_map[material.index] = {{"ams_id", material.ams_id}, {"slot_id", material.slot_id}};
        // Mirrors SelectMachineDialog: nozzleId is present only for a resolved
        // plate filament map, and uses task IDs (left=1, right=0).
        info[material.index] = {{"ams", material.tray_index}, {"targetColor", material.target_color},
            {"sourceColor", material.source_color}, {"filamentId", material.filament_id},
            {"filamentType", material.type}};
        if (material.task_nozzle_id >= 0)
            info[material.index]["nozzleId"] = material.task_nozzle_id;
    }
    return {{"ams_mapping", old_map}, {"ams_mapping2", new_map}, {"ams_mapping_info", info}};
}

} // namespace Slic3r::AgentControl
