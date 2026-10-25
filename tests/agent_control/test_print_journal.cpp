#include <system_error>
#include <catch2/catch_test_macros.hpp>
#include "slic3r/AgentControl/PrintJournal.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

using Slic3r::AgentControl::PrintJournal;
namespace fs=std::filesystem;
namespace {
struct Temporary {
    fs::path path;
    Temporary() {
        static std::atomic<unsigned> next{0};
        path=fs::temp_directory_path()/("orca-print-journal-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(next.fetch_add(1)));
    }
    ~Temporary(){std::error_code error;fs::remove_all(path,error);}
};
}
TEST_CASE("A print intent is consumed once across concurrent callers", "[PrintJournal]") {
    Temporary temporary;PrintJournal log(temporary.path);std::atomic<int> claimed{0},failed{0};
    std::vector<std::thread> workers;
    for(int i=0;i<12;++i)workers.emplace_back([&](){try{if(log.claim("same-operation",{{"action","start"}}))++claimed;}catch(...){++failed;}});
    for(auto& worker:workers)worker.join();
    REQUIRE(claimed.load()==1);REQUIRE(failed.load()==0);
    PrintJournal restarted(temporary.path);
    REQUIRE_FALSE(restarted.claim("same-operation",{{"action","start"}}));
    REQUIRE(restarted.read("same-operation")["outcome"]["state"]=="outcome_unknown");
}
TEST_CASE("A durable outcome preserves the immutable consumed intent", "[PrintJournal]") {
    Temporary temporary;PrintJournal log(temporary.path);
    REQUIRE(log.claim("accepted",{{"artifact_sha256","frozen-hash"},{"device_id","test-device"}}));
    log.record_outcome("accepted",{{"state","uploaded"},{"physical_start_confirmed",false}});
    PrintJournal restarted(temporary.path);auto record=restarted.read("accepted");
    REQUIRE(record["intent"]["artifact_sha256"]=="frozen-hash");
    REQUIRE(record["outcome"]["state"]=="uploaded");
    REQUIRE_FALSE(restarted.claim("accepted",{{"artifact_sha256","different-hash"}}));
    REQUIRE_THROWS(log.record_outcome("orphan",{{"state","uploaded"}}));
    REQUIRE(log.read("absent").is_null());
}
TEST_CASE("Incomplete print outcomes never release a consumed claim", "[PrintJournal]") {
    Temporary temporary;PrintJournal log(temporary.path);REQUIRE(log.claim("partial",{{"action","start"}}));
    {std::ofstream broken(temporary.path/"partial.outcome.json");broken<<"{incomplete";}
    REQUIRE_THROWS(log.read("partial"));
    REQUIRE_FALSE(log.claim("partial",{{"action","start"}}));
}
TEST_CASE("Print journal paths cannot escape the private directory", "[PrintJournal]") {
    Temporary temporary;PrintJournal log(temporary.path);
    REQUIRE_THROWS(log.claim("../escape",{{"action","start"}}));
    REQUIRE_THROWS(log.claim("bad/name",{{"action","start"}}));
#ifndef _WIN32
    Temporary link;fs::create_directory_symlink(temporary.path,link.path);
    REQUIRE_THROWS(PrintJournal(link.path));
#endif
}
#ifndef _WIN32
TEST_CASE("Separate processes cannot transmit the same print intent twice", "[PrintJournal]") {
    Temporary temporary;PrintJournal log(temporary.path);std::vector<pid_t> processes;
    for(int i=0;i<4;++i) {
        const pid_t pid=::fork();REQUIRE(pid>=0);
        if(pid==0) {
            try{PrintJournal child(temporary.path);::_exit(child.claim("cross-process",{{"action","start"}})?0:1);}catch(...){::_exit(2);}
        }
        processes.push_back(pid);
    }
    int winners=0;
    for(pid_t child:processes) {int status=0;REQUIRE(::waitpid(child,&status,0)==child);REQUIRE(WIFEXITED(status));REQUIRE(WEXITSTATUS(status)<=1);if(WEXITSTATUS(status)==0)++winners;}
    REQUIRE(winners==1);
}
#endif
