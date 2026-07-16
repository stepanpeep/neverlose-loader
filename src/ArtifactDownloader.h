#pragma once
#include "Models.h"
#include <atomic>
#include <filesystem>
#include <functional>

class ArtifactDownloader {
public:
    using Progress = std::function<void(size_t current, size_t total, const std::wstring& file)>;
    bool ensure(const std::vector<Artifact>& artifacts, const std::filesystem::path& root,
                std::atomic_bool& cancelled, Progress progress, std::wstring& error) const;
    static std::wstring fileHash(const std::filesystem::path& file, const std::wstring& algorithm);
};
