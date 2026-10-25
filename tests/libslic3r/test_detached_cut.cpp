#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <cmath>
#include <string>

TEST_CASE("Detached plane cuts preserve both halves without a material", "[DetachedCut]")
{
    Slic3r::Model model;
    auto* object=model.add_object("cube","",Slic3r::TriangleMesh(Slic3r::its_make_cube(20,20,20)));
    object->add_instance();
    Slic3r::Cut cut(object,0,Slic3r::Geometry::translation_transform(Slic3r::Vec3d(0,0,10)),
        Slic3r::ModelObjectCutAttribute::KeepUpper|Slic3r::ModelObjectCutAttribute::KeepLower);
    const auto& halves=cut.perform_with_plane();
    REQUIRE(halves.size()==2);
    double volume=0;
    for(const auto* half:halves) {
        REQUIRE(half->instances.size()==1);
        for(const auto* part:half->volumes) {
            REQUIRE(part->material_id().empty());
            REQUIRE(Slic3r::its_num_open_edges(part->mesh().its)==0);
            volume+=std::abs(Slic3r::its_volume(part->mesh().its));
        }
    }
    REQUIRE_THAT(volume,Catch::Matchers::WithinRel(8000.0,1e-5));
    REQUIRE(object->volumes.size()==1);
    REQUIRE_THAT(std::abs(Slic3r::its_volume(object->volumes.front()->mesh().its)),Catch::Matchers::WithinRel(8000.0,1e-5));
}

TEST_CASE("Detached plane cuts retain named material attributes", "[DetachedCut]")
{
    Slic3r::Model model;
    auto* object=model.add_object("cube","",Slic3r::TriangleMesh(Slic3r::its_make_cube(20,20,20)));
    object->add_instance();
    auto* material=model.add_material("assigned");
    material->attributes["name"]="Native material";
    object->volumes.front()->set_material_id("assigned");
    Slic3r::Cut cut(object,0,Slic3r::Geometry::translation_transform(Slic3r::Vec3d(0,0,10)),
        Slic3r::ModelObjectCutAttribute::KeepUpper|Slic3r::ModelObjectCutAttribute::KeepLower);
    const auto& halves=cut.perform_with_plane();
    REQUIRE(halves.size()==2);
    for(const auto* half:halves)for(const auto* part:half->volumes) {
        REQUIRE(part->material_id()=="assigned");
        REQUIRE(part->material()!=nullptr);
        REQUIRE(part->material()->attributes.at("name")=="Native material");
    }
}
