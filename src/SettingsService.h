#pragma once
#include "Models.h"
#include <filesystem>

class SettingsService {
public:
    LauncherSettings load() const;
    bool save(const LauncherSettings& settings) const;
    std::filesystem::path settingsPath() const;
};
