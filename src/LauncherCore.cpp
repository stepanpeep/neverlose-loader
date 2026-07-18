#include "LauncherCore.h"
#include "JavaRuntimeService.h"
#include "Utf.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <sstream>


namespace {
constexpr const wchar_t* kOfficialManifestUrl = L"https://api.github.com/repos/stepanpeep/neverlose-loader/contents/manifest/manifest.example.json?ref=main";
constexpr const wchar_t* kRawManifestUrl = L"https://raw.githubusercontent.com/stepanpeep/neverlose-loader/main/manifest/manifest.example.json";
constexpr const wchar_t* kCdnManifestUrl = L"https://cdn.jsdelivr.net/gh/stepanpeep/neverlose-loader@main/manifest/manifest.example.json";
constexpr const wchar_t* kLauncherVersion = L"1.0.3";
std::atomic_bool onlineManifestVerified{false};

std::wstring freshUrl(const wchar_t* base) {
    FILETIME time{};
    GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    std::wstring url(base);
    url += url.find(L'?') == std::wstring::npos ? L"?nl=" : L"&nl=";
    return url + std::to_wstring(value.QuadPart);
}

std::vector<int> versionParts(const std::wstring& value) {
    std::vector<int> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(L'.', start);
        std::wstring part = value.substr(start, end == std::wstring::npos ? value.size() - start : end - start);
        try { parts.push_back(part.empty() ? 0 : std::stoi(part)); } catch (...) { parts.push_back(0); }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    parts.resize(std::max<size_t>(3, parts.size()), 0);
    return parts;
}

bool requiresUpdate(const LauncherManifest& manifest) {
    const auto current = versionParts(kLauncherVersion);
    if (!manifest.latestVersion.empty() && current != versionParts(manifest.latestVersion)) return true;
    if (!manifest.minimumVersion.empty() && current != versionParts(manifest.minimumVersion)) return true;
    return false;
}

void removeLegacyManagedState(const std::filesystem::path& root) {
    std::error_code ignored;
    const auto state = root / L"mods" / L".neverlose-managed.txt";
    std::filesystem::remove(state, ignored);
    ignored.clear();
    std::filesystem::remove(state.wstring() + L".tmp", ignored);
}
}

LauncherCore::LauncherCore() = default;

bool LauncherCore::bootstrap() {
    settings_ = settingsService_.load();
    settings_.manifestUrl = kOfficialManifestUrl;
    return refreshManifest();
}

bool LauncherCore::refreshManifest() {
    onlineManifestVerified = false;
    LauncherManifest next;
    std::wstring primaryError;
    std::wstring rawError;
    std::wstring cdnError;
    std::wstring cacheError;
    bool loaded = manifestService_.load(freshUrl(kOfficialManifestUrl), next, primaryError);
    settings_.manifestUrl = kOfficialManifestUrl;
    if (!loaded) {
        loaded = manifestService_.load(freshUrl(kRawManifestUrl), next, rawError);
        if (loaded) settings_.manifestUrl = kRawManifestUrl;
    }
    if (!loaded) {
        loaded = manifestService_.load(freshUrl(kCdnManifestUrl), next, cdnError);
        if (loaded) settings_.manifestUrl = kCdnManifestUrl;
    }
    if (loaded) onlineManifestVerified = true;
    else loaded = manifestService_.loadCached(next, cacheError);
    if (!loaded) {
        status_ = L"Unable to verify online manifest: " + primaryError;
        if (!rawError.empty() && rawError != primaryError) status_ += L"; raw: " + rawError;
        if (!cdnError.empty() && cdnError != rawError) status_ += L"; CDN: " + cdnError;
        return false;
    }
    manifest_ = std::move(next);
    size_t versionIndex = selectedVersionIndex();
    if (versionIndex >= manifest_.versions.size() || !manifest_.versions[versionIndex].available) {
        auto available = std::find_if(manifest_.versions.begin(), manifest_.versions.end(), [](const VersionEntry& entry) { return entry.available; });
        if (available == manifest_.versions.end()) { status_ = L"No client versions are available"; return false; }
        settings_.selectedVersion = available->id;
    }
    if (manifest_.presets.empty()) settings_.selectedPreset.clear();
    else if (selectedPresetIndex() >= manifest_.presets.size()) settings_.selectedPreset = manifest_.presets.front().id;
    applyPreset();
    if (!onlineManifestVerified) status_ = L"Online manifest verification failed. Launch is blocked.";
    else status_ = manifest_.maintenance ? (manifest_.maintenanceMessage.empty() ? L"Maintenance" : manifest_.maintenanceMessage) : L"Ready to launch";
    return true;
}

