#include <set>
#include <cstddef>
#include "slic3r/AgentControl/Domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include "slic3r/AgentControl/PrintMaterials.hpp"
#include <limits>

using namespace Slic3r::AgentControl;

TEST_CASE("Slice consumption preserves sparse project filament IDs", "[PrintMaterials]")
{
    // The processor aggregates model/painting, supports and tower into this map.
    const auto used=sliced_filament_indices({{2,12.0},{5,3.0},{7,0.0}},8);
    // Native used-filament APIs preserve all keys, including accumulated zeros.
    REQUIRE(used==std::set<std::size_t>{2,5,7});
    REQUIRE_THROWS_AS(sliced_filament_indices({{0,-1.0}},8),DomainError);
    REQUIRE_THROWS_AS(sliced_filament_indices({{8,1.0}},8),DomainError);
    REQUIRE_THROWS_AS(sliced_filament_indices({},8),DomainError);
    REQUIRE_THROWS_AS(sliced_filament_indices({{0,std::numeric_limits<double>::quiet_NaN()}},8),DomainError);
}

TEST_CASE("A single used filament maps independently of unused project filaments", "[PrintMaterials]")
{
    const auto used=sliced_filament_indices({{4,10.0}},6);
    const auto mapping=explicit_ams_mapping({{"use_ams",true},{"ams_tray_index",3}},used);
    REQUIRE(mapping.at(4)==3);
    REQUIRE(explicit_ams_mapping({{"use_ams",false}},used).empty());
}

TEST_CASE("AMS mappings cover exactly the sliced materials", "[PrintMaterials]")
{
    const std::set<std::size_t> used{1,3};
    Json options={{"use_ams",true},{"ams_mapping",Json::array({{{"filament_id",2},{"tray_index",0}},{{"filament_id",4},{"tray_index",2}}})}};
    const auto mapping=explicit_ams_mapping(options,used);
    REQUIRE(mapping.at(1)==0);
    REQUIRE(mapping.at(3)==2);
    options["ams_mapping"].erase(1);
    REQUIRE_THROWS_AS(explicit_ams_mapping(options,used),DomainError);
    options["ams_mapping"].push_back({{"filament_id",2},{"tray_index",3}});
    REQUIRE_THROWS_AS(explicit_ams_mapping(options,used),DomainError);
    options["ams_mapping"][1]["filament_id"]=1;
    REQUIRE_THROWS_AS(explicit_ams_mapping(options,used),DomainError);
    REQUIRE_THROWS_AS(explicit_ams_mapping({{"use_ams",false}},used),DomainError);
    REQUIRE_THROWS_AS(explicit_ams_mapping({{"use_ams",false},{"ams_tray_index",0}},{0}),DomainError);
    REQUIRE_THROWS_AS(explicit_ams_mapping({{"use_ams",true},{"ams_tray_index",254}},{0}),DomainError);
}

TEST_CASE("Native AMS payloads retain unused positions and both mapping generations", "[PrintMaterials]")
{
    const auto payload=native_ams_mapping(5,{{1,7,1,3,-1,"PLA","OFPLA","#FF0000FF","FF0000FF"},
        {4,25,6,1,0,"PETG","OFPETG","#00FF00FF","00FF00FF"}});
    REQUIRE(payload.at("ams_mapping")==Json::array({-1,7,-1,-1,25}));
    REQUIRE(payload.at("ams_mapping2")[0]==Json({{"ams_id",255},{"slot_id",255}}));
    REQUIRE(payload.at("ams_mapping2")[4]==Json({{"ams_id",6},{"slot_id",1}}));
    REQUIRE(payload.at("ams_mapping_info")[1].at("filamentId")=="OFPLA");
    REQUIRE_FALSE(payload.at("ams_mapping_info")[1].contains("nozzleId"));
    REQUIRE(payload.at("ams_mapping_info")[4].at("nozzleId")==0);
    REQUIRE(payload.at("ams_mapping_info")[4].at("filamentType")=="PETG");
    REQUIRE(payload.at("ams_mapping_info")[0].at("ams")==-1);
}

TEST_CASE("External spool metadata maps a nonzero used project filament", "[PrintMaterials]")
{
    const auto payload=native_ams_mapping(4,{{3,255,255,0,-1,"PLA","OFPLA","#FFFFFFFF","FFFFFFFF"}});
    REQUIRE(payload.at("ams_mapping")==Json::array({-1,-1,-1,-1}));
    REQUIRE(payload.at("ams_mapping2")[3]==Json({{"ams_id",255},{"slot_id",0}}));
    REQUIRE(payload.at("ams_mapping_info")[3].at("ams")==255);
    REQUIRE(payload.at("ams_mapping_info")[3].at("filamentType")=="PLA");
    REQUIRE(payload.at("ams_mapping_info")[0].at("filamentType")=="");
}
