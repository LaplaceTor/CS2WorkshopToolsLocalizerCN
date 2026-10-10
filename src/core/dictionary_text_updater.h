#pragma once

#include <string>
#include <vector>

namespace dict_updater {

struct UpdateResult {
    bool success{false};
    std::string errorMessage;
};

// JSON 字符串转义工具
std::string EscapeJsonString(const std::string& input);
std::string UnescapeJsonString(const std::string& input);

// 更新已有条目
// filePath: .jsonc 文件绝对路径
// module: 模块作用域（如 "hammer", "common"；若为空则为全局顶层，如 FGD 或 Fallback）
// key: 英文原文键
// newValue: 新中文译文
UpdateResult UpdateEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key,
    const std::string& newValue
);

// 新增条目
// 若 module 不为空且主词典尚不存在该模块作用域块，将自动创建 "module": { ... }
UpdateResult AddEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key,
    const std::string& value
);

// 物理删除条目
// 仅删除键值对所在行，保留紧贴的上行注释，并妥善维护逗号闭合完整性
UpdateResult DeleteEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key
);

} // namespace dict_updater