const wchar_t* LauncherCore::currentVersion() { return kLauncherVersion; }

bool LauncherCore::manifestVerifiedOnline() const { return onlineManifestVerified.load(); }

bool LauncherCore::updateRequired() const {
    return requiresUpdate(manifest_);
}

bool LauncherCore::saveSettings() {
    if (settings_.nickname.empty()) { status_ = L"Enter a Minecraft nickname"; return false; }
    bool ok = settingsService_.save(settings_);
    status_ = ok ? L"Settings saved" : L"Unable to save settings";
    return ok;
}

size_t LauncherCore::selectedVersionIndex() const {
    for (size_t i = 0; i < manifest_.versions.size(); ++i) if (manifest_.versions[i].id == settings_.selectedVersion) return i;
    return manifest_.versions.size();
}
size_t LauncherCore::selectedPresetIndex() const {
    for (size_t i = 0; i < manifest_.presets.size(); ++i) if (manifest_.presets[i].id == settings_.selectedPreset) return i;
    return manifest_.presets.size();
}
const VersionEntry* LauncherCore::selectedVersion() const {
    auto index = selectedVersionIndex();
    return index < manifest_.versions.size() && manifest_.versions[index].available ? &manifest_.versions[index] : nullptr;
}
void LauncherCore::selectVersion(size_t index) {
    if (index >= manifest_.versions.size()) return;
    if (!manifest_.versions[index].available) { status_ = L"This version is coming soon"; return; }
    settings_.selectedVersion = manifest_.versions[index].id;
    status_ = L"Selected version: " + manifest_.versions[index].name;
}
void LauncherCore::selectPreset(size_t index) {
    if (index < manifest_.presets.size()) { settings_.selectedPreset = manifest_.presets[index].id; applyPreset(); status_ = L"Applied preset: " + manifest_.presets[index].name; }
}
void LauncherCore::applyPreset() {
    enabledModules_.clear(); auto index = selectedPresetIndex(); if (index < manifest_.presets.size()) enabledModules_ = manifest_.presets[index].modules;
}
bool LauncherCore::moduleEnabled(size_t index) const {
    return index < manifest_.modules.size() && std::find(enabledModules_.begin(), enabledModules_.end(), manifest_.modules[index].id) != enabledModules_.end();
}
void LauncherCore::toggleModule(size_t index) {
    if (index >= manifest_.modules.size()) return;
    const auto& id = manifest_.modules[index].id;
    auto it = std::find(enabledModules_.begin(), enabledModules_.end(), id);
    if (it == enabledModules_.end()) enabledModules_.push_back(id); else enabledModules_.erase(it);
}

std::wstring LauncherCore::quoted(const std::wstring& value) {
    std::wstring out = L"\""; size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'\"') { out.append(slashes * 2 + 1, L'\\'); out += c; slashes = 0; continue; }
        out.append(slashes, L'\\'); slashes = 0; out += c;
    }
    out.append(slashes * 2, L'\\'); out += L'\"'; return out;
}

std::wstring LauncherCore::offlineUuid(const std::wstring& nickname) {
    std::string bytes = "OfflinePlayer:" + WideToUtf8(nickname);
    BCRYPT_ALG_HANDLE provider = nullptr; BCRYPT_HASH_HANDLE hash = nullptr; DWORD objectSize = 0, hashSize = 0, received = 0;
    if (BCryptOpenAlgorithmProvider(&provider, BCRYPT_MD5_ALGORITHM, nullptr, 0) < 0) return L"00000000000000000000000000000000";
    BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &received, 0);
    BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &received, 0);
    std::vector<UCHAR> object(objectSize), digest(hashSize);
    BCryptCreateHash(provider, &hash, object.data(), objectSize, nullptr, 0, 0);
    BCryptHashData(hash, reinterpret_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0);
    BCryptFinishHash(hash, digest.data(), hashSize, 0);
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(provider, 0);
    digest[6] = static_cast<UCHAR>((digest[6] & 0x0f) | 0x30); digest[8] = static_cast<UCHAR>((digest[8] & 0x3f) | 0x80);
    std::wostringstream result; result << std::hex << std::setfill(L'0'); for (auto byte : digest) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

