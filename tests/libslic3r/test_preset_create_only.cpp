#include <catch2/catch_test_macros.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "test_utils.hpp"

#include <cstddef>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <stdexcept>

namespace {
struct PresetDataDirectory {
    ScopedTemporaryDir temporary;
    std::string previous {Slic3r::data_dir()};
    PresetDataDirectory() { Slic3r::set_data_dir(temporary.string()); }
    ~PresetDataDirectory() { Slic3r::set_data_dir(previous); }
};
std::string read_bytes(const boost::filesystem::path& path)
{
    std::ifstream stream(path.string(), std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}

TEST_CASE("Create-only preset save preserves an existing complete file", "[PresetCreateOnly][Regression]")
{
    PresetDataDirectory data;
    const auto path = data.temporary.path() / "preset.json";
    Slic3r::Preset preset(Slic3r::Preset::TYPE_PRINT, "Prepared preset");
    preset.file = path.string();
    preset.config.set_key_value("wall_loops", new Slic3r::ConfigOptionInt(3));
    REQUIRE(preset.save(nullptr, true));
    const auto original = read_bytes(path);
    REQUIRE_FALSE(original.empty());
    preset.config.set_key_value("wall_loops", new Slic3r::ConfigOptionInt(7));
    REQUIRE_FALSE(preset.save(nullptr, true));
    CHECK(read_bytes(path) == original);
    std::size_t count = 0;
    for (const auto& entry : boost::filesystem::directory_iterator(data.temporary.path())) {
        if (entry.path().filename() == "preset.json") ++count;
        CHECK(entry.path().extension() != ".tmp");
    }
    CHECK(count == 1);
}

#ifndef _WIN32
TEST_CASE("Create-only preset save refuses destination symlinks", "[PresetCreateOnly][Regression]")
{
    PresetDataDirectory data;
    const auto real = data.temporary.path() / "existing.json";
    const auto link = data.temporary.path() / "preset.json";
    { std::ofstream stream(real.string()); stream << "existing-user-data"; }
    boost::filesystem::create_symlink(real, link);
    Slic3r::Preset preset(Slic3r::Preset::TYPE_PRINT, "Prepared preset");
    preset.file = link.string();
    preset.config.set_key_value("wall_loops", new Slic3r::ConfigOptionInt(3));
    REQUIRE_FALSE(preset.save(nullptr, true));
    CHECK(boost::filesystem::is_symlink(link));
    CHECK(read_bytes(real) == "existing-user-data");
}
#endif

TEST_CASE("Failed create-only save leaves the preset registry and selection unchanged", "[PresetCreateOnly][Regression]")
{
    PresetDataDirectory data;
    Slic3r::PresetCollection presets(Slic3r::Preset::TYPE_PRINT, Slic3r::Preset::print_options(),
                                    static_cast<const Slic3r::PrintRegionConfig&>(Slic3r::FullPrintConfig::defaults()));
    presets.update_user_presets_directory(data.temporary.string(), "print");
    // A brand-new collection has no selected parent profile; its first user
    // preset is a native base preset. Ask the owner for that destination.
    Slic3r::Preset conflicting(Slic3r::Preset::TYPE_PRINT, "Existing");
    const auto path = boost::filesystem::path(presets.path_for_preset(conflicting));
    boost::filesystem::create_directories(path.parent_path());
    { std::ofstream stream(path.string()); stream << "existing-user-data"; }
    const auto selected = presets.get_selected_preset_name();
    const auto count = presets.get_presets().size();
    REQUIRE_THROWS_AS(presets.save_current_preset("Existing", false, false, nullptr, true), std::runtime_error);
    CHECK(presets.get_presets().size() == count);
    CHECK(presets.get_selected_preset_name() == selected);
    CHECK(presets.find_preset("Existing", false) == nullptr);
    CHECK(read_bytes(path) == "existing-user-data");
}

TEST_CASE("Create-only save registers a newly persisted preset once", "[PresetCreateOnly][Regression]")
{
    PresetDataDirectory data;
    Slic3r::PresetCollection presets(Slic3r::Preset::TYPE_PRINT, Slic3r::Preset::print_options(),
                                    static_cast<const Slic3r::PrintRegionConfig&>(Slic3r::FullPrintConfig::defaults()));
    presets.update_user_presets_directory(data.temporary.string(), "print");
    const auto count = presets.get_presets().size();
    REQUIRE_NOTHROW(presets.save_current_preset("New preset", false, false, nullptr, true));
    REQUIRE(presets.find_preset("New preset", false) != nullptr);
    CHECK(presets.get_presets().size() == count + 1);
    CHECK(presets.get_selected_preset_name() == "New preset");
    CHECK(boost::filesystem::is_regular_file(presets.find_preset("New preset", false)->file));
    REQUIRE_THROWS_AS(presets.save_current_preset("New preset", false, false, nullptr, true), std::runtime_error);
    CHECK(presets.get_presets().size() == count + 1);
}
