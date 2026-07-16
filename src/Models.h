#pragma once
#include <string>
#include <vector>

struct Artifact {
    std::wstring path;
    std::wstring url;
    std::wstring hash;
    std::wstring hashAlgorithm = L"sha256";
};

struct ModuleEntry {
    std::wstring id;
    std::wstring name;
    std::wstring description;
    std::wstring modrinthProject;
    std::vector<Artifact> artifacts;
};

struct PresetEntry {
    std::wstring id;
    std::wstring name;
    std::vector<std::wstring> modules;
};

struct VersionEntry {
    std::wstring id;
    std::wstring name;
    std::wstring minecraftVersion;
    std::wstring loader;
    std::wstring inheritsPreset;
    std::wstring mainClass;
    std::vector<std::wstring> arguments;
    std::vector<Artifact> artifacts;
};

struct LauncherManifest {
    bool maintenance = false;
    std::wstring maintenanceMessage;
    std::wstring minimumVersion;
    std::wstring title = L"Neverlose Loader";
    std::vector<VersionEntry> versions;
    std::vector<PresetEntry> presets;
    std::vector<ModuleEntry> modules;
};

struct LauncherSettings {
    std::wstring nickname;
    std::wstring avatarPath;
    std::wstring language = L"ru";
    std::wstring installDir = L"C:\\NeverloseClient";
    std::wstring manifestUrl = L"manifest\\manifest.example.json";
    std::wstring selectedVersion = L"1.21";
    std::wstring selectedPreset;
    int ramMb = 4096;
    bool firstRunComplete = false;
};

struct GameInstallResult {
    std::wstring versionId;
    std::wstring mainClass;
    std::wstring assetIndex;
    std::wstring gameVersion;
    std::wstring nativesDir;
    std::wstring loggingConfig;
    std::vector<std::wstring> classpath;
};
