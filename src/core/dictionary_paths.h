#pragma once

// 词典文件（*.jsonc）的查找规则。
//
// 词典可能位于工作目录、工作目录的 translations/、上级目录的 translations/，
// 或当前工作目录（开发时从 build/ 直接启动）的对应位置。
// 原先这段逻辑是 mainwindow.cpp 里的静态函数，注入服务也需要用，故提升为共享头文件。

#include <filesystem>
#include <string>
#include <vector>

#include "core/path_constants.h"

namespace fs = std::filesystem;

// 按优先级在若干候选目录中查找 <filename 去掉扩展名>.jsonc。
// 全部未命中时返回「工作目录/translations/<name>.jsonc」（调用方据此判断文件不存在）。
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
