#include "core/qm/translation_engine.h"
#include "core/dictionary_compiler.h"
#include "core/encoding_util.h"
#include "core/pe_patcher.h"
#include "core/shared_log.h"
#include "core/untranslated_capture.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace qm {

namespace {

// ==============================================================================
// 1. 全局配置与状态
// ==============================================================================
std::atomic<bool> g_bTranslationEnabled{true};

std::unordered_map<std::string, std::string> g_CommonDict;
std::unordered_map<std::string, std::string> g_CommonCache;
std::unordered_map<std::string, std::string> g_CommonReverseDict;
std::unordered_map<std::string, std::string> g_CommonReverseCache;

std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedDicts;
std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedCaches;
std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedReverseDicts;
std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> g_ScopedReverseCaches;

std::mutex g_DictMutex;
std::once_flag g_dictInitFlag;
std::atomic<bool> g_bDictLoaded{false};

capture::UntranslatedCaptureWriter g_UntranslatedCapture;

// ==============================================================================
// 2. 模块快照双缓冲实现
// ==============================================================================
struct MasterModuleItem {
    uintptr_t base;
    uintptr_t end;
    std::wstring stem;
};

ModuleRangeSnapshot g_SnapshotBuffers[2];
std::atomic<size_t> g_ActiveSnapshotIndex{0};
std::atomic<const ModuleRangeSnapshot*> g_pActiveSnapshot{nullptr};

std::mutex g_ModuleWriterMutex; // 仅写者加锁，读者 100% 纯无锁
std::vector<MasterModuleItem> g_MasterModules;

std::wstring ExtractStem(const std::wstring& path) {
    if (path.empty()) return L"";
    size_t lastSlash = path.find_last_of(L"\\/");
    std::wstring filename = (lastSlash == std::wstring::npos) ? path : path.substr(lastSlash + 1);
    size_t lastDot = filename.find_last_of(L'.');
    std::wstring stem = (lastDot == std::wstring::npos) ? filename : filename.substr(0, lastDot);
    std::transform(stem.begin(), stem.end(), stem.begin(), ::towlower);
    return stem;
}

void PublishNewSnapshotLocked() {
    size_t nextIdx = 1 - g_ActiveSnapshotIndex.load(std::memory_order_relaxed);
    ModuleRangeSnapshot& nextBuf = g_SnapshotBuffers[nextIdx];

    size_t count = (std::min)(g_MasterModules.size(), MAX_CACHED_MODULE_RANGES);
    nextBuf.count = count;
    for (size_t i = 0; i < count; ++i) {
        nextBuf.entries[i].base = g_MasterModules[i].base;
        nextBuf.entries[i].end = g_MasterModules[i].end;
        wcsncpy_s(nextBuf.entries[i].stem, 32, g_MasterModules[i].stem.c_str(), _TRUNCATE);
    }

    g_pActiveSnapshot.store(&nextBuf, std::memory_order_release);
    g_ActiveSnapshotIndex.store(nextIdx, std::memory_order_release);
}

// ==============================================================================
// 3. 递归智能拆分辅助函数
// ==============================================================================
inline bool TryDirectMatch(const std::unordered_map<std::string, std::string>& dict, 
                           const std::string& text, std::string& outResult) {
    if (text.empty()) return false;
    auto it = dict.find(text);
    if (it != dict.end()) {
        outResult = it->second;
        return true;
    }
    return false;
}

std::string ToTitleCase(const std::string& str) {
    std::string res = str;
    bool newWord = true;
    for (size_t i = 0; i < res.length(); ++i) {
        unsigned char c = (unsigned char)res[i];
        if (c == ' ' || c == '\t' || c == '-' || c == '_') {
            newWord = true;
        } else if (newWord) {
            if (c >= 'a' && c <= 'z') {
                res[i] = (char)(c - 'a' + 'A');
            }
            newWord = false;
        }
    }
    return res;
}

std::string ToSentenceCase(const std::string& str) {
    std::string res = str;
    if (!res.empty() && res[0] >= 'a' && res[0] <= 'z') {
        res[0] = (char)(res[0] - 'a' + 'A');
    }
    return res;
}

bool MatchAndTranslateInternal(const std::unordered_map<std::string, std::string>& dict, 
                               const std::string& text, std::string& outResult, int depth = 0) {
    if (text.empty() || depth > 4) return false;

    // 1. 直接全匹配
    if (TryDirectMatch(dict, text, outResult)) return true;

    // 1.1 尝试大小写归一化匹配
    std::string titleCase = ToTitleCase(text);
    if (titleCase != text) {
        if (TryDirectMatch(dict, titleCase, outResult)) return true;
    }
    std::string sentenceCase = ToSentenceCase(text);
    if (sentenceCase != text && sentenceCase != titleCase) {
        if (TryDirectMatch(dict, sentenceCase, outResult)) return true;
    }

    // 2. 去除快捷键加速符 '&'
    if (text.find('&') != std::string::npos) {
        std::string stripped;
        stripped.reserve(text.length());
        for (char c : text) {
            if (c != '&') stripped.push_back(c);
        }
        if (MatchAndTranslateInternal(dict, stripped, outResult, depth + 1)) return true;
    }

    // 3. 去除前后空白字符（保留原始前后空格填充）
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first != std::string::npos) {
        size_t last = text.find_last_not_of(" \t\r\n");
        if (first > 0 || last < text.length() - 1) {
            std::string sub = text.substr(first, last - first + 1);
            std::string subTrans;
            if (MatchAndTranslateInternal(dict, sub, subTrans, depth + 1)) {
                outResult = text.substr(0, first) + subTrans + text.substr(last + 1);
                return true;
            }
        }
    }

