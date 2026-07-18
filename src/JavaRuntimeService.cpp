#include "JavaRuntimeService.h"
#include "GameInstaller.h"
#include "ManifestService.h"
#include "MiniJson.h"
#include "Utf.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <utility>
#include <vector>

namespace {
std::wstring quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

std::string readText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string javaVersionOutput(const std::filesystem::path& javaw) {
    auto java = javaw.parent_path() / L"java.exe";
    if (!std::filesystem::exists(java)) return {};
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &security, 0)) return {};
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring command = quote(java.wstring()) + L" -version";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    BOOL started = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                  nullptr, nullptr, &startup, &process);
    CloseHandle(writePipe);
    if (!started) {
        CloseHandle(readPipe);
        return {};
    }
    DWORD wait = WaitForSingleObject(process.hProcess, 10000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
    }
    std::string output;
    char buffer[2048];
    DWORD received = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &received, nullptr) && received) output.append(buffer, received);
    CloseHandle(readPipe);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return output;
}

bool extractArchive(const std::filesystem::path& archive, const std::filesystem::path& destination, std::wstring& error) {
    std::wstring command = L"tar.exe -xf " + quote(archive.wstring()) + L" -C " + quote(destination.wstring());
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(0);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        error = L"Unable to start the Java archive extractor";
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        error = L"Unable to extract the Java runtime";
        return false;
    }
    return true;
}
}

bool JavaRuntimeService::isJava21(const std::filesystem::path& executable) {
    if (!std::filesystem::exists(executable)) return false;
    auto release = readText(executable.parent_path().parent_path() / L"release");
    if (release.find("JAVA_VERSION=\"21") != std::string::npos || release.find("JAVA_VERSION=21") != std::string::npos) return true;
    auto output = javaVersionOutput(executable);
    std::transform(output.begin(), output.end(), output.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return output.find("version \"21") != std::string::npos || output.find("openjdk 21") != std::string::npos;
}

bool JavaRuntimeService::ensureBundled(const std::filesystem::path& installRoot,
                                       std::atomic_bool& cancelled, ArtifactDownloader::Progress progress,
                                       std::wstring& executable, std::wstring& error) {
    auto runtime = installRoot / L"runtime";
    auto javaw = runtime / L"bin" / L"javaw.exe";
    if (isJava21(javaw)) {
        executable = javaw.wstring();
        return true;
    }

    if (progress) progress(0, 3, L"Resolving Java 21");
    std::string metadataBytes;
    const std::wstring metadataUrl = L"https://api.adoptium.net/v3/assets/latest/21/hotspot?architecture=x64&image_type=jre&os=windows&vendor=eclipse";
    if (!ManifestService::fetchBytes(metadataUrl, metadataBytes, error, 4 * 1024 * 1024)) return false;
    JsonValue metadata;
    std::string parseError;
    if (!MiniJson::parse(metadataBytes, metadata, parseError) || metadata.array().empty()) {
        error = L"Invalid Java runtime metadata: " + Utf8ToWide(parseError);
        return false;
    }
    const auto& package = metadata.array().front().get("binary").get("package");
    std::wstring url = Utf8ToWide(package.get("link").string());
    std::wstring checksum = Utf8ToWide(package.get("checksum").string());
    std::wstring name = Utf8ToWide(package.get("name").string("java21-runtime.zip"));
    if (url.empty() || checksum.size() != 64) {
        error = L"Java runtime metadata does not contain a verified package";
        return false;
    }
    if (cancelled) {
        error = L"Java runtime download cancelled";
        return false;
    }

    Artifact packageArtifact{L".cache\\java\\" + name, url, checksum, L"sha256"};
    ArtifactDownloader downloader;
    auto downloadProgress = [&](size_t current, size_t, const std::wstring&) {
        if (progress) progress(current == 0 ? 1 : 2, 3, L"Downloading Java 21");
    };
    if (!downloader.ensure(std::vector<Artifact>{packageArtifact}, installRoot, cancelled, downloadProgress, error)) return false;
    auto archive = installRoot / packageArtifact.path;
    auto staging = installRoot / L".java-runtime-staging";
    std::error_code filesystemError;
    std::filesystem::remove_all(staging, filesystemError);
    filesystemError.clear();
    std::filesystem::create_directories(staging, filesystemError);
    if (filesystemError) {
        error = L"Unable to create the Java staging directory";
        return false;
    }
    if (progress) progress(2, 3, L"Installing Java 21");
    if (!extractArchive(archive, staging, error)) {
        std::filesystem::remove_all(staging, filesystemError);
        return false;
    }
    if (cancelled) {
        std::filesystem::remove_all(staging, filesystemError);
        error = L"Java runtime installation cancelled";
        return false;
    }

    std::filesystem::path extractedRoot;
    for (auto iterator = std::filesystem::recursive_directory_iterator(staging, std::filesystem::directory_options::skip_permission_denied, filesystemError);
         iterator != std::filesystem::recursive_directory_iterator(); iterator.increment(filesystemError)) {
        if (filesystemError) {
            filesystemError.clear();
            continue;
        }
        if (iterator->path().filename() == L"javaw.exe" && iterator->path().parent_path().filename() == L"bin") {
            extractedRoot = iterator->path().parent_path().parent_path();
            break;
        }
    }
    if (extractedRoot.empty()) {
        std::filesystem::remove_all(staging, filesystemError);
        error = L"The downloaded Java package is incomplete";
        return false;
    }

    std::filesystem::remove_all(runtime, filesystemError);
    filesystemError.clear();
    std::filesystem::rename(extractedRoot, runtime, filesystemError);
    if (filesystemError) {
        std::filesystem::remove_all(staging, filesystemError);
        error = L"Unable to install Java 21 into the runtime directory";
        return false;
    }
    std::filesystem::remove_all(staging, filesystemError);
    javaw = runtime / L"bin" / L"javaw.exe";
    if (!isJava21(javaw)) {
        error = L"The installed Java runtime failed validation";
        return false;
    }
    if (progress) progress(3, 3, L"Java 21 ready");
    executable = javaw.wstring();
    return true;
}

bool JavaRuntimeService::resolve(const std::filesystem::path& installRoot, const std::wstring& mode,
                                 std::atomic_bool& cancelled, ArtifactDownloader::Progress progress,
                                 std::wstring& executable, std::wstring& error) const {
    auto bundled = installRoot / L"runtime" / L"bin" / L"javaw.exe";
    if (mode == L"bundled") return ensureBundled(installRoot, cancelled, std::move(progress), executable, error);

    std::wstring system = GameInstaller::findJava();
    if (mode == L"system" && isJava21(std::filesystem::path(system))) {
        executable = system;
        return true;
    }
    if (mode == L"auto") {
        if (isJava21(bundled)) {
            executable = bundled.wstring();
            return true;
        }
        if (isJava21(std::filesystem::path(system))) {
            executable = system;
            return true;
        }
    }
    return ensureBundled(installRoot, cancelled, std::move(progress), executable, error);
}
