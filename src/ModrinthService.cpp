#include "ModrinthService.h"
#include "ManifestService.h"
#include "MiniJson.h"
#include "Utf.h"

namespace {
std::wstring encode(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value); std::wstring out;
    const wchar_t* hex = L"0123456789ABCDEF";
    for (unsigned char c : utf8) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') out += static_cast<wchar_t>(c);
        else { out += L'%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
}

bool ModrinthService::resolveLatest(const std::wstring& slug, const std::wstring& gameVersion,
                                    Artifact& artifact, std::wstring& error) const {
    std::wstring loaders = L"[\"fabric\"]";
    std::wstring games = L"[\"" + gameVersion + L"\"]";
    std::wstring url = L"https://api.modrinth.com/v2/project/" + encode(slug) + L"/version?loaders=" + encode(loaders)
        + L"&game_versions=" + encode(games) + L"&include_changelog=false";
    std::string bytes;
    if (!ManifestService::fetchBytes(url, bytes, error, 4 * 1024 * 1024)) return false;
    JsonValue root; std::string parseError;
    if (!MiniJson::parse(bytes, root, parseError) || !root.isArray() || root.array().empty()) {
        error = L"No compatible Modrinth version for " + slug + L" / " + gameVersion;
        return false;
    }
    const auto& files = root.array().front().get("files").array();
    if (files.empty()) { error = L"Modrinth returned no files for " + slug; return false; }
    const JsonValue* selected = &files.front();
    for (const auto& file : files) if (file.get("primary").boolean(false)) { selected = &file; break; }
    std::wstring filename = Utf8ToWide(selected->get("filename").string());
    std::wstring download = Utf8ToWide(selected->get("url").string());
    std::wstring sha512 = Utf8ToWide(selected->get("hashes").get("sha512").string());
    if (filename.empty() || download.empty() || sha512.empty()) { error = L"Incomplete Modrinth response for " + slug; return false; }
    artifact = {L"mods\\" + filename, download, sha512, L"sha512"};
    return true;
}
