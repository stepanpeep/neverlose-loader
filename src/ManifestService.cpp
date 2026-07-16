#include "ManifestService.h"
#include "MiniJson.h"
#include "Utf.h"
#include <windows.h>
#include <winhttp.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
bool isWeb(const std::wstring& s) { return s.rfind(L"https://",0)==0 || s.rfind(L"http://",0)==0; }
std::filesystem::path appDir() { wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr,path,MAX_PATH); return std::filesystem::path(path).parent_path(); }
Artifact artifact(const JsonValue& j) {
    std::wstring hash = Utf8ToWide(j.get("sha256").string());
    std::wstring algorithm = L"sha256";
    if (hash.empty()) { hash = Utf8ToWide(j.get("sha512").string()); algorithm = L"sha512"; }
    if (hash.empty()) { hash = Utf8ToWide(j.get("sha1").string()); algorithm = L"sha1"; }
    return {Utf8ToWide(j.get("path").string()), Utf8ToWide(j.get("url").string()), hash, algorithm};
}
std::vector<Artifact> artifacts(const JsonValue& list){ std::vector<Artifact> out; for(const auto& j:list.array()) out.push_back(artifact(j)); return out; }
}

bool ManifestService::fetchBytes(const std::wstring& source, std::string& data, std::wstring& error, size_t maximumBytes) {
    data.clear();
    if (!isWeb(source)) {
        auto path=std::filesystem::path(source);
        if(path.is_relative()) path=appDir()/path;
        std::ifstream in(path,std::ios::binary);
        if(!in){error=L"Cannot open manifest: "+path.wstring();return false;}
        if (maximumBytes) {
            in.seekg(0, std::ios::end); auto length = in.tellg(); in.seekg(0, std::ios::beg);
            if (length < 0 || static_cast<unsigned long long>(length) > maximumBytes) { error=L"Local response is too large"; return false; }
        }
        std::ostringstream ss; ss<<in.rdbuf(); data=ss.str(); return true;
    }
    URL_COMPONENTS uc{sizeof(uc)}; wchar_t host[256]{}, urlPath[2048]{}, extra[2048]{};
    uc.lpszHostName=host; uc.dwHostNameLength=256; uc.lpszUrlPath=urlPath; uc.dwUrlPathLength=2048; uc.lpszExtraInfo=extra; uc.dwExtraInfoLength=2048;
    if(!WinHttpCrackUrl(source.c_str(),0,0,&uc)){error=L"Invalid manifest URL";return false;}
    HINTERNET session=WinHttpOpen(L"NeverloseLoader/1.0",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,nullptr,nullptr,0);
    if(!session){error=L"WinHTTP initialization failed";return false;}
    WinHttpSetTimeouts(session,8000,8000,15000,30000);
    HINTERNET connect=WinHttpConnect(session,std::wstring(host,uc.dwHostNameLength).c_str(),uc.nPort,0);
    DWORD flags=uc.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0;
    std::wstring requestPath(urlPath,uc.dwUrlPathLength); requestPath.append(extra,uc.dwExtraInfoLength);
    HINTERNET request=connect?WinHttpOpenRequest(connect,L"GET",requestPath.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags):nullptr;
    bool ok=request && WinHttpSendRequest(request,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0) && WinHttpReceiveResponse(request,nullptr);
    DWORD status=0,size=sizeof(status); if(ok) WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,nullptr,&status,&size,nullptr);
    if(!ok || status<200 || status>=300){error=L"Request failed (HTTP "+std::to_wstring(status)+L")";ok=false;}
    while(ok){DWORD available=0;if(!WinHttpQueryDataAvailable(request,&available)){ok=false;break;}if(!available)break;size_t old=data.size();data.resize(old+available);DWORD read=0;if(!WinHttpReadData(request,data.data()+old,available,&read)){ok=false;break;}data.resize(old+read);if(maximumBytes && data.size()>maximumBytes){error=L"HTTP response is too large";ok=false;break;}}
    if(request)WinHttpCloseHandle(request);if(connect)WinHttpCloseHandle(connect);WinHttpCloseHandle(session);
    if(!ok && error.empty()) error=L"Unable to download manifest";
    return ok;
}

bool ManifestService::load(const std::wstring& source, LauncherManifest& m, std::wstring& error) const {
    std::string bytes; if(!fetchBytes(source,bytes,error,8 * 1024 * 1024)) return false;
    JsonValue root; std::string parseError; if(!MiniJson::parse(bytes,root,parseError)||!root.isObject()){error=L"Manifest JSON error: "+Utf8ToWide(parseError);return false;}
    LauncherManifest next;
    const auto& launcher=root.get("launcher"); next.minimumVersion=Utf8ToWide(launcher.get("minimumVersion").string()); next.title=Utf8ToWide(launcher.get("branding").get("title").string("Neverlose Loader"));
    next.maintenance=root.get("maintenance").get("enabled").boolean(); next.maintenanceMessage=Utf8ToWide(root.get("maintenance").get("message").string());
    for(const auto& j:root.get("modules").array()){ModuleEntry x; x.id=Utf8ToWide(j.get("id").string());x.name=Utf8ToWide(j.get("name").string());x.description=Utf8ToWide(j.get("description").string());x.modrinthProject=Utf8ToWide(j.get("modrinthProject").string());x.artifacts=artifacts(j.get("artifacts"));if(!x.id.empty())next.modules.push_back(std::move(x));}
    for(const auto& j:root.get("presets").array()){PresetEntry x;x.id=Utf8ToWide(j.get("id").string());x.name=Utf8ToWide(j.get("name").string());for(const auto& v:j.get("modules").array())x.modules.push_back(Utf8ToWide(v.string()));if(!x.id.empty())next.presets.push_back(std::move(x));}
    for(const auto& j:root.get("versions").array()){VersionEntry x;x.id=Utf8ToWide(j.get("id").string());x.name=Utf8ToWide(j.get("name").string());x.minecraftVersion=Utf8ToWide(j.get("minecraftVersion").string());x.loader=Utf8ToWide(j.get("loader").string());x.inheritsPreset=Utf8ToWide(j.get("inheritsPreset").string());x.mainClass=Utf8ToWide(j.get("mainClass").string());for(const auto& v:j.get("arguments").array())x.arguments.push_back(Utf8ToWide(v.string()));x.artifacts=artifacts(j.get("artifacts"));if(!x.id.empty())next.versions.push_back(std::move(x));}
    if(next.versions.empty()){error=L"Manifest contains no versions";return false;} m=std::move(next); return true;
}
