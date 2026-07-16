#pragma once
#include "ArtifactDownloader.h"
#include "Models.h"
#include <atomic>
#include <filesystem>

class GameInstaller {
public:
    bool prepare(const std::filesystem::path& root, const std::wstring& gameVersion,
                 std::atomic_bool& cancelled, ArtifactDownloader::Progress progress,
                 GameInstallResult& result, std::wstring& error) const;
    static std::wstring findJava();

private:
    static bool extractNatives(const std::vector<std::filesystem::path>& archives,
                               const std::filesystem::path& destination, std::wstring& error);
};
