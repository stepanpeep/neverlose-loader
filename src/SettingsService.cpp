#include "SettingsService.h"
#include <windows.h>
#include <shlobj.h>

namespace {
std::wstring readValue(const std::filesystem::path& file, const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[4096]{};
    GetPrivateProfileStringW(L"Launcher", key, fallback, value, 4096, file.c_str());
    return value;
}
bool readBool(const std::filesystem::path& file, const wchar_t* key, bool fallback) {
    return GetPrivateProfileIntW(L"Launcher", key, fallback ? 1 : 0, file.c_str()) != 0;
}
}

std::filesystem::path SettingsService::settingsPath() const {
    PWSTR raw = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        base = raw;
        CoTaskMemFree(raw);
    } else base = std::filesystem::temp_directory_path();
    base /= L"NeverloseLoader";
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base / L"launcher.ini";
}

LauncherSettings SettingsService::load() const {
    LauncherSettings s;
    auto file = settingsPath();
    s.nickname = readValue(file, L"Nickname", L"");
    s.avatarPath = readValue(file, L"AvatarPath", L"");
    s.language = readValue(file, L"Language", L"en");
    s.installDir = readValue(file, L"InstallDir", L"C:\\NeverloseClient");
    s.manifestUrl = L"https://raw.githubusercontent.com/stepanpeep/neverlose-loader/main/manifest/manifest.example.json";
    s.selectedVersion = readValue(file, L"SelectedVersion", L"1.21");
    s.selectedPreset = readValue(file, L"SelectedPreset", L"");
    s.theme = GetPrivateProfileIntW(L"Launcher", L"Theme", 1, file.c_str());
    s.ramMb = GetPrivateProfileIntW(L"Launcher", L"RamMb", 4096, file.c_str());
    s.firstRunComplete = readBool(file, L"FirstRunComplete", false);
    if (s.theme < 0 || s.theme > 2) s.theme = 1;
    if (s.ramMb < 2048 || s.ramMb > 16384) s.ramMb = 4096;
    return s;
}

bool SettingsService::save(const LauncherSettings& s) const {
    auto file = settingsPath();
    auto write = [&](const wchar_t* key, const std::wstring& value) {
        return WritePrivateProfileStringW(L"Launcher", key, value.c_str(), file.c_str()) != FALSE;
    };
    bool ok = true;
    ok &= write(L"Nickname", s.nickname);
    ok &= write(L"AvatarPath", s.avatarPath);
    ok &= write(L"Language", s.language);
    ok &= write(L"InstallDir", s.installDir);
    ok &= write(L"SelectedVersion", s.selectedVersion);
    ok &= write(L"SelectedPreset", s.selectedPreset);
    ok &= write(L"Theme", std::to_wstring(s.theme));
    ok &= write(L"RamMb", std::to_wstring(s.ramMb));
    ok &= write(L"FirstRunComplete", s.firstRunComplete ? L"1" : L"0");
    return ok;
}
