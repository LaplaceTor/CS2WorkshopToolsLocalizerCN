#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
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

    // 逐行翻译时的跨行状态（类作用域 + 多行字符串拼接跟踪）。
    // 只在本类内部解释，调用方在同一文件的各行之间复用一个实例即可。
    class LineState {
    public:
        LineState();
        ~LineState();
        LineState(LineState&&) noexcept;
        LineState& operator=(LineState&&) noexcept;
        LineState(const LineState&) = delete;
        LineState& operator=(const LineState&) = delete;

    private:
        friend class FgdCore;
        struct Impl;
        std::unique_ptr<Impl> m_impl;
    };

    // 单行翻译（带完整跨行状态：类作用域 + 多行字符串拼接）。
    //
    // 【为何只保留这一个入口】
    // 早期提供过若干以 `std::string& inOutCurrentClass` 为状态的便捷重载，
    // 它们无法跨行保存「多行字符串拼接」的跟踪状态，只能在内部用
    // 「以调用方局部变量地址为键的 thread_local 缓存」来模拟，导致：
    //   - 调用方局部变量被栈帧复用时会命中上一次的脏状态；
    //   - 缓存超阈值被 clear() 时会误伤另一路正在解析中的状态。
    // 现在统一要求调用方显式持有一个 LineState 实例并在同一文件各行间复用，
    // 状态归属清晰、无隐式全局可变状态。
    //
    // 注意：逐行流式调用时，多行拼接链只有「按字面量逐段翻译」的效果；
    // 若需要把整条链折叠成一条完整译文，请改用 TranslateContent（它能同时改写整条链的所有行）。
    static std::string TranslateLine(
        const std::string& line,
        const FgdDict& dict,
        const FgdOverrideData& overrideData,
        LineState& state
    );

    // 纯内存批量翻译整个 FGD 文本内容（推荐入口）
    static bool TranslateContent(
        const std::string& inputContent,
        std::string& outTranslated,
        const FgdDict& dict,
        const FgdOverrideData& overrideData = FgdOverrideData()
    );
};
