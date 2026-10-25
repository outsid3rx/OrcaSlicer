#include <catch2/catch_test_macros.hpp>
#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PrintConfig.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace {
struct Timestamped final : Slic3r::ObjectWithTimestamp {
    Timestamped() = default;
    ~Timestamped() override = default;
};
}

TEST_CASE("Detached model workers allocate distinct identities and change timestamps", "[IdentityAllocation]")
{
    constexpr std::size_t count=2000, workers=4;
    struct Values { std::vector<std::size_t> ids; std::vector<std::uint64_t> objects,configs; };
    std::array<Values,workers> values;
    std::array<std::thread,workers> threads;
    for(std::size_t i=0;i<workers;++i)threads[i]=std::thread([&,i](){
        auto& out=values[i];out.ids.reserve(count);out.objects.reserve(count);out.configs.reserve(count);
        for(std::size_t j=0;j<count;++j) {
            Timestamped object;object.touch();
            Slic3r::ModelConfig config;config.touch();
            out.ids.push_back(object.id().id);out.objects.push_back(object.timestamp());out.configs.push_back(config.timestamp());
        }
    });
    for(auto& thread:threads)thread.join();
    std::vector<std::size_t> ids;std::vector<std::uint64_t> object_times,config_times;
    for(auto& value:values){ids.insert(ids.end(),value.ids.begin(),value.ids.end());object_times.insert(object_times.end(),value.objects.begin(),value.objects.end());config_times.insert(config_times.end(),value.configs.begin(),value.configs.end());}
    const auto unique=[](auto& sequence){std::sort(sequence.begin(),sequence.end());return std::adjacent_find(sequence.begin(),sequence.end())==sequence.end();};
    REQUIRE(ids.size()==count*workers);
    REQUIRE(unique(ids));REQUIRE(unique(object_times));REQUIRE(unique(config_times));
    REQUIRE(ids.front()>0);REQUIRE(object_times.front()>1);REQUIRE(config_times.front()>1);
}
