#pragma once
#include "ArtifactDownloader.h"
#include <atomic>
#include <filesystem>
#include <string>

class JavaRuntimeService {
public:
    bool resolve(const std::filesystem::path& installRoot, const std::wstring& mode,
                 std::atomic_bool& cancelled, ArtifactDownloader::Progress progress,
                 std::wstring& executable, std::wstring& error) const;

private:
    static bool isJava21(const std::filesystem::path& executable);
    static bool ensureBundled(const std::filesystem::path& installRoot,
                              std::atomic_bool& cancelled, ArtifactDownloader::Progress progress,
                              std::wstring& executable, std::wstring& error);
};