    // 4. 智能匹配动态数字前缀
    if (isdigit((unsigned char)text[0]) || (text[0] == '+' && text.length() > 1 && isdigit((unsigned char)text[1]))) {
        size_t i = 0;
        while (i < text.length() && (isdigit((unsigned char)text[i]) || text[i] == '/' || text[i] == '.' || 
                                     text[i] == '+' || text[i] == '-' || text[i] == '%' || text[i] == ':')) {
            i++;
        }
        if (i > 0 && i < text.length() && (text[i] == ' ' || text[i] == '\t')) {
            std::string numPrefix = text.substr(0, i);
            size_t remainderStart = text.find_first_not_of(" \t", i);
            if (remainderStart != std::string::npos) {
                std::string remainder = text.substr(remainderStart);
                std::string remainderTrans;

                if (MatchAndTranslateInternal(dict, remainder, remainderTrans, depth + 1)) {
                    outResult = numPrefix + " " + remainderTrans;
                    return true;
                }

                std::string genericPattern = "%1 " + remainder;
                if (TryDirectMatch(dict, genericPattern, remainderTrans)) {
                    size_t pos = remainderTrans.find("%1");
                    if (pos != std::string::npos) {
                        outResult = remainderTrans.substr(0, pos) + numPrefix + remainderTrans.substr(pos + 2);
                        return true;
                    }
                }
            }
        }
    }

    // 5. 智能匹配由 " + " 或 " - " 连接的复合词
    size_t plusPos = text.find(" + ");
    if (plusPos != std::string::npos && plusPos > 0) {
        std::string partA = text.substr(0, plusPos);
        std::string partB = text.substr(plusPos + 3);
        std::string transA, transB;
        bool okA = MatchAndTranslateInternal(dict, partA, transA, depth + 1);
        bool okB = MatchAndTranslateInternal(dict, partB, transB, depth + 1);
        if (okA || okB) {
            outResult = (okA ? transA : partA) + " + " + (okB ? transB : partB);
            return true;
        }
    }

