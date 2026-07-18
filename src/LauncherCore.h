#pragma once

#include "ArtifactDownloader.h"
#include "GameInstaller.h"
#include "ManifestService.h"
#include "ModrinthService.h"
#include "Models.h"
#include "SettingsService.h"

#include <atomic>
#include <string>
#include <vector>

class LauncherCore {
public:
    LauncherCore();

    bool bootstrap();
    bool refreshManifest();
    bool saveSettings();
    bool updateRequired() const;
    static const wchar_t* currentVersion();
    bool launch(std::atomic_bool& cancelled, ArtifactDownloader::Progress progress);

    void selectVersion(size_t index);
    void selectPreset(size_t index);
    void toggleModule(size_t index);
    bool moduleEnabled(size_t index) const;

    LauncherSettings& settings() { return settings_; }
    const LauncherSettings& settings() const { return settings_; }
    const LauncherManifest& manifest() const { return manifest_; }
    const std::wstring& status() const { return status_; }
    void setStatus(std::wstring value) { status_ = std::move(value); }

    size_t selectedVersionIndex() const;
    size_t selectedPresetIndex() const;

private:
    const VersionEntry* selectedVersion() const;
    void applyPreset();

    static std::wstring quoted(const std::wstring& value);
    static std::wstring offlineUuid(const std::wstring& nickname);

    SettingsService settingsService_;
    ManifestService manifestService_;
    ModrinthService modrinthService_;
    ArtifactDownloader downloader_;
    GameInstaller gameInstaller_;

    LauncherSettings settings_;
    LauncherManifest manifest_;
    std::vector<std::wstring> enabledModules_;
    std::wstring status_ = L"Ready";
};
