#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/path_constants.h"

namespace fs = std::filesystem;

inline fs::path ResolveDictionaryPath(const std::wstring& workingDir, const std::wstring& filename) {
    const fs::path workPath(workingDir);
    const std::wstring targetName = fs::path(filename).stem().wstring() + L".jsonc";

    const std::vector<fs::path> baseDirs = {
        workPath / paths::kTranslationsDir,
        workPath,
        workPath / L".." / paths::kTranslationsDir,
        fs::current_path() / paths::kTranslationsDir,
        fs::current_path(),
        fs::current_path() / L".." / paths::kTranslationsDir
    };

    for (const auto& dir : baseDirs) {
        const fs::path p = dir / targetName;
        if (fs::exists(p)) return p;
    }

    return workPath / paths::kTranslationsDir / targetName;
}
