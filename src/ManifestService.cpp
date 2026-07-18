#include "ManifestService.h"
#include "MiniJson.h"
#include "Utf.h"
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
bool isWeb(const std::wstring& value) {
    return value.rfind(L"https://", 0) == 0 || value.rfind(L"http://", 0) == 0;
}

std::filesystem::path appDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path cachePath() {
    PWSTR raw = nullptr;
    std::filesystem::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        base = raw;
        CoTaskMemFree(raw);
    } else {
        base = std::filesystem::temp_directory_path();
    }
    base /= L"NeverloseLoader";
    std::error_code ignored;
    std::filesystem::create_directories(base, ignored);
    return base / L"manifest-cache.json";
}

std::wstring windowsError(DWORD code) {
    wchar_t* message = nullptr;
    DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = length && message ? std::wstring(message, length) : L"Windows error " + std::to_wstring(code);
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' ')) result.pop_back();
    return result;
}

bool readFile(const std::filesystem::path& path, std::string& data, std::wstring& error, size_t maximumBytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = L"Cannot open file: " + path.wstring();
        return false;
    }
    input.seekg(0, std::ios::end);
    auto length = input.tellg();
    input.seekg(0, std::ios::beg);
    if (length < 0 || maximumBytes && static_cast<unsigned long long>(length) > maximumBytes) {
        error = L"Local response is too large";
        return false;
    }
    std::ostringstream output;
    output << input.rdbuf();
    data = output.str();
    return true;
}

void saveCache(const std::string& data) {
    auto cache = cachePath();
    auto temporary = cache;
    temporary += L".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    output.close();
    if (!output) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return;
    }
    std::error_code error;
    std::filesystem::rename(temporary, cache, error);
    if (error) {
        error.clear();
        std::filesystem::remove(cache, error);
        error.clear();
        std::filesystem::rename(temporary, cache, error);
    }
}

bool fetchHttpOnce(const std::wstring& source, DWORD accessType, std::string& data, std::wstring& error, size_t maximumBytes) {
    data.clear();
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(source.c_str(), static_cast<DWORD>(source.size()), 0, &parts)) {
        error = L"Invalid URL: " + windowsError(GetLastError());
        return false;
    }

    HINTERNET session = WinHttpOpen(L"NeverloseLoader/1.0.3", accessType, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = L"WinHTTP initialization failed: " + windowsError(GetLastError());
        return false;
    }

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    bool githubApi = host == L"api.github.com";
    bool smallRequest = maximumBytes <= 16 * 1024 * 1024;
    WinHttpSetTimeouts(session,
                       smallRequest ? 3000 : 6000,
                       smallRequest ? 3500 : 6000,
                       smallRequest ? 5000 : 12000,
                       smallRequest ? (githubApi ? 7000 : 9000) : 30000);
    DWORD connectRetries = 1;
    WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_RETRIES, &connectRetries, sizeof(connectRetries));
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connection) {
        DWORD code = GetLastError();
        WinHttpCloseHandle(session);
        error = L"Connection failed: " + windowsError(code);
        return false;
    }

    std::wstring path;
    if (parts.dwUrlPathLength) path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    const wchar_t* accepted[] = {L"application/vnd.github.raw+json", L"application/json", L"text/plain", L"*/*", nullptr};
    HINTERNET request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, accepted, flags);
    if (!request) {
        DWORD code = GetLastError();
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = L"Request creation failed: " + windowsError(code);
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));
#ifdef WINHTTP_OPTION_DECOMPRESSION
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
#endif

    const wchar_t* headers = L"Accept: application/vnd.github.raw+json, application/json, text/plain, */*\r\nX-GitHub-Api-Version: 2022-11-28\r\nCache-Control: no-cache, no-store, max-age=0\r\nPragma: no-cache\r\nConnection: close\r\n";
    if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD code = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = L"Send failed: " + windowsError(code);
        return false;
    }
    if (!WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = L"Response failed: " + windowsError(code);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &statusSize, nullptr)) {
        DWORD code = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = L"Status query failed: " + windowsError(code);
        return false;
    }
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        error = L"Server returned HTTP " + std::to_wstring(status);
        return false;
    }

    bool success = true;
    while (success) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = L"Read failed: " + windowsError(GetLastError());
            success = false;
            break;
        }
        if (!available) break;
        if (maximumBytes && data.size() + available > maximumBytes) {
            error = L"HTTP response is too large";
            success = false;
            break;
        }
        size_t offset = data.size();
        data.resize(offset + available);
        DWORD received = 0;
        if (!WinHttpReadData(request, data.data() + offset, available, &received)) {
            error = L"Read failed: " + windowsError(GetLastError());
            success = false;
            break;
        }
        data.resize(offset + received);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (success && data.empty()) {
        error = L"Server returned an empty response";
        return false;
    }
    return success;
}

Artifact artifact(const JsonValue& value) {
    std::wstring hash = Utf8ToWide(value.get("sha256").string());
    std::wstring algorithm = L"sha256";
    if (hash.empty()) {
        hash = Utf8ToWide(value.get("sha512").string());
        algorithm = L"sha512";
    }
    if (hash.empty()) {
        hash = Utf8ToWide(value.get("sha1").string());
        algorithm = L"sha1";
    }
    return {Utf8ToWide(value.get("path").string()), Utf8ToWide(value.get("url").string()), hash, algorithm};
}

