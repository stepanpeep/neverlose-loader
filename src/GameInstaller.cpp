#include "GameInstaller.h"
#include "ManifestService.h"
#include "MiniJson.h"
#include "Utf.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <set>

namespace {
bool json(const std::wstring& url, JsonValue& out, std::wstring& error, size_t limit = 16 * 1024 * 1024) {
    std::string bytes, parseError;
    if (!ManifestService::fetchBytes(url, bytes, error, limit)) return false;
    if (!MiniJson::parse(bytes, out, parseError)) { error = L"JSON error: " + Utf8ToWide(parseError); return false; }
    return true;
}
std::wstring trim(std::wstring s) { while (!s.empty() && iswspace(s.front())) s.erase(s.begin()); while (!s.empty() && iswspace(s.back())) s.pop_back(); return s; }
bool allowed(const JsonValue& lib) {
    const auto& rules = lib.get("rules").array(); if (rules.empty()) return true; bool result = false;
    for (const auto& rule : rules) { auto os = rule.get("os").get("name").string(); if (os.empty() || os == "windows") result = rule.get("action").string() == "allow"; }
    return result;
}
std::wstring mavenPath(const std::wstring& name) {
    std::vector<std::wstring> p; size_t begin = 0;
    while (true) { auto end = name.find(L':', begin); p.push_back(name.substr(begin, end == std::wstring::npos ? end : end - begin)); if (end == std::wstring::npos) break; begin = end + 1; }
    if (p.size() < 3) return {}; std::replace(p[0].begin(), p[0].end(), L'.', L'/');
    std::wstring file = p[1] + L"-" + p[2] + (p.size() > 3 ? L"-" + p[3] : L"") + L".jar";
    return p[0] + L"/" + p[1] + L"/" + p[2] + L"/" + file;
}
void add(std::vector<Artifact>& files, std::set<std::wstring>& seen, Artifact a) { if (!a.path.empty() && seen.insert(a.path).second) files.push_back(std::move(a)); }
std::wstring replaceArch(std::wstring s) { auto p = s.find(L"${arch}"); if (p != std::wstring::npos) s.replace(p, 7, L"64"); return s; }
std::wstring quote(const std::wstring& s) { return L"\"" + s + L"\""; }
}

