#pragma once
#include <filesystem>
#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r::AgentControl {
// A claim is permanent: loss of the response or process must never make a
// potentially transmitted physical job eligible for automatic retransmission.
class PrintJournal {
public:
    explicit PrintJournal(std::filesystem::path directory);
    bool claim(const std::string& operation_id, const nlohmann::json& intent) const;
    void record_outcome(const std::string& operation_id, const nlohmann::json& outcome) const;
    nlohmann::json read(const std::string& operation_id) const;
private:
    std::filesystem::path path(const std::string& id, const char* suffix) const;
    void sync_directory() const;
    std::filesystem::path m_directory;
};
}
