#pragma once
#include "Domain.hpp"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace Slic3r::AgentControl {
// Native parsers only see an immutable private copy of an actually opened,
// root-validated regular input. The copy keeps the original format suffix.
class InputSnapshot {
public:
    explicit InputSnapshot(const Json& source, std::uintmax_t max_bytes = 128ULL * 1024 * 1024);
    ~InputSnapshot();
    InputSnapshot(const InputSnapshot&) = delete;
    InputSnapshot& operator=(const InputSnapshot&) = delete;
    const std::filesystem::path& path() const;
    const std::filesystem::path& original() const;
    const std::string& sha256() const;
    std::uintmax_t size() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// Native exporters write in a private temporary directory. Publication copies
// through a pinned output directory handle, then atomically links/renames the
// complete artifact. No-replace mode never clobbers a racing destination.
class OutputArtifact {
public:
    OutputArtifact(const Json& destination, bool overwrite = false);
    ~OutputArtifact();
    OutputArtifact(const OutputArtifact&) = delete;
    OutputArtifact& operator=(const OutputArtifact&) = delete;
    const std::filesystem::path& path() const;
    const std::filesystem::path& destination() const;
    Json publish();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace Slic3r::AgentControl
