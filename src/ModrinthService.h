#pragma once
#include "Models.h"
#include <string>

class ModrinthService {
public:
    bool resolveLatest(const std::wstring& projectSlug, const std::wstring& gameVersion,
                       Artifact& artifact, std::wstring& error) const;
};