bool LauncherCore::launch(std::atomic_bool& cancelled, ArtifactDownloader::Progress progress) {
    LauncherManifest accessManifest;
    std::wstring accessError;
    std::wstring rawAccessError;
    std::wstring cdnAccessError;
    bool accessLoaded = manifestService_.load(freshUrl(kOfficialManifestUrl), accessManifest, accessError);
    if (!accessLoaded) accessLoaded = manifestService_.load(freshUrl(kRawManifestUrl), accessManifest, rawAccessError);
    if (!accessLoaded) accessLoaded = manifestService_.load(freshUrl(kCdnManifestUrl), accessManifest, cdnAccessError);
    onlineManifestVerified = accessLoaded;
    if (!onlineManifestVerified.load()) {
        status_ = L"Online manifest verification is required before launch: " + accessError;
        if (!rawAccessError.empty() && rawAccessError != accessError) status_ += L"; raw: " + rawAccessError;
        if (!cdnAccessError.empty() && cdnAccessError != rawAccessError) status_ += L"; CDN: " + cdnAccessError;
        return false;
    }
    if (requiresUpdate(accessManifest)) { status_ = L"Loader update required"; return false; }
    if (accessManifest.maintenance) { status_ = accessManifest.maintenanceMessage.empty() ? L"The loader is under maintenance" : accessManifest.maintenanceMessage; return false; }
    if (updateRequired()) { status_ = L"Loader update required"; return false; }
    if (manifest_.maintenance) { status_ = manifest_.maintenanceMessage.empty() ? L"The loader is under maintenance" : manifest_.maintenanceMessage; return false; }
    if (settings_.nickname.empty()) { status_ = L"Enter a Minecraft nickname"; return false; }
    if (settings_.installDir.empty()) { status_ = L"Choose an installation directory"; return false; }
    const auto* version = selectedVersion(); if (!version) { status_ = L"Select a version"; return false; }

    GameInstallResult install;
    status_ = L"Checking Minecraft and Fabric files";
    if (!gameInstaller_.prepare(settings_.installDir, version->minecraftVersion, cancelled, progress, install, status_)) return false;

    std::vector<Artifact> mods;
    for (size_t i = 0; i < manifest_.modules.size(); ++i) {
        if (!moduleEnabled(i)) continue;
        const auto& module = manifest_.modules[i];
        if (!module.modrinthProject.empty()) {
            Artifact resolved; std::wstring error;
            if (!modrinthService_.resolveLatest(module.modrinthProject, version->minecraftVersion, resolved, error)) { status_ = error; return false; }
            mods.push_back(std::move(resolved));
        }
        mods.insert(mods.end(), module.artifacts.begin(), module.artifacts.end());
    }
    mods.insert(mods.end(), version->artifacts.begin(), version->artifacts.end());
    removeLegacyManagedState(settings_.installDir);
    if (!mods.empty() && !downloader_.ensure(mods, settings_.installDir, cancelled, progress, status_)) return false;
    if (cancelled) { status_ = L"Operation cancelled"; return false; }
    removeLegacyManagedState(settings_.installDir);

    JavaRuntimeService javaRuntime;
    std::wstring java;
    status_ = L"Checking Java 21 runtime";
    if (!javaRuntime.resolve(settings_.installDir, settings_.javaMode, cancelled, progress, java, status_)) return false;

    std::wstring classpath;
    for (const auto& relative : install.classpath) {
        if (!classpath.empty()) classpath += L';';
        classpath += (std::filesystem::path(settings_.installDir) / relative).wstring();
    }
    std::filesystem::path root(settings_.installDir);
    std::wstring command = quoted(java)
        + L" -Xms512M -Xmx" + std::to_wstring(settings_.ramMb) + L"M"
        + L" -Djava.library.path=" + quoted((root / install.nativesDir).wstring())
        + L" -Dminecraft.launcher.brand=neverlose-loader -Dminecraft.launcher.version=1.0"
        + (install.loggingConfig.empty() ? L"" : L" -Dlog4j.configurationFile=" + quoted((root / install.loggingConfig).wstring()))
        + L" -cp " + quoted(classpath) + L" " + install.mainClass
        + L" --username " + quoted(settings_.nickname)
        + L" --version " + quoted(install.versionId)
        + L" --gameDir " + quoted(root.wstring())
        + L" --assetsDir " + quoted((root / L"assets").wstring())
        + L" --assetIndex " + quoted(install.assetIndex)
        + L" --uuid " + offlineUuid(settings_.nickname)
        + L" --accessToken 0 --clientId 0 --xuid 0 --userType legacy --versionType release --userProperties {}";

    settings_.firstRunComplete = true; settingsService_.save(settings_);
    STARTUPINFOW startup{sizeof(startup)}; PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(0);
    BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, settings_.installDir.c_str(), &startup, &process);
    if (started) { CloseHandle(process.hThread); CloseHandle(process.hProcess); status_ = L"Minecraft started"; return true; }
    status_ = L"Failed to start Java (error " + std::to_wstring(GetLastError()) + L")"; return false;
}
