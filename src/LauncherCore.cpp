#include "LauncherCore.h"
#include "Utf.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <iomanip>
#include <iterator>
#include <sstream>


namespace {
std::wstring canonicalModPath(const std::wstring& value) {
    std::filesystem::path p(value);
    if (p.is_absolute() || p.empty()) return {};
    p = p.lexically_normal();
    auto it = p.begin();
    if (it == p.end() || _wcsicmp(it->c_str(), L"mods") != 0) return {};
    ++it;
    if (it == p.end() || std::next(it) != p.end() || *it == L"..") return {};
    return (std::filesystem::path(L"mods") / *it).wstring();
}

bool commitManagedMods(const std::filesystem::path& root, const std::vector<Artifact>& mods, std::wstring& error) {
    std::set<std::wstring> desired;
    for (const auto& mod : mods) { auto path = canonicalModPath(mod.path); if (!path.empty()) desired.insert(path); }
    auto state = root / L"mods" / L".neverlose-managed.txt";
    std::wifstream input(state);
    std::wstring line; std::error_code ec;
    while (std::getline(input, line)) {
        auto old = canonicalModPath(line);
        if (!old.empty() && !desired.contains(old)) std::filesystem::remove(root / old, ec);
        ec.clear();
    }
    std::filesystem::create_directories(state.parent_path(), ec);
    if (ec) { error = L"Cannot create mods directory"; return false; }
    auto temporary = state; temporary += L".tmp";
    { std::wofstream output(temporary, std::ios::trunc); if (!output) { error=L"Cannot save managed mods state"; return false; } for (const auto& item : desired) output << item << L'\n'; }
    std::filesystem::remove(state, ec); ec.clear();
    std::filesystem::rename(temporary, state, ec);
    if (ec) { std::filesystem::remove(temporary, ec); error=L"Cannot commit managed mods state"; return false; }
    return true;
}
}

LauncherCore::LauncherCore() = default;

bool LauncherCore::bootstrap() {
    settings_ = settingsService_.load();
    return refreshManifest();
}

bool LauncherCore::refreshManifest() {
    LauncherManifest next; std::wstring error;
    if (!manifestService_.load(settings_.manifestUrl, next, error)) {
        if (settings_.manifestUrl.rfind(L"http", 0) != 0 || !manifestService_.load(L"manifest\\manifest.example.json", next, error)) {
            status_ = error; return false;
        }
    }
    manifest_ = std::move(next);
    if (selectedVersionIndex() >= manifest_.versions.size()) settings_.selectedVersion = manifest_.versions.front().id;
    if (manifest_.presets.empty()) settings_.selectedPreset.clear();
    else if (selectedPresetIndex() >= manifest_.presets.size()) settings_.selectedPreset = manifest_.presets.front().id;
    applyPreset();
    status_ = manifest_.maintenance ? (manifest_.maintenanceMessage.empty() ? L"Maintenance" : manifest_.maintenanceMessage) : L"Ready to launch";
    return true;
}

bool LauncherCore::saveSettings() {
    if (settings_.nickname.empty()) { status_ = L"Nickname is required"; return false; }
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
    auto index = selectedVersionIndex(); return index < manifest_.versions.size() ? &manifest_.versions[index] : nullptr;
}
void LauncherCore::selectVersion(size_t index) {
    if (index < manifest_.versions.size()) { settings_.selectedVersion = manifest_.versions[index].id; status_ = L"Version selected: " + manifest_.versions[index].name; }
}
void LauncherCore::selectPreset(size_t index) {
    if (index < manifest_.presets.size()) { settings_.selectedPreset = manifest_.presets[index].id; applyPreset(); status_ = L"Preset applied: " + manifest_.presets[index].name; }
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
    if (manifest_.maintenance) { status_ = manifest_.maintenanceMessage.empty() ? L"Launcher is under maintenance" : manifest_.maintenanceMessage; return false; }
    if (settings_.nickname.empty()) { status_ = L"Enter a nickname"; return false; }
    if (settings_.installDir.empty()) { status_ = L"Choose an install directory"; return false; }
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
    if (!mods.empty() && !downloader_.ensure(mods, settings_.installDir, cancelled, progress, status_)) return false;
    if (cancelled) { status_ = L"Cancelled"; return false; }
    if (!commitManagedMods(settings_.installDir, mods, status_)) return false;

    std::filesystem::path localJava = std::filesystem::path(settings_.installDir) / L"runtime" / L"bin" / L"javaw.exe";
    std::wstring java = std::filesystem::exists(localJava) ? localJava.wstring() : GameInstaller::findJava();
    if (java.empty()) { status_ = L"Java 21 not found. Install Temurin 21 or place it in runtime/bin"; return false; }

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