bool GameInstaller::prepare(const std::filesystem::path& root, const std::wstring& gameVersion,
                            std::atomic_bool& cancelled, ArtifactDownloader::Progress progress,
                            GameInstallResult& result, std::wstring& error) const {
    result = {}; result.gameVersion = gameVersion;
    JsonValue rootManifest; if (!json(L"https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", rootManifest, error)) return false;
    std::wstring versionUrl;
    for (const auto& v : rootManifest.get("versions").array()) if (Utf8ToWide(v.get("id").string()) == gameVersion) { versionUrl = Utf8ToWide(v.get("url").string()); break; }
    if (versionUrl.empty()) { error = L"Minecraft version not found: " + gameVersion; return false; }
    JsonValue vanilla; if (!json(versionUrl, vanilla, error)) return false;

    std::vector<Artifact> files; std::set<std::wstring> seen; std::vector<std::filesystem::path> nativeArchives;
    const auto& client = vanilla.get("downloads").get("client");
    std::wstring clientPath = L"versions/" + gameVersion + L"/" + gameVersion + L".jar";
    add(files, seen, {clientPath, Utf8ToWide(client.get("url").string()), Utf8ToWide(client.get("sha1").string()), L"sha1"});
    result.classpath.push_back(clientPath);

    const auto& index = vanilla.get("assetIndex"); result.assetIndex = Utf8ToWide(index.get("id").string());
    std::wstring indexUrl = Utf8ToWide(index.get("url").string());
    add(files, seen, {L"assets/indexes/" + result.assetIndex + L".json", indexUrl, Utf8ToWide(index.get("sha1").string()), L"sha1"});
    JsonValue assetJson; if (!json(indexUrl, assetJson, error, 64 * 1024 * 1024)) return false;
    for (const auto& entry : assetJson.get("objects").object()) {
        const auto& object = entry.second;
        std::wstring hash = Utf8ToWide(object.get("hash").string()); if (hash.size() < 2) continue;
        std::wstring relative = L"assets/objects/" + hash.substr(0, 2) + L"/" + hash;
        add(files, seen, {relative, L"https://resources.download.minecraft.net/" + hash.substr(0, 2) + L"/" + hash, hash, L"sha1"});
    }

    for (const auto& lib : vanilla.get("libraries").array()) {
        if (!allowed(lib)) continue;
        const auto& artifact = lib.get("downloads").get("artifact");
        std::wstring path = Utf8ToWide(artifact.get("path").string());
        if (!path.empty()) { std::wstring rel = L"libraries/" + path; add(files, seen, {rel, Utf8ToWide(artifact.get("url").string()), Utf8ToWide(artifact.get("sha1").string()), L"sha1"}); result.classpath.push_back(rel); }
        std::wstring classifier = replaceArch(Utf8ToWide(lib.get("natives").get("windows").string()));
        if (!classifier.empty()) {
            const auto& native = lib.get("downloads").get("classifiers").get(WideToUtf8(classifier));
            std::wstring npath = Utf8ToWide(native.get("path").string());
            if (!npath.empty()) { std::wstring rel = L"libraries/" + npath; add(files, seen, {rel, Utf8ToWide(native.get("url").string()), Utf8ToWide(native.get("sha1").string()), L"sha1"}); nativeArchives.push_back(root / rel); }
        }
    }
    const auto& log = vanilla.get("logging").get("client").get("file");
    if (log.isObject()) { result.loggingConfig = L"assets/log_configs/" + Utf8ToWide(log.get("id").string()); add(files, seen, {result.loggingConfig, Utf8ToWide(log.get("url").string()), Utf8ToWide(log.get("sha1").string()), L"sha1"}); }

    JsonValue loaders; if (!json(L"https://meta.fabricmc.net/v2/versions/loader/" + gameVersion, loaders, error) || loaders.array().empty()) { error = L"No Fabric Loader for " + gameVersion; return false; }
    const JsonValue* chosen = &loaders.array().front();
    for (const auto& item : loaders.array()) if (item.get("loader").get("stable").boolean()) { chosen = &item; break; }
    std::wstring loader = Utf8ToWide(chosen->get("loader").get("version").string());
    JsonValue profile; if (!json(L"https://meta.fabricmc.net/v2/versions/loader/" + gameVersion + L"/" + loader + L"/profile/json", profile, error)) return false;
    result.versionId = Utf8ToWide(profile.get("id").string()); result.mainClass = Utf8ToWide(profile.get("mainClass").string());
    if (result.mainClass.empty()) result.mainClass = L"net.fabricmc.loader.impl.launch.knot.KnotClient";
    for (const auto& lib : profile.get("libraries").array()) {
        std::wstring path = mavenPath(Utf8ToWide(lib.get("name").string())); if (path.empty()) continue;
        std::wstring base = Utf8ToWide(lib.get("url").string("https://maven.fabricmc.net/")); if (base.empty()) base = L"https://maven.fabricmc.net/"; if (base.back() != L'/') base += L'/'; std::wstring url = base + path;
        std::string hashBytes; if (!ManifestService::fetchBytes(url + L".sha1", hashBytes, error, 4096)) return false;
        std::wstring rel = L"libraries/" + path; add(files, seen, {rel, url, trim(Utf8ToWide(hashBytes)), L"sha1"}); result.classpath.insert(result.classpath.begin(), rel);
    }
    if (cancelled) { error = L"Installation cancelled"; return false; }
    ArtifactDownloader downloader; if (!downloader.ensure(files, root, cancelled, std::move(progress), error)) return false;
    result.nativesDir = L"natives/" + gameVersion;
    return extractNatives(nativeArchives, root / result.nativesDir, error);
}

bool GameInstaller::extractNatives(const std::vector<std::filesystem::path>& archives, const std::filesystem::path& destination, std::wstring& error) {
    std::error_code ec; std::filesystem::remove_all(destination, ec); ec.clear(); std::filesystem::create_directories(destination, ec);
    if (ec) { error = L"Cannot create natives directory"; return false; }
    for (const auto& archive : archives) {
        std::wstring cmd = L"tar.exe -xf " + quote(archive.wstring()) + L" -C " + quote(destination.wstring()); std::vector<wchar_t> data(cmd.begin(), cmd.end()); data.push_back(0);
        STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, data.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { error = L"Cannot extract natives"; return false; }
        WaitForSingleObject(pi.hProcess, INFINITE); DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (code) { error = L"Native extraction failed"; return false; }
    }
    return true;
}

std::wstring GameInstaller::findJava() {
    wchar_t env[32768]{}; DWORD n = GetEnvironmentVariableW(L"JAVA_HOME", env, 32768);
    if (n && n < 32768) { auto java = std::filesystem::path(env) / L"bin/javaw.exe"; if (std::filesystem::exists(java)) return java.wstring(); }
    wchar_t path[MAX_PATH]{}; if (SearchPathW(nullptr, L"javaw.exe", nullptr, MAX_PATH, path, nullptr)) return path;
    PWSTR roaming = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming))) {
        auto runtime = std::filesystem::path(roaming) / L".minecraft/runtime"; CoTaskMemFree(roaming); std::error_code ec;
        if (std::filesystem::exists(runtime, ec)) for (auto it = std::filesystem::recursive_directory_iterator(runtime, std::filesystem::directory_options::skip_permission_denied, ec); it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) if (!ec && it->path().filename() == L"javaw.exe") return it->path().wstring();
    }
    return {};
}