    // 6. 智能匹配中括号快捷键后缀：例如 "Clipping Tool [Shift+X]" -> "剪切工具 [Shift+X]"
    if (text.back() == ']') {
        size_t openBracket = text.rfind('[');
        if (openBracket != std::string::npos && openBracket > 0) {
            std::string base = text.substr(0, openBracket);
            size_t baseLast = base.find_last_not_of(" \t");
            if (baseLast != std::string::npos) {
                base = base.substr(0, baseLast + 1);
                std::string baseTrans;
                if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                    outResult = baseTrans + " " + text.substr(openBracket);
                    return true;
                }
            }
        }
    }

    // 7. 智能匹配圆括号快捷键后缀：例如 "Undo (Ctrl+Z)" -> "撤销 (Ctrl+Z)"
    if (text.back() == ')') {
        size_t openParen = text.rfind('(');
        if (openParen != std::string::npos && openParen > 0) {
            std::string base = text.substr(0, openParen);
            size_t baseLast = base.find_last_not_of(" \t");
            if (baseLast != std::string::npos) {
                base = base.substr(0, baseLast + 1);
                std::string baseTrans;
                if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                    outResult = baseTrans + " " + text.substr(openParen);
                    return true;
                }
            }
        }
    }

    // 8. 智能匹配制表符快捷键后缀：例如 "Save\tCtrl+S" -> "保存\tCtrl+S"
    size_t tabPos = text.find('\t');
    if (tabPos != std::string::npos && tabPos > 0) {
        std::string base = text.substr(0, tabPos);
        std::string baseTrans;
        if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
            outResult = baseTrans + text.substr(tabPos);
            return true;
        }
    }

    // 9. 智能匹配省略号后缀：例如 "Save As..." -> "另存为..."
    if (text.length() > 3 && text.substr(text.length() - 3) == "...") {
        std::string base = text.substr(0, text.length() - 3);
        size_t baseLast = base.find_last_not_of(" \t");
        if (baseLast != std::string::npos) {
            base = base.substr(0, baseLast + 1);
            std::string baseTrans;
            if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                outResult = baseTrans + "...";
                return true;
            }
        }
    } else if (text.length() >= 3 && text.substr(text.length() - 3) == "\xe2\x80\xa6") { // UTF-8 "…"
        std::string base = text.substr(0, text.length() - 3);
        size_t baseLast = base.find_last_not_of(" \t");
        if (baseLast != std::string::npos) {
            base = base.substr(0, baseLast + 1);
            std::string baseTrans;
            if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
                outResult = baseTrans + "\xe2\x80\xa6";
                return true;
            }
        }
    }

    // 10. 智能匹配带冒号的前缀与复合结构
    size_t colonPos = text.find(':');
    if (colonPos != std::string::npos && colonPos > 0) {
        bool isScope = (colonPos + 1 < text.length() && text[colonPos + 1] == ':') || (colonPos > 0 && text[colonPos - 1] == ':');
        bool isUrl = (colonPos + 2 < text.length() && text[colonPos + 1] == '/' && text[colonPos + 2] == '/');
        bool isDriveLetter = (colonPos == 1 && isalpha((unsigned char)text[0]) && (colonPos + 1 < text.length() && (text[colonPos + 1] == '\\' || text[colonPos + 1] == '/')));

        if (!isScope && !isUrl && !isDriveLetter) {
            std::string prefix = text.substr(0, colonPos);
            size_t prefixLast = prefix.find_last_not_of(" \t");
            if (prefixLast != std::string::npos) {
                prefix = prefix.substr(0, prefixLast + 1);
                std::string remainder = text.substr(colonPos + 1);
                size_t remainderFirst = remainder.find_first_not_of(" \t");
                std::string spacePad = " ";
                if (remainderFirst != std::string::npos) {
                    if (remainderFirst > 0) {
                        spacePad = remainder.substr(0, remainderFirst);
                    }
                    remainder = remainder.substr(remainderFirst);
                } else {
                    remainder = "";
                }

                if (!remainder.empty()) {
                    std::string patternS = prefix + ": %s";
                    std::string pattern1 = prefix + ": %1";
                    std::string templateTrans;
                    std::string remainderTrans;
                    bool okRemainder = MatchAndTranslateInternal(dict, remainder, remainderTrans, depth + 1);
                    const std::string& finalRemainder = okRemainder ? remainderTrans : remainder;

                    if (TryDirectMatch(dict, patternS, templateTrans)) {
                        size_t pos = templateTrans.find("%s");
                        if (pos != std::string::npos) {
                            outResult = templateTrans.substr(0, pos) + finalRemainder + templateTrans.substr(pos + 2);
                            return true;
                        }
                    } else if (TryDirectMatch(dict, pattern1, templateTrans)) {
                        size_t pos = templateTrans.find("%1");
                        if (pos != std::string::npos) {
                            outResult = templateTrans.substr(0, pos) + finalRemainder + templateTrans.substr(pos + 2);
                            return true;
                        }
                    }

                    std::string prefixTrans;
                    if (MatchAndTranslateInternal(dict, prefix, prefixTrans, depth + 1)) {
                        outResult = prefixTrans + ":" + spacePad + finalRemainder;
                        return true;
                    }
                } else {
                    std::string prefixTrans;
                    if (MatchAndTranslateInternal(dict, prefix, prefixTrans, depth + 1)) {
                        outResult = prefixTrans + ":";
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

// 判断模块是否为 Qt / UI 基础框架透传层（回溯调用栈时应跳过，以定位上层业务工具模块）
inline bool IsQtFrameworkModule(const wchar_t* stem) {
    if (!stem || stem[0] == L'\0') return false;
    return (_wcsicmp(stem, L"qt5widgets") == 0 ||
            _wcsicmp(stem, L"qt5gui") == 0 ||
            _wcsicmp(stem, L"qt5core") == 0 ||
            _wcsicmp(stem, L"qt5concurrent") == 0 ||
            _wcsicmp(stem, L"qtadvanceddocking-qt5") == 0 ||
            _wcsicmp(stem, L"qtcore_qm") == 0);
}

// 计算反向还原候选英文词的 UI 规范度权重（分值越高越优先作为中文 -> 英文反向还原结果）
int ComputeReverseCandidateScore(const std::string& en) {
    if (en.empty()) return -100;

    int score = 0;
    unsigned char firstChar = static_cast<unsigned char>(en[0]);
    bool startsUpper = (firstChar >= 'A' && firstChar <= 'Z');
    bool startsLower = (firstChar >= 'a' && firstChar <= 'z');

    bool hasLower = false;
    bool hasUpper = false;
    bool hasUnderscore = false;
    bool hasAmpersand = false;

    for (char ch : en) {
        unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 'a' && c <= 'z') hasLower = true;
        else if (c >= 'A' && c <= 'Z') hasUpper = true;
        else if (c == '_') hasUnderscore = true;
        else if (c == '&') hasAmpersand = true;
    }

    if (startsUpper && hasLower) {
        // 标准 TitleCase / SentenceCase（如 "Tool", "Selection Tool"）—— 标准 UI 首选形态
        score += 30;
    } else if (startsUpper && !hasLower) {
        // 全大写专有名词或缩写（如 "OK", "FOV", "UV"）—— 次优 UI 形态
        score += 20;
    } else if (!startsLower && hasUpper) {
        // 非字母开头但包含大写字母（如 "[Shift+X] Tool"）
        score += 10;
    } else if (startsLower && hasUpper) {
        // camelCase 内部标识符（如 "toolName"）
        score += 5;
    } else {
        // 全小写词（如 "tool", "type"）—— 多为底层 Schema 属性名，反向还原优先级最低
        score += 0;
    }

    if (hasUnderscore) score -= 15; // snake_case 标识符（如 "base_item"）
    if (hasAmpersand)  score -= 5;  // 含 '&' 加速符的变体优先让位给纯净词条

    return score;
}

// 按 UI 规范度权重插入反向字典，消除多对一映射下的全小写覆盖退化与哈希遍历顺序不确定性
void InsertReverseEntryWithPriority(std::unordered_map<std::string, std::string>& revDict,
                                    const std::string& en,
                                    const std::string& cn) {
    if (en.empty() || cn.empty()) return;
    auto it = revDict.find(cn);
    if (it == revDict.end()) {
        revDict.emplace(cn, en);
        return;
    }

    int oldScore = ComputeReverseCandidateScore(it->second);
    int newScore = ComputeReverseCandidateScore(en);
    if (newScore > oldScore) {
        it->second = en;
    } else if (newScore == oldScore && en < it->second) {
        // 分值相同时按字典序稳定裁决，保证跨平台/跨桶序 100% 确定性
        it->second = en;
    }
}

} // namespace

std::wstring GetBinDirectory() {
    wchar_t szPath[MAX_PATH] = {0};
    HMODULE hMod = GetModuleHandleW(L"Qt5Core.dll");
    if (!hMod) hMod = GetModuleHandleW(NULL);
    GetModuleFileNameW(hMod, szPath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(szPath, L'\\');
    if (lastSlash) *(lastSlash + 1) = L'\0';
    return std::wstring(szPath);
}

bool GetCallerModuleName(void* callerAddr, wchar_t* outBuf, size_t maxLen) {
    if (!callerAddr || !outBuf || maxLen == 0) return false;
    outBuf[0] = L'\0';

    const ModuleRangeSnapshot* snapshot = g_pActiveSnapshot.load(std::memory_order_acquire);
    if (!snapshot) return false;

    uintptr_t addr = reinterpret_cast<uintptr_t>(callerAddr);
    size_t count = snapshot->count;
    if (count > MAX_CACHED_MODULE_RANGES) count = MAX_CACHED_MODULE_RANGES;

    for (size_t i = 0; i < count; ++i) {
        if (addr >= snapshot->entries[i].base && addr < snapshot->entries[i].end) {
            wcsncpy_s(outBuf, maxLen, snapshot->entries[i].stem, _TRUNCATE);
            return (outBuf[0] != L'\0');
        }
    }

    return false;
}

bool ResolveCallerModuleName(void* directCallerAddr, wchar_t* outBuf, size_t maxLen) {
    if (!outBuf || maxLen == 0) return false;
    outBuf[0] = L'\0';

    // 1. 极速路径 (Fast Path)：直接检查一层返回地址，若已命中非 Qt 框架层模块则直接返回（0 栈回溯开销）
    bool hasDirect = (directCallerAddr != nullptr && GetCallerModuleName(directCallerAddr, outBuf, maxLen));
    if (hasDirect && !IsQtFrameworkModule(outBuf)) {
        return true;
    }

    // 2. 栈回溯路径 (Stack Walk)：当直接调用者位于 Qt5Widgets/Qt5Gui 等框架内部时，向上回溯定位真实业务工具模块
    void* stack[10] = {0};
    WORD frames = RtlCaptureStackBackTrace(1, 10, stack, nullptr);
    wchar_t frameStem[64] = {0};
    for (WORD i = 0; i < frames; ++i) {
        if (GetCallerModuleName(stack[i], frameStem, 64)) {
            if (!IsQtFrameworkModule(frameStem)) {
                wcsncpy_s(outBuf, maxLen, frameStem, _TRUNCATE);
                return (outBuf[0] != L'\0');
            }
        }
    }

    return hasDirect;
}

bool AddModuleRange(HMODULE hMod, const std::wstring& overrideStem) {
    if (!hMod) return false;
    uint32_t sizeOfImage = PePatcher::GetModuleSizeOfImage(hMod);
    if (sizeOfImage == 0) return false;

    uintptr_t base = reinterpret_cast<uintptr_t>(hMod);
    uintptr_t end = base + sizeOfImage;

    std::wstring stem = overrideStem;
    if (stem.empty()) {
        wchar_t szPath[MAX_PATH] = {0};
        if (GetModuleFileNameW(hMod, szPath, MAX_PATH)) {
            stem = ExtractStem(szPath);
        }
    }
    if (stem.empty()) return false;

    std::lock_guard<std::mutex> lock(g_ModuleWriterMutex);
    for (const auto& item : g_MasterModules) {
        if (item.base == base) {
            return true; // 已存在
        }
    }

    if (g_MasterModules.size() >= MAX_CACHED_MODULE_RANGES) {
        return false;
    }

    g_MasterModules.push_back({ base, end, stem });
    PublishNewSnapshotLocked();
    return true;
}

bool RemoveModuleRangeByBase(uintptr_t base) {
    std::lock_guard<std::mutex> lock(g_ModuleWriterMutex);
    bool found = false;
    for (auto it = g_MasterModules.begin(); it != g_MasterModules.end(); ++it) {
        if (it->base == base) {
            g_MasterModules.erase(it);
            found = true;
            break;
        }
    }
    if (!found) return false;

    PublishNewSnapshotLocked();
    return true;
}

void InvalidateUnloadedModules() {
    std::lock_guard<std::mutex> lock(g_ModuleWriterMutex);
    bool changed = false;
    for (auto it = g_MasterModules.begin(); it != g_MasterModules.end();) {
        HMODULE hCheck = reinterpret_cast<HMODULE>(it->base);
        HMODULE hVerified = NULL;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)hCheck, &hVerified) && hVerified == hCheck) {
            ++it;
        } else {
            it = g_MasterModules.erase(it);
            changed = true;
        }
    }

    if (changed || g_pActiveSnapshot.load(std::memory_order_relaxed) == nullptr) {
        PublishNewSnapshotLocked();
    }
}

void ScanKnownToolModules() {
    InvalidateUnloadedModules();

    static const wchar_t* const s_KnownModules[] = {
        L"hammer.dll",
        L"modeldoc_editor.dll",
        L"pet.dll",
        L"met.dll",
        L"sfm.dll",
        L"postprocessing.dll",
        L"postprocessingeditor.dll",
        L"smartprops_editor.dll",
        L"smartprops.dll",
        L"pulse_editor.dll",
        L"pulse_system.dll",
        L"subtool_modeldoc.dll",
        L"subtool_worldeditor.dll",
        L"subtool_particle.dll",
        L"subrecteditor.dll",
        L"cs2_item_editor.dll",
        L"cs2_workshop_manager.dll",
        L"worldeditor.dll",
        L"assetbrowser.dll",
        L"assetpreview.dll",
        L"assetrename.dll",
        L"assetsystem.dll",
        L"propertyeditor.dll",
        L"toolframework2.dll",
        L"toolscenenodes.dll",
        L"helpsystem.dll",
        L"modeldoc_utils.dll",
        L"resourcecompiler.dll",
        L"vconsole2.dll",
        L"particles.dll",
        L"worldrenderer.dll",
        L"soundsystem.dll",
        L"schemasystem.dll",
        L"vscript.dll",
        L"engine2.dll",
        L"client.dll",
        L"server.dll",
        L"rendersystemvulkan.dll",
        L"rendersystemdx11.dll",
        L"scenesystem.dll",
        L"tier0.dll",
        L"qtadvanceddocking-qt5.dll",
        L"qt5concurrent.dll",
        L"qt5widgets.dll",
        L"qt5gui.dll",
        L"qt5core.dll"
    };

    for (const wchar_t* modName : s_KnownModules) {
        HMODULE hMod = GetModuleHandleW(modName);
        if (hMod) {
            AddModuleRange(hMod);
        }
    }
}

void LoadMasterTranslations() {
    std::wstring binDir = GetBinDirectory();
    std::wstring jsoncPath = L"";
    std::wstring fallbackPath = L"";
    bool useMachineTrans = true;

    std::wstring appDirPointerPath = binDir + L"localizer_appdir.txt";
    FILE* fpPointer = _wfopen(appDirPointerPath.c_str(), L"r, ccs=UTF-8");
    if (!fpPointer) {
        fpPointer = _wfopen(appDirPointerPath.c_str(), L"r");
    }
    std::wstring appDir = L"";
    if (fpPointer) {
        wchar_t lineBuf[MAX_PATH] = {0};
        if (fgetws(lineBuf, MAX_PATH, fpPointer)) {
            size_t len = wcslen(lineBuf);
            while (len > 0 && (lineBuf[len - 1] == L'\r' || lineBuf[len - 1] == L'\n' || lineBuf[len - 1] == L' ' || lineBuf[len - 1] == L'\t')) {
                lineBuf[--len] = L'\0';
            }
            if (len > 0 && (lineBuf[len - 1] == L'\\' || lineBuf[len - 1] == L'/')) {
                lineBuf[--len] = L'\0';
            }
            if (len > 0) {
                appDir = lineBuf;
            }
        }
        while (fgetws(lineBuf, MAX_PATH, fpPointer)) {
            std::wstring opt = lineBuf;
            if (opt.find(L"use_machine_trans=0") != std::wstring::npos) {
                useMachineTrans = false;
            } else if (opt.find(L"use_machine_trans=1") != std::wstring::npos) {
                useMachineTrans = true;
            }
        }
        fclose(fpPointer);
    }

    if (appDir.empty()) {
        shared_log::LogHook("[DICT] Error: Cannot find launcher appDir pointer from %ls", appDirPointerPath.c_str());
        return;
    }

    std::wstring launcherJsonc = appDir + L"\\translations\\qt_translations.jsonc";
    DWORD dwAttrib = GetFileAttributesW(launcherJsonc.c_str());
    if (dwAttrib == INVALID_FILE_ATTRIBUTES || (dwAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
        shared_log::LogHook("[DICT] Error: Primary Qt dictionary not found at %ls", launcherJsonc.c_str());
        return;
    }
    jsoncPath = launcherJsonc;

    if (useMachineTrans) {
        std::wstring launcherFallback = appDir + L"\\translations\\qt_fallback.jsonc";
        DWORD fbAttrib = GetFileAttributesW(launcherFallback.c_str());
        if (fbAttrib != INVALID_FILE_ATTRIBUTES && !(fbAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
            fallbackPath = launcherFallback;
        }
    }

    // 分层解析兜底字典与主字典，保证：
    // 1. 正向字典：主字典覆盖兜底字典；
    // 2. 反向字典：主字典条目绝对优先于兜底字典，同层级内部按 UI 规范度权重（TitleCase > 全小写）稳定裁决。
    std::unordered_map<std::string, std::string> fbCommon;
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> fbScoped;
    if (!fallbackPath.empty()) {
        std::wstring fbErr;
        DictionaryCompiler::ParseJsoncFileToMaps(fallbackPath, fbCommon, fbScoped, fbErr);
    }

    std::unordered_map<std::string, std::string> primaryCommon;
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> primaryScoped;
    std::wstring err;
    bool primaryOk = DictionaryCompiler::ParseJsoncFileToMaps(jsoncPath, primaryCommon, primaryScoped, err);

    std::unordered_map<std::string, std::string> newCommonDict = fbCommon;
    for (const auto& kv : primaryCommon) {
        newCommonDict[kv.first] = kv.second;
    }

    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> newScopedDicts = fbScoped;
    for (const auto& sec : primaryScoped) {
        auto& targetSec = newScopedDicts[sec.first];
        for (const auto& kv : sec.second) {
            targetSec[kv.first] = kv.second;
        }
    }

    if (primaryOk || !newCommonDict.empty() || !newScopedDicts.empty()) {
        shared_log::LogHook("[DICT] Loaded JSONC dictionary from %ls (fallback: %ls): %zu common, %zu scoped modules",
            jsoncPath.c_str(), fallbackPath.empty() ? L"none" : fallbackPath.c_str(),
            newCommonDict.size(), newScopedDicts.size());
    } else {
        shared_log::LogHook("[DICT] Failed to load JSONC dictionary from %ls: %ls", jsoncPath.c_str(), err.c_str());
    }

    // 构建公共反向字典：先对兜底词条做规范度优选，再由主字典优选词条高优先级覆盖
    std::unordered_map<std::string, std::string> newCommonReverseDict;
    for (const auto& kv : fbCommon) {
        InsertReverseEntryWithPriority(newCommonReverseDict, kv.first, kv.second);
    }
    std::unordered_map<std::string, std::string> primaryCommonRev;
    for (const auto& kv : primaryCommon) {
        InsertReverseEntryWithPriority(primaryCommonRev, kv.first, kv.second);
    }
    for (const auto& kv : primaryCommonRev) {
        newCommonReverseDict[kv.first] = kv.second;
    }

    // 构建模块作用域反向字典
    std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> newScopedReverseDicts;
    for (const auto& sec : fbScoped) {
        auto& revMap = newScopedReverseDicts[sec.first];
        for (const auto& kv : sec.second) {
            InsertReverseEntryWithPriority(revMap, kv.first, kv.second);
        }
    }
    for (const auto& sec : primaryScoped) {
        std::unordered_map<std::string, std::string> primarySecRev;
        for (const auto& kv : sec.second) {
            InsertReverseEntryWithPriority(primarySecRev, kv.first, kv.second);
        }
        auto& revMap = newScopedReverseDicts[sec.first];
        for (const auto& kv : primarySecRev) {
            revMap[kv.first] = kv.second;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_DictMutex);
        g_CommonDict.swap(newCommonDict);
        g_CommonCache.clear();
        g_ScopedDicts.swap(newScopedDicts);
        g_ScopedCaches.clear();
        g_CommonReverseDict.swap(newCommonReverseDict);
        g_CommonReverseCache.clear();
        g_ScopedReverseDicts.swap(newScopedReverseDicts);
        g_ScopedReverseCaches.clear();
    }
}

void EnsureDictionaryLoaded() {
    if (g_bDictLoaded.load(std::memory_order_acquire)) {
        return;
    }
    std::call_once(g_dictInitFlag, [] {
        ScanKnownToolModules();
        LoadMasterTranslations();
        g_bDictLoaded.store(true, std::memory_order_release);
    });
}

void ClearAllTranslations() {
    std::lock_guard<std::mutex> lock(g_DictMutex);
    g_CommonDict.clear();
    g_CommonCache.clear();
    g_ScopedDicts.clear();
    g_ScopedCaches.clear();
    g_CommonReverseDict.clear();
    g_CommonReverseCache.clear();
    g_ScopedReverseDicts.clear();
    g_ScopedReverseCaches.clear();
}

bool IsTranslationEnabled() {
    return g_bTranslationEnabled.load(std::memory_order_acquire);
}

void SetTranslationEnabled(bool enabled) {
    g_bTranslationEnabled.store(enabled, std::memory_order_release);
}

bool IsFilePathOrIdentifier(const std::string& s) {
    if (s.empty()) return false;

    // 1. URL 协议检测
    if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("file://", 0) == 0) {
        return true;
    }

    // 2. Windows 反斜杠路径
    if (s.find('\\') != std::string::npos) {
        return true;
    }

    // 3. 正斜杠路径检测（排除纯数字比率）
    size_t slash = s.find('/');
    while (slash != std::string::npos) {
        bool isFraction = (slash > 0 && slash + 1 < s.length() && 
                           isdigit((unsigned char)s[slash - 1]) && 
                           isdigit((unsigned char)s[slash + 1]));
        if (!isFraction) {
            return true;
        }
        slash = s.find('/', slash + 1);
    }

    // 4. 文件扩展名检测
    size_t dot = s.rfind('.');
    if (dot != std::string::npos && dot > 0 && dot + 1 < s.length()) {
        if (s[dot - 1] != '.' && (dot + 1 >= s.length() || s[dot + 1] != '.')) {
            std::string ext = s.substr(dot + 1);
            bool validExt = true;
            for (char c : ext) {
                if (!isalnum((unsigned char)c) && c != '_') {
                    validExt = false;
                    break;
                }
            }
            if (validExt && ext.length() >= 1 && ext.length() <= 8) {
                bool allDigits = true;
                for (char c : ext) {
                    if (!isdigit((unsigned char)c)) { allDigits = false; break; }
                }
                if (!allDigits) {
                    return true;
                }
            }
        }
    }

    return false;
}

bool FindTranslationScoped(void* callerAddr, const char* text, std::string& outResult, TranslationSource source) {
    if (!text || text[0] == '\0') return false;
    if (!g_bTranslationEnabled.load(std::memory_order_relaxed)) return false;
    if (!g_bDictLoaded.load(std::memory_order_acquire)) return false;

    // 绘制热路径短路：已包含中文字符的文本无需再次执行正向查表与栈回溯
    if (source == TranslationSource::Painter && capture::ContainsChinese(text)) {
        return false;
    }

    std::string textStr(text);

    bool isFileOrPath = IsFilePathOrIdentifier(textStr);
    if (isFileOrPath && source != TranslationSource::StaticUI) {
        return false;
    }

    wchar_t callerStemBuf[64] = {0};
    bool hasCaller = ResolveCallerModuleName(callerAddr, callerStemBuf, 64);
    std::wstring callerStem = hasCaller ? callerStemBuf : L"";

    if (source == TranslationSource::Painter) {
        bool explicitlyInScope = false;
        if (hasCaller) {
            std::lock_guard<std::mutex> lock(g_DictMutex);
            auto itSec = g_ScopedDicts.find(callerStem);
            if (itSec != g_ScopedDicts.end() && itSec->second.find(textStr) != itSec->second.end()) {
                explicitlyInScope = true;
            }
        }
        if (!explicitlyInScope) {
            if (textStr.find('_') != std::string::npos) {
                std::lock_guard<std::mutex> lock(g_DictMutex);
                if (g_CommonDict.find(textStr) == g_CommonDict.end()) {
                    return false;
                }
            }
            if (!textStr.empty() && textStr.find(' ') == std::string::npos) {
                bool allLower = true;
                for (char c : textStr) {
                    if (!islower((unsigned char)c)) { allLower = false; break; }
                }
                if (allLower) {
                    return false;
                }
            }
        }
    }

    if (source == TranslationSource::ItemWidget) {
        if (textStr.find('_') != std::string::npos) {
            std::lock_guard<std::mutex> lock(g_DictMutex);
            bool inDict = (g_CommonDict.find(textStr) != g_CommonDict.end());
            if (!inDict && hasCaller) {
                auto itSec = g_ScopedDicts.find(callerStem);
                if (itSec != g_ScopedDicts.end() && itSec->second.find(textStr) != itSec->second.end()) {
                    inDict = true;
                }
                if (!inDict && callerStem != L"hammer") {
                    auto itHammer = g_ScopedDicts.find(L"hammer");
                    if (itHammer != g_ScopedDicts.end() && itHammer->second.find(textStr) != itHammer->second.end()) {
                        inDict = true;
                    }
                }
            }
            if (!inDict) {
                return false;
            }
        }
    }

    auto tryDirectScoped = [&](const std::unordered_map<std::string, std::string>& dict, 
                               std::unordered_map<std::string, std::string>& cache) -> bool {
        auto itDirect = dict.find(textStr);
        if (itDirect != dict.end()) {
            outResult = itDirect->second;
            return true;
        }
        auto itCache = cache.find(textStr);
        if (itCache != cache.end()) {
            outResult = itCache->second;
            return true;
        }
        size_t first = textStr.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            size_t last = textStr.find_last_not_of(" \t\r\n");
            if (first > 0 || last < textStr.length() - 1) {
                std::string sub = textStr.substr(first, last - first + 1);
                auto itSub = dict.find(sub);
                if (itSub != dict.end()) {
                    outResult = textStr.substr(0, first) + itSub->second + textStr.substr(last + 1);
                    cache[textStr] = outResult;
                    return true;
                }
            }
        }
        if (textStr.find('&') != std::string::npos) {
            std::string stripped;
            stripped.reserve(textStr.length());
            for (char c : textStr) {
                if (c != '&') stripped.push_back(c);
            }
            auto itStrip = dict.find(stripped);
            if (itStrip != dict.end()) {
                outResult = itStrip->second;
                cache[textStr] = outResult;
                return true;
            }
        }
        return false;
    };

    if (hasCaller) {
        std::lock_guard<std::mutex> lock(g_DictMutex);
        auto itSec = g_ScopedDicts.find(callerStem);
        if (itSec != g_ScopedDicts.end()) {
            const auto& secDict = itSec->second;
            auto& secCache = g_ScopedCaches[callerStem];

            if (tryDirectScoped(secDict, secCache)) {
                return true;
            }

            if (source == TranslationSource::StaticUI && !isFileOrPath) {
                if (MatchAndTranslateInternal(secDict, textStr, outResult)) {
                    secCache[textStr] = outResult;
                    return true;
                }
            }
        }

        // 资产浏览器 (assetbrowser.dll) 包含大量动态用户资源项，未命中其专属字典时非静态 UI 严禁回退至通用词典
        if (source != TranslationSource::StaticUI && callerStem == L"assetbrowser") {
            return false;
        }

        if (callerStem != L"hammer") {
            auto itHammer = g_ScopedDicts.find(L"hammer");
            if (itHammer != g_ScopedDicts.end()) {
                const auto& hammerDict = itHammer->second;
                auto& hammerCache = g_ScopedCaches[L"hammer"];

                if (tryDirectScoped(hammerDict, hammerCache)) {
                    return true;
                }

                if (source == TranslationSource::StaticUI && !isFileOrPath) {
                    if (MatchAndTranslateInternal(hammerDict, textStr, outResult)) {
                        hammerCache[textStr] = outResult;
                        return true;
                    }
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_DictMutex);
        if (!g_CommonDict.empty()) {
            if (tryDirectScoped(g_CommonDict, g_CommonCache)) {
                return true;
            }

            if (source == TranslationSource::StaticUI && !isFileOrPath) {
                if (MatchAndTranslateInternal(g_CommonDict, textStr, outResult)) {
                    g_CommonCache[textStr] = outResult;
                    return true;
                }
            }
        }
    }

    if (!isFileOrPath && (source == TranslationSource::StaticUI || source == TranslationSource::ItemWidget)) {
        bool isAlreadyTranslated = capture::ContainsChinese(text);
        if (!isAlreadyTranslated) {
            std::lock_guard<std::mutex> lock(g_DictMutex);
            if (g_CommonReverseDict.find(textStr) != g_CommonReverseDict.end()) {
                isAlreadyTranslated = true;
            } else if (hasCaller) {
                auto itRev = g_ScopedReverseDicts.find(callerStem);
                if (itRev != g_ScopedReverseDicts.end() && itRev->second.find(textStr) != itRev->second.end()) {
                    isAlreadyTranslated = true;
                }
            }
        }

        if (!isAlreadyTranslated) {
            g_UntranslatedCapture.record(callerStemBuf, text);
        }
    }

    return false;
}

bool FindTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult, TranslationSource source) {
    if (!wstr || wstr[0] == L'\0') return false;
    char utf8Stack[1024] = {0};
    int written = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8Stack, sizeof(utf8Stack), NULL, NULL);
    if (written > 0) {
        return FindTranslationScoped(callerAddr, utf8Stack, outResult, source);
    }
    std::string dynamicUtf8 = enc::WideToUtf8(wstr);
    if (!dynamicUtf8.empty()) {
        return FindTranslationScoped(callerAddr, dynamicUtf8.c_str(), outResult, source);
    }
    return false;
}

bool FindReverseTranslationScoped(void* callerAddr, const char* text, std::string& outResult) {
    if (!text || text[0] == '\0') return false;
    if (!g_bDictLoaded.load(std::memory_order_acquire)) return false;

    bool hasNonAscii = false;
    for (const char* p = text; *p; ++p) {
        if (static_cast<unsigned char>(*p) >= 0x80) {
            hasNonAscii = true;
            break;
        }
    }
    if (!hasNonAscii) return false;

    std::string textStr(text);
    wchar_t callerStemBuf[64] = {0};
    bool hasCaller = ResolveCallerModuleName(callerAddr, callerStemBuf, 64);
    std::wstring callerStem = hasCaller ? callerStemBuf : L"";

    auto tryDirectScopedReverse = [&](const std::unordered_map<std::string, std::string>& revDict,
                                      std::unordered_map<std::string, std::string>& revCache) -> bool {
        auto itCache = revCache.find(textStr);
        if (itCache != revCache.end()) {
            outResult = itCache->second;
            return true;
        }

        auto itDirect = revDict.find(textStr);
        if (itDirect != revDict.end()) {
            outResult = itDirect->second;
            revCache[textStr] = outResult;
            return true;
        }

        size_t first = textStr.find_first_not_of(" \t\r\n");
        if (first != std::string::npos) {
            size_t last = textStr.find_last_not_of(" \t\r\n");
            std::string prefixPad = textStr.substr(0, first);
            std::string suffixPad = textStr.substr(last + 1);
            std::string trimmed = textStr.substr(first, last - first + 1);

            auto itTrimmed = revDict.find(trimmed);
            if (itTrimmed != revDict.end()) {
                outResult = prefixPad + itTrimmed->second + suffixPad;
                revCache[textStr] = outResult;
                return true;
            }

            if (trimmed.find('&') != std::string::npos) {
                std::string stripped;
                stripped.reserve(trimmed.length());
                for (char c : trimmed) {
                    if (c != '&') stripped.push_back(c);
                }
                auto itStrip = revDict.find(stripped);
                if (itStrip != revDict.end()) {
                    outResult = prefixPad + itStrip->second + suffixPad;
                    revCache[textStr] = outResult;
                    return true;
                }
            }

            size_t tLen = trimmed.length();
            if (tLen >= 4 && trimmed.back() == ')') {
                if (trimmed[tLen - 4] == '(' && trimmed[tLen - 3] == '&' &&
                    isalnum(static_cast<unsigned char>(trimmed[tLen - 2]))) {
                    std::string base = trimmed.substr(0, tLen - 4);
                    size_t bLast = base.find_last_not_of(" \t");
                    if (bLast != std::string::npos) base = base.substr(0, bLast + 1);
                    auto itBase = revDict.find(base);
                    if (itBase != revDict.end()) {
                        outResult = prefixPad + itBase->second + suffixPad;
                        revCache[textStr] = outResult;
                        return true;
                    }
                }
            }

            static const std::string kCnEllipsis = "\xe2\x80\xa6"; // …
            static const std::string kEnEllipsis = "...";
            static const std::string kCnColon = "\xef\xbc\x9a"; // ：
            static const std::string kEnColon = ":";

            auto tryReplaceAndFind = [&](const std::string& from, const std::string& to) -> bool {
                size_t pos = trimmed.rfind(from);
                if (pos != std::string::npos && pos + from.length() == trimmed.length()) {
                    std::string base = trimmed.substr(0, pos);
                    size_t bLast = base.find_last_not_of(" \t");
                    if (bLast != std::string::npos) base = base.substr(0, bLast + 1);
                    auto itBase = revDict.find(base);
                    if (itBase != revDict.end()) {
                        outResult = prefixPad + itBase->second + to + suffixPad;
                        revCache[textStr] = outResult;
                        return true;
                    }
                }
                return false;
            };

            if (tryReplaceAndFind(kCnEllipsis, kEnEllipsis)) return true;
            if (tryReplaceAndFind(kEnEllipsis, kCnEllipsis)) return true;
            if (tryReplaceAndFind(kCnColon, kEnColon)) return true;
            if (tryReplaceAndFind(kEnColon, kCnColon)) return true;
        }

        return false;
    };

    std::lock_guard<std::mutex> lock(g_DictMutex);

    if (hasCaller) {
        auto itSec = g_ScopedReverseDicts.find(callerStem);
        if (itSec != g_ScopedReverseDicts.end()) {
            auto& secCache = g_ScopedReverseCaches[callerStem];
            if (tryDirectScopedReverse(itSec->second, secCache)) {
                return true;
            }
        }
        if (callerStem != L"hammer") {
            auto itHammer = g_ScopedReverseDicts.find(L"hammer");
            if (itHammer != g_ScopedReverseDicts.end()) {
                auto& hammerCache = g_ScopedReverseCaches[L"hammer"];
                if (tryDirectScopedReverse(itHammer->second, hammerCache)) {
                    return true;
                }
            }
        }
    }

    if (!g_CommonReverseDict.empty()) {
        if (tryDirectScopedReverse(g_CommonReverseDict, g_CommonReverseCache)) {
            return true;
        }
    }

    // 兜底：若由 Qt 底层异步重绘触发（栈上仅有 Qt 框架模块）且未在 common/hammer 命中，遍历其余工具作用域反向字典
    if (!hasCaller || IsQtFrameworkModule(callerStemBuf)) {
        for (const auto& sec : g_ScopedReverseDicts) {
            if (sec.first == L"hammer") continue;
            auto& secCache = g_ScopedReverseCaches[sec.first];
            if (tryDirectScopedReverse(sec.second, secCache)) {
                return true;
            }
        }
    }

    return false;
}

bool FindReverseTranslationScopedW(void* callerAddr, const wchar_t* wstr, std::string& outResult) {
    if (!wstr || wstr[0] == L'\0') return false;
    bool hasNonAscii = false;
    for (const wchar_t* p = wstr; *p; ++p) {
        if (*p >= 128) {
            hasNonAscii = true;
            break;
        }
    }
    if (!hasNonAscii) return false;

    char utf8Stack[1024] = {0};
    int written = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, utf8Stack, sizeof(utf8Stack), NULL, NULL);
    if (written > 0) {
        return FindReverseTranslationScoped(callerAddr, utf8Stack, outResult);
    }
    std::string dynamicUtf8 = enc::WideToUtf8(wstr);
    if (!dynamicUtf8.empty()) {
        return FindReverseTranslationScoped(callerAddr, dynamicUtf8.c_str(), outResult);
    }
    return false;
}

} // namespace qm