std::vector<Artifact> artifacts(const JsonValue& list) {
    std::vector<Artifact> result;
    for (const auto& value : list.array()) result.push_back(artifact(value));
    return result;
}
}

bool ManifestService::fetchBytes(const std::wstring& source, std::string& data, std::wstring& error, size_t maximumBytes) {
    data.clear();
    error.clear();
    if (!isWeb(source)) {
        auto path = std::filesystem::path(source);
        if (path.is_relative()) path = appDir() / path;
        return readFile(path, data, error, maximumBytes);
    }

    std::wstring directError;
    if (fetchHttpOnce(source, WINHTTP_ACCESS_TYPE_NO_PROXY, data, directError, maximumBytes)) return true;
    Sleep(120);

    std::wstring proxyError;
    if (fetchHttpOnce(source, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, data, proxyError, maximumBytes)) return true;
    error = directError;
    if (!proxyError.empty() && proxyError != directError) error += L"; configured proxy: " + proxyError;
    return false;
}

bool ManifestService::load(const std::wstring& source, LauncherManifest& manifest, std::wstring& error) const {
    std::string bytes;
    if (!fetchBytes(source, bytes, error, 8 * 1024 * 1024)) return false;

    JsonValue root;
    std::string parseError;
    if (!MiniJson::parse(bytes, root, parseError) || !root.isObject()) {
        error = L"Manifest JSON error: " + Utf8ToWide(parseError);
        return false;
    }
    LauncherManifest next;
    const auto& launcher = root.get("launcher");
    next.latestVersion = Utf8ToWide(launcher.get("version").string());
    next.minimumVersion = Utf8ToWide(launcher.get("minimumVersion").string());
    if (next.latestVersion.empty() && next.minimumVersion.empty()) {
        error = L"Manifest does not define launcher.version or launcher.minimumVersion";
        return false;
    }
    next.updateUrl = Utf8ToWide(launcher.get("updateUrl").string("https://discord.gg/WbZarvYWgX"));
    next.updateMessage = Utf8ToWide(launcher.get("updateMessage").string("This loader version is outdated. Install the latest build."));
    next.title = Utf8ToWide(launcher.get("branding").get("title").string("Neverlose Loader"));
    const auto& maintenance = root.get("maintenance");
    next.maintenance = maintenance.isObject() ? maintenance.get("enabled").boolean() : maintenance.boolean();
    next.maintenanceMessage = maintenance.isObject() ? Utf8ToWide(maintenance.get("message").string()) : L"";

    for (const auto& value : root.get("modules").array()) {
        ModuleEntry entry;
        entry.id = Utf8ToWide(value.get("id").string());
        entry.name = Utf8ToWide(value.get("name").string());
        entry.description = Utf8ToWide(value.get("description").string());
        entry.modrinthProject = Utf8ToWide(value.get("modrinthProject").string());
        entry.artifacts = artifacts(value.get("artifacts"));
        if (!entry.id.empty()) next.modules.push_back(std::move(entry));
    }
    for (const auto& value : root.get("presets").array()) {
        PresetEntry entry;
        entry.id = Utf8ToWide(value.get("id").string());
        entry.name = Utf8ToWide(value.get("name").string());
        for (const auto& module : value.get("modules").array()) entry.modules.push_back(Utf8ToWide(module.string()));
        if (!entry.id.empty()) next.presets.push_back(std::move(entry));
    }
    for (const auto& value : root.get("versions").array()) {
        VersionEntry entry;
        entry.id = Utf8ToWide(value.get("id").string());
        entry.name = Utf8ToWide(value.get("name").string());
        entry.minecraftVersion = Utf8ToWide(value.get("minecraftVersion").string());
        entry.loader = Utf8ToWide(value.get("loader").string());
        entry.inheritsPreset = Utf8ToWide(value.get("inheritsPreset").string());
        entry.mainClass = Utf8ToWide(value.get("mainClass").string());
        entry.available = value.get("available").boolean(true);
        entry.badge = Utf8ToWide(value.get("badge").string());
        for (const auto& argument : value.get("arguments").array()) entry.arguments.push_back(Utf8ToWide(argument.string()));
        entry.artifacts = artifacts(value.get("artifacts"));
        if (!entry.id.empty()) next.versions.push_back(std::move(entry));
    }

    const bool hasUpcoming = std::any_of(next.versions.begin(), next.versions.end(), [](const VersionEntry& entry) { return entry.id == L"1.21.4"; });
    if (!hasUpcoming) {
        VersionEntry upcoming;
        upcoming.id = L"1.21.4";
        upcoming.name = L"Minecraft 1.21.4";
        upcoming.minecraftVersion = L"1.21.4";
        upcoming.loader = L"fabric";
        upcoming.inheritsPreset = L"performance";
        upcoming.available = false;
        upcoming.badge = L"Soon";
        next.versions.push_back(std::move(upcoming));
    }
    if (next.versions.empty()) {
        error = L"Manifest contains no versions";
        return false;
    }
    if (isWeb(source)) saveCache(bytes);
    manifest = std::move(next);
    error.clear();
    return true;
}

bool ManifestService::loadCached(LauncherManifest& manifest, std::wstring& error) const {
    return load(cachePath().wstring(), manifest, error);
}
