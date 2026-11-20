#pragma once

#include <string>
#include <vector>

namespace dict_updater {

struct UpdateResult {
    bool success{false};
    std::string errorMessage;
};

std::string EscapeJsonString(const std::string& input);
std::string UnescapeJsonString(const std::string& input);

UpdateResult UpdateEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key,
    const std::string& newValue
);

UpdateResult AddEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key,
    const std::string& value
);

UpdateResult DeleteEntry(
    const std::wstring& filePath,
    const std::string& module,
    const std::string& key
);

}
