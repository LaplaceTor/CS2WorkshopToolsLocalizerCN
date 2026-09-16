#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

struct FgdPropertyOverride {
    std::string description;
    std::string displayName;
};

struct FgdOverrideData {
    // 1. 全局属性覆盖 (按属性名查找，如 disableshadows, bodygroups)
    std::unordered_map<std::string, FgdPropertyOverride> globalProperties;

    // 2. I/O 覆盖 (如 SetParent, Kill)
    std::unordered_map<std::string, std::string> ioOverrides;

    // 3. 类说明覆盖 (如 info_node, env_cubemap)
    std::unordered_map<std::string, std::string> classDescriptions;

    // 4. 类作用域特定属性覆盖 class -> (prop -> override)
    std::unordered_map<std::string, std::unordered_map<std::string, FgdPropertyOverride>> classProperties;

    bool empty() const {
        return globalProperties.empty() && ioOverrides.empty() && classDescriptions.empty() && classProperties.empty();
    }
};

class FgdCore {
public:
    using FgdDict = std::unordered_map<std::string, std::string>;

    // 单行翻译（带类作用域与状态机跟踪）
    static std::string TranslateLine(
        const std::string& line,
        const FgdDict& dict,
        const FgdOverrideData& overrideData,
        std::string& inOutCurrentClass,
        std::string& inOutPendingClassDesc
    );

    static std::string TranslateLine(
        const std::string& line,
        const FgdDict& dict,
        const FgdOverrideData& overrideData,
        std::string& inOutCurrentClass
    );

    static std::string TranslateLine(
        const std::string& line,
        const FgdDict& dict
    );

    // 纯内存批量翻译整个 FGD 文本内容
    static bool TranslateContent(
        const std::string& inputContent,
        std::string& outTranslated,
        const FgdDict& dict,
        const FgdOverrideData& overrideData = FgdOverrideData()
    );
};
