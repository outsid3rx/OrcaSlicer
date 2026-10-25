#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include "slic3r/AgentControl/FileAccess.hpp"
#include "slic3r/AgentControl/Domain.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdlib.h>
#include <exception>
#include <filesystem>
#include <ios>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {
namespace fs=std::filesystem;
using Slic3r::AgentControl::Json;
using Slic3r::AgentControl::DomainError;
using Slic3r::AgentControl::InputSnapshot;
using Slic3r::AgentControl::OutputArtifact;
void env(const char* key,const std::string& value) {
#ifdef _WIN32
    REQUIRE(_putenv_s(key,value.c_str())==0);
#else
    // setenv is declared by the public POSIX <stdlib.h>; Darwin wraps a private SDK header.
    REQUIRE(::setenv(key,value.c_str(),1)==0); // NOLINT(misc-include-cleaner)
#endif
}
void write(const fs::path& path,const std::string& value){std::ofstream out(path,std::ios::binary);out<<value;REQUIRE(out.good());}
std::string read(const fs::path& path){std::ifstream in(path,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
struct Files {
    fs::path root;
    Files():root(fs::temp_directory_path()/("orca-agent-file-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        fs::create_directories(root/"input");fs::create_directories(root/"output");fs::create_directories(root/"outside");
        env("ORCA_AGENT_INPUT_ROOTS",Json::array({(root/"input").u8string()}).dump());
        env("ORCA_AGENT_OUTPUT_ROOTS",Json::array({(root/"output").u8string()}).dump());
    }
    ~Files(){std::error_code error;fs::remove_all(root,error);}
};
}

TEST_CASE("Agent file snapshots isolate native parsers from input changes", "[AgentControlFiles]") {
    Files files;write(files.root/"input/model.stl","original");
    InputSnapshot snapshot((files.root/"input/model.stl").u8string());
    REQUIRE(snapshot.size()==8);REQUIRE(snapshot.sha256().size()==64);REQUIRE(snapshot.path().extension()==".stl");
    write(files.root/"input/model.stl","changed");REQUIRE(read(snapshot.path())=="original");
    fs::remove(files.root/"input/model.stl");REQUIRE(read(snapshot.path())=="original");
#ifndef _WIN32
    struct stat info{};REQUIRE(::stat(snapshot.path().c_str(),&info)==0);REQUIRE((info.st_mode&0777)==0600);
    REQUIRE(::stat(snapshot.path().parent_path().c_str(),&info)==0);REQUIRE((info.st_mode&0777)==0700);
#endif
    write(files.root/"outside/secret.stl","not admitted");
    REQUIRE_THROWS_AS(InputSnapshot((files.root/"outside/secret.stl").u8string()),DomainError);
    write(files.root/"input/large.stl","larger than two bytes");
    REQUIRE_THROWS_AS(InputSnapshot((files.root/"input/large.stl").u8string(),2),DomainError);
    std::error_code error;fs::create_symlink(files.root/"outside/secret.stl",files.root/"input/link.stl",error);
    if(!error)REQUIRE_THROWS_AS(InputSnapshot((files.root/"input/link.stl").u8string()),DomainError);
    else WARN("Symlink fixture unavailable on this host; outside-root checks still ran");
}

TEST_CASE("Agent output publication is atomic, private and does not clobber racers", "[AgentControlFiles]") {
    Files files;
    {
        OutputArtifact out((files.root/"output/model.stl").u8string());write(out.path(),"complete");
        REQUIRE_FALSE(fs::exists(files.root/"output/model.stl"));
        const auto receipt=out.publish();REQUIRE(receipt["size"]==8);REQUIRE(receipt["sha256"].get<std::string>().size()==64);
        REQUIRE(read(files.root/"output/model.stl")=="complete");
    }
    {
        OutputArtifact out((files.root/"output/race.stl").u8string());write(out.path(),"must not overwrite");
        write(files.root/"output/race.stl","competing output");REQUIRE_THROWS_AS(out.publish(),DomainError);
        REQUIRE(read(files.root/"output/race.stl")=="competing output");
    }
    {
        OutputArtifact out((files.root/"output/model.stl").u8string(),true);write(out.path(),"replacement");out.publish();
        REQUIRE(read(files.root/"output/model.stl")=="replacement");
    }
    fs::path colliding_stage;
    {
        OutputArtifact out((files.root/"output/collision.stl").u8string());write(out.path(),"export");
        colliding_stage=files.root/"output"/("."+out.path().parent_path().filename().string()+".publish");
        write(colliding_stage,"belongs to another creator");
        REQUIRE_THROWS_AS(out.publish(),DomainError);
    }
    REQUIRE(read(colliding_stage)=="belongs to another creator");fs::remove(colliding_stage);
    {
        fs::create_directory(files.root/"output/parent");OutputArtifact out((files.root/"output/parent/result.stl").u8string());write(out.path(),"private");
        fs::rename(files.root/"output/parent",files.root/"outside/moved");fs::create_directory(files.root/"output/parent");
        REQUIRE_THROWS_AS(out.publish(),DomainError);REQUIRE_FALSE(fs::exists(files.root/"outside/moved/result.stl"));REQUIRE_FALSE(fs::exists(files.root/"output/parent/result.stl"));
    }
    for(const auto& file:fs::recursive_directory_iterator(files.root/"output"))REQUIRE(file.path().filename().string().find(".publish")==std::string::npos);
}

#ifndef _WIN32
TEST_CASE("Agent snapshots reject outside files during parent-directory rebinding", "[AgentControlFiles]") {
    Files files;fs::create_directory(files.root/"input/slot");write(files.root/"input/slot/model.stl","allowed");write(files.root/"outside/model.stl","outside");
    std::atomic<bool> stop{false};
    std::thread attacker([&] {
        while(!stop.load()) {
            std::error_code error;fs::rename(files.root/"input/slot",files.root/"input/held",error);if(error)continue;
            fs::create_directory_symlink(files.root/"outside",files.root/"input/slot",error);
            fs::remove(files.root/"input/slot",error);fs::rename(files.root/"input/held",files.root/"input/slot",error);
        }
    });
    bool escaped=false;
    try {
        for(int attempt=0;attempt<100;++attempt) {
            try {InputSnapshot snapshot((files.root/"input/slot/model.stl").u8string());escaped|=read(snapshot.path())!="allowed";}
            catch(const DomainError&){}catch(const fs::filesystem_error&){}
        }
    }catch(...){stop.store(true);attacker.join();throw;}
    stop.store(true);attacker.join();REQUIRE_FALSE(escaped);
}
#endif

#ifndef _WIN32
TEST_CASE("Agent file access rejects FIFOs without waiting for a writer", "[AgentControlFiles]") {
    Files files;
    REQUIRE(::mkfifo((files.root/"input/pipe.stl").c_str(),0600)==0);
    REQUIRE_THROWS_AS(InputSnapshot((files.root/"input/pipe.stl").u8string()),DomainError);
    OutputArtifact out((files.root/"output/pipe.stl").u8string());
    REQUIRE(::mkfifo(out.path().c_str(),0600)==0);
    REQUIRE_THROWS_AS(out.publish(),DomainError);
    REQUIRE_FALSE(fs::exists(files.root/"output/pipe.stl"));
}
#endif
