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
    std::unordered_map<std::string, FgdPropertyOverride> globalProperties;

    std::unordered_map<std::string, std::string> ioOverrides;

    std::unordered_map<std::string, std::string> classDescriptions;

    std::unordered_map<std::string, std::unordered_map<std::string, FgdPropertyOverride>> classProperties;

    bool empty() const {
        return globalProperties.empty() && ioOverrides.empty() && classDescriptions.empty() && classProperties.empty();
    }
};

class FgdCore {
public:
    using FgdDict = std::unordered_map<std::string, std::string>;

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

    static std::string TranslateLine(
        const std::string& line,
        const FgdDict& dict,
        const FgdOverrideData& overrideData,
        LineState& state
    );

    static bool TranslateContent(
        const std::string& inputContent,
        std::string& outTranslated,
        const FgdDict& dict,
        const FgdOverrideData& overrideData = FgdOverrideData()
    );
};
