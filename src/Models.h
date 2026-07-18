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
    bool available = true;
    std::wstring badge;
    std::vector<std::wstring> arguments;
    std::vector<Artifact> artifacts;
};

struct LauncherManifest {
    bool maintenance = false;
    std::wstring maintenanceMessage;
    std::wstring minimumVersion;
    std::wstring updateUrl = L"https://discord.gg/WbZarvYWgX";
    std::wstring updateMessage = L"This loader version is outdated. Install the latest build.";
    std::wstring title = L"Neverlose Loader";
    std::vector<VersionEntry> versions;
    std::vector<PresetEntry> presets;
    std::vector<ModuleEntry> modules;
};

struct LauncherSettings {
    std::wstring nickname;
    std::wstring avatarPath;
    std::wstring language = L"en";
    std::wstring installDir = L"C:\\NeverloseClient";
    std::wstring manifestUrl = L"https://raw.githubusercontent.com/stepanpeep/neverlose-loader/main/manifest/manifest.example.json";
    std::wstring selectedVersion = L"1.21";
    std::wstring selectedPreset;
    int theme = 1;
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
