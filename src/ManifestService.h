#pragma once
#include "Models.h"
#include <cstddef>
#include <string>

class ManifestService {
public:
    bool load(const std::wstring& source, LauncherManifest& manifest, std::wstring& error) const;
    static bool fetchBytes(const std::wstring& source, std::string& data, std::wstring& error,
                           size_t maximumBytes = 8 * 1024 * 1024);
};
