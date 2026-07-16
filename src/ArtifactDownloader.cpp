#include "ArtifactDownloader.h"
#include "ManifestService.h"
#include <windows.h>
#include <bcrypt.h>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
bool safeRelative(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& part : path) if (part == L"..") return false;
    return true;
}
std::wstring lower(std::wstring value) {
    for (auto& c : value) c = static_cast<wchar_t>(towlower(c));
    return value;
}
bool validHash(const std::wstring& hash, const std::wstring& algorithm) {
    size_t expected = algorithm == L"sha1" ? 40 : algorithm == L"sha512" ? 128 : 64;
    if (hash.size() != expected) return false;
    for (wchar_t c : hash) if (!iswxdigit(c)) return false;
    return true;
}
LPCWSTR algorithmName(const std::wstring& algorithm) {
    if (algorithm == L"sha1") return BCRYPT_SHA1_ALGORITHM;
    if (algorithm == L"sha512") return BCRYPT_SHA512_ALGORITHM;
    return BCRYPT_SHA256_ALGORITHM;
}
}

std::wstring ArtifactDownloader::fileHash(const std::filesystem::path& file, const std::wstring& algorithm) {
    std::ifstream input(file, std::ios::binary);
    if (!input) return {};
    BCRYPT_ALG_HANDLE provider = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0, hashSize = 0, received = 0;
    if (BCryptOpenAlgorithmProvider(&provider, algorithmName(algorithm), nullptr, 0) < 0) return {};
    BCryptGetProperty(provider, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &received, 0);
    BCryptGetProperty(provider, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &received, 0);
    std::vector<UCHAR> object(objectSize), digest(hashSize);
    if (BCryptCreateHash(provider, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(provider, 0); return {};
    }
    std::vector<char> buffer(1 << 20);
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        auto count = input.gcount();
        if (count > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0);
    }
    BCryptFinishHash(hash, digest.data(), hashSize, 0);
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(provider, 0);
    std::wostringstream result; result << std::hex << std::setfill(L'0');
    for (auto byte : digest) result << std::setw(2) << static_cast<unsigned>(byte);
    return result.str();
}

bool ArtifactDownloader::ensure(const std::vector<Artifact>& artifacts, const std::filesystem::path& root,
                                std::atomic_bool& cancelled, Progress progress, std::wstring& error) const {
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) { error = L"Cannot create install directory"; return false; }

    for (size_t i = 0; i < artifacts.size(); ++i) {
        if (cancelled) { error = L"Download cancelled"; return false; }
        const auto& artifact = artifacts[i];
        std::wstring algorithm = lower(artifact.hashAlgorithm.empty() ? L"sha256" : artifact.hashAlgorithm);
        if (!validHash(artifact.hash, algorithm)) { error = L"Missing valid " + algorithm + L" for: " + artifact.path; return false; }
        std::filesystem::path relative(artifact.path);
        if (!safeRelative(relative)) { error = L"Unsafe artifact path: " + artifact.path; return false; }
        auto destination = root / relative;
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) { error = L"Cannot create artifact directory"; return false; }
        if (progress) progress(i, artifacts.size(), artifact.path);

        if (std::filesystem::exists(destination) && lower(fileHash(destination, algorithm)) == lower(artifact.hash)) continue;
        std::string bytes;
        std::wstring lastError;
        bool downloaded = false;
        for (int attempt = 0; attempt < 3 && !cancelled; ++attempt) {
            if (ManifestService::fetchBytes(artifact.url, bytes, lastError, 512ull * 1024 * 1024)) { downloaded = true; break; }
            if (attempt < 2) Sleep(static_cast<DWORD>(350 * (attempt + 1)));
        }
        if (cancelled) { error = L"Download cancelled"; return false; }
        if (!downloaded) { error = lastError.empty() ? L"Download failed: " + artifact.path : lastError; return false; }
        auto temporary = destination; temporary += L".download";
        std::filesystem::remove(temporary, ec); ec.clear();
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) { error = L"Cannot write " + destination.wstring(); return false; }
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output) { error = L"Write failed: " + destination.wstring(); return false; }
        }
        if (lower(fileHash(temporary, algorithm)) != lower(artifact.hash)) {
            std::filesystem::remove(temporary, ec);
            error = algorithm + L" mismatch: " + artifact.path;
            return false;
        }
        std::filesystem::remove(destination, ec); ec.clear();
        std::filesystem::rename(temporary, destination, ec);
        if (ec) { std::filesystem::remove(temporary, ec); error = L"Cannot commit " + destination.wstring(); return false; }
    }
    if (progress) progress(artifacts.size(), artifacts.size(), L"Ready");
    return true;
}
