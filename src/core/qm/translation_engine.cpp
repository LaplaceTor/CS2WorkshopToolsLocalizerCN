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
#include <unordered_set>
#include <vector>

namespace qm {

namespace {

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

std::mutex g_ProtectedFgdIoMutex;
std::unordered_set<std::string> g_ProtectedFgdIoNames{
    "command", "trigger", "enable", "disable", "toggle",
    "open", "close", "lock", "unlock", "start", "stop",
    "kill", "killhierarchy", "reset", "cancel", "cancelpending",
    "break", "use", "press", "alpha", "color", "scale", "skin",
    "add", "subtract", "multiply", "divide", "compare", "test",
    "display", "fade", "ignite", "ignitelifetime", "explode",
    "spawn", "wake", "sleep", "touch", "activate", "deactivate",
    "play", "playsound", "startsound", "stopsound", "pause", "resume",
    "pitch", "volume", "show", "hide", "clear", "reload",
    "increment", "decrement", "setvalue", "setvaluenofire", "getvalue",
    "sethitmax", "sethitmin", "sethealth", "addhealth", "removehealth",
    "setparent", "clearparent", "setparentattachment",
    "setparentattachmentmaintainoffset", "followentity", "addoutput",
    "fireuser1", "fireuser2", "fireuser3", "fireuser4",
    "turnon", "turnoff", "setanimation", "setdefaultanimation",
    "setplaybackrate", "setbodygroup", "setmaterialgroup",
    "enablemotion", "disablemotion", "enableshadow", "disableshadow",
    "enabledraw", "disabledraw", "enabledamageforces", "disabledamageforces",
    "ontrigger", "onstarttouch", "onendtouch", "onstarttouchall", "onendtouchall",
    "onpressed", "onin", "onout", "onopen", "onclose",
    "onfullyopen", "onfullyclosed", "onblockedopening", "onblockedclosing",
    "onunblockedopening", "onunblockedclosing", "onlockeduse", "onbreak",
    "onhealthchanged", "ontakedamage", "ondamaged", "onkilled", "onignite",
    "onuser1", "onuser2", "onuser3", "onuser4", "onspawn",
    "ontimer", "ontimerhigh", "ontimerlow", "ontrue", "onfalse",
    "onpass", "onfail", "ondefault", "outvalue",
    "onhitmin", "onhitmax", "ongetvalue", "onchangedfrommin", "onchangedfrommax"
};

std::string NormalizeProtectedIoCandidate(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    std::string out;
    out.reserve(last - first + 1);
    for (size_t i = first; i <= last; ++i) {
        char c = s[i];
        if (c == '&') continue;
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

capture::UntranslatedCaptureWriter g_UntranslatedCapture;

struct MasterModuleItem {
    uintptr_t base;
    uintptr_t end;
    std::wstring stem;
};

ModuleRangeSnapshot g_SnapshotBuffers[2];
std::atomic<size_t> g_ActiveSnapshotIndex{0};
std::atomic<const ModuleRangeSnapshot*> g_pActiveSnapshot{nullptr};

std::mutex g_ModuleWriterMutex;
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

    if (TryDirectMatch(dict, text, outResult)) return true;

    std::string titleCase = ToTitleCase(text);
    if (titleCase != text) {
        if (TryDirectMatch(dict, titleCase, outResult)) return true;
    }
    std::string sentenceCase = ToSentenceCase(text);
    if (sentenceCase != text && sentenceCase != titleCase) {
        if (TryDirectMatch(dict, sentenceCase, outResult)) return true;
    }

    if (text.find('&') != std::string::npos) {
        std::string stripped;
        stripped.reserve(text.length());
        for (char c : text) {
            if (c != '&') stripped.push_back(c);
        }
        if (MatchAndTranslateInternal(dict, stripped, outResult, depth + 1)) return true;
    }

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

    size_t tabPos = text.find('\t');
    if (tabPos != std::string::npos && tabPos > 0) {
        std::string base = text.substr(0, tabPos);
        std::string baseTrans;
        if (MatchAndTranslateInternal(dict, base, baseTrans, depth + 1)) {
            outResult = baseTrans + text.substr(tabPos);
            return true;
        }
    }

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
    } else if (text.length() >= 3 && text.substr(text.length() - 3) == "\xe2\x80\xa6") {
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

inline bool IsQtFrameworkModule(const wchar_t* stem) {
    if (!stem || stem[0] == L'\0') return false;
    return (_wcsicmp(stem, L"qt5widgets") == 0 ||
            _wcsicmp(stem, L"qt5gui") == 0 ||
            _wcsicmp(stem, L"qt5core") == 0 ||
            _wcsicmp(stem, L"qt5concurrent") == 0 ||
            _wcsicmp(stem, L"qtadvanceddocking-qt5") == 0 ||
            _wcsicmp(stem, L"qtcore_qm") == 0);
}

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
        score += 30;
    } else if (startsUpper && !hasLower) {
        score += 20;
    } else if (!startsLower && hasUpper) {
        score += 10;
    } else if (startsLower && hasUpper) {
        score += 5;
    } else {
        score += 0;
    }

    if (hasUnderscore) score -= 15;
    if (hasAmpersand)  score -= 5;

    return score;
}

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
        it->second = en;
    }
}

}

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

    bool hasDirect = (directCallerAddr != nullptr && GetCallerModuleName(directCallerAddr, outBuf, maxLen));
    if (hasDirect && !IsQtFrameworkModule(outBuf)) {
        return true;
    }

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
            return true;
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

bool ReadAppDirPointer(const std::wstring& binDir,
                       std::wstring& outAppDir,
                       bool& outUseMachineTrans,
                       std::wstring* outPointerPath) {
    outAppDir.clear();
    outUseMachineTrans = true;

    const std::wstring pointerPath = binDir + L"localizer_appdir.txt";
    if (outPointerPath) {
        *outPointerPath = pointerPath;
    }

    FILE* fpPointer = _wfopen(pointerPath.c_str(), L"r, ccs=UTF-8");
    if (!fpPointer) {
        fpPointer = _wfopen(pointerPath.c_str(), L"r");
    }
    if (!fpPointer) {
        return false;
    }

    constexpr size_t kLineBufSize = 1024;
    wchar_t lineBuf[kLineBufSize] = {0};
    if (fgetws(lineBuf, static_cast<int>(kLineBufSize), fpPointer)) {
        size_t len = wcslen(lineBuf);
        while (len > 0 && (lineBuf[len - 1] == L'\r' || lineBuf[len - 1] == L'\n' || lineBuf[len - 1] == L' ' || lineBuf[len - 1] == L'\t')) {
            lineBuf[--len] = L'\0';
        }
        if (len > 0 && (lineBuf[len - 1] == L'\\' || lineBuf[len - 1] == L'/')) {
            lineBuf[--len] = L'\0';
        }
        if (len > 0) {
            outAppDir = lineBuf;
        }
    }
    while (fgetws(lineBuf, static_cast<int>(kLineBufSize), fpPointer)) {
        std::wstring opt = lineBuf;
        if (opt.find(L"use_machine_trans=0") != std::wstring::npos) {
            outUseMachineTrans = false;
        } else if (opt.find(L"use_machine_trans=1") != std::wstring::npos) {
            outUseMachineTrans = true;
        }
    }
    fclose(fpPointer);
    return !outAppDir.empty();
}

void LoadMasterTranslations() {
    std::wstring binDir = GetBinDirectory();
    std::wstring jsoncPath = L"";
    std::wstring fallbackPath = L"";
    std::wstring appDir;
    std::wstring appDirPointerPath;
    bool useMachineTrans = true;

    if (!ReadAppDirPointer(binDir, appDir, useMachineTrans, &appDirPointerPath)) {
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

    if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("file://", 0) == 0) {
        return true;
    }

    if (s.find('\\') != std::string::npos) {
        return true;
    }

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

void RegisterProtectedFgdIoNames(const std::vector<std::string>& names) {
    if (names.empty()) return;
    std::lock_guard<std::mutex> lock(g_ProtectedFgdIoMutex);
    for (const std::string& name : names) {
        std::string norm = NormalizeProtectedIoCandidate(name);
        if (!norm.empty()) {
            g_ProtectedFgdIoNames.insert(std::move(norm));
        }
    }
}

void ExtractAndRegisterFgdIoNames(const std::string& fgdContent) {
    if (fgdContent.empty()) return;

    std::vector<std::string> extracted;
    size_t lineStart = 0;
    const size_t n = fgdContent.size();

    while (lineStart < n) {
        size_t lineEnd = fgdContent.find('\n', lineStart);
        if (lineEnd == std::string::npos) {
            lineEnd = n;
        }

        size_t codeEnd = lineEnd;
        bool inQuote = false;
        for (size_t i = lineStart; i < lineEnd; ++i) {
            char c = fgdContent[i];
            if (inQuote) {
                if (c == '\\' && i + 1 < lineEnd) {
                    ++i;
                    continue;
                }
                if (c == '"') {
                    inQuote = false;
                }
            } else {
                if (c == '"') {
                    inQuote = true;
                } else if (c == '/' && i + 1 < lineEnd && fgdContent[i + 1] == '/') {
                    codeEnd = i;
                    break;
                }
            }
        }

        size_t pos = lineStart;
        while (pos < codeEnd && (fgdContent[pos] == ' ' || fgdContent[pos] == '\t' || fgdContent[pos] == '\r')) {
            ++pos;
        }

        size_t kwLen = 0;
        if (pos + 5 < codeEnd &&
            std::tolower(static_cast<unsigned char>(fgdContent[pos])) == 'i' &&
            std::tolower(static_cast<unsigned char>(fgdContent[pos + 1])) == 'n' &&
            std::tolower(static_cast<unsigned char>(fgdContent[pos + 2])) == 'p' &&
            std::tolower(static_cast<unsigned char>(fgdContent[pos + 3])) == 'u' &&
            std::tolower(static_cast<unsigned char>(fgdContent[pos + 4])) == 't' &&
            (fgdContent[pos + 5] == ' ' || fgdContent[pos + 5] == '\t')) {
            kwLen = 5;
        } else if (pos + 6 < codeEnd &&
                   std::tolower(static_cast<unsigned char>(fgdContent[pos])) == 'o' &&
                   std::tolower(static_cast<unsigned char>(fgdContent[pos + 1])) == 'u' &&
                   std::tolower(static_cast<unsigned char>(fgdContent[pos + 2])) == 't' &&
                   std::tolower(static_cast<unsigned char>(fgdContent[pos + 3])) == 'p' &&
                   std::tolower(static_cast<unsigned char>(fgdContent[pos + 4])) == 'u' &&
                   std::tolower(static_cast<unsigned char>(fgdContent[pos + 5])) == 't' &&
                   (fgdContent[pos + 6] == ' ' || fgdContent[pos + 6] == '\t')) {
            kwLen = 6;
        }

        if (kwLen > 0) {
            pos += kwLen;
            while (pos < codeEnd && (fgdContent[pos] == ' ' || fgdContent[pos] == '\t')) {
                ++pos;
            }
            size_t nameStart = pos;
            while (pos < codeEnd && (std::isalnum(static_cast<unsigned char>(fgdContent[pos])) || fgdContent[pos] == '_')) {
                ++pos;
            }
            if (pos > nameStart) {
                size_t checkParen = pos;
                while (checkParen < codeEnd && (fgdContent[checkParen] == ' ' || fgdContent[checkParen] == '\t')) {
                    ++checkParen;
                }
                if (checkParen < codeEnd && fgdContent[checkParen] == '(') {
                    extracted.emplace_back(fgdContent.substr(nameStart, pos - nameStart));
                }
            }
        }

        lineStart = (lineEnd < n) ? (lineEnd + 1) : n;
    }

    if (!extracted.empty()) {
        RegisterProtectedFgdIoNames(extracted);
    }
}

bool IsProtectedFgdIoName(const std::string& s) {
    std::string norm = NormalizeProtectedIoCandidate(s);
    if (norm.empty()) return false;
    std::lock_guard<std::mutex> lock(g_ProtectedFgdIoMutex);
    return g_ProtectedFgdIoNames.find(norm) != g_ProtectedFgdIoNames.end();
}

bool FindTranslationScoped(void* callerAddr, const char* text, std::string& outResult, TranslationSource source) {
    if (!text || text[0] == '\0') return false;
    if (!g_bTranslationEnabled.load(std::memory_order_relaxed)) return false;
    if (!g_bDictLoaded.load(std::memory_order_acquire)) return false;

    if (source == TranslationSource::Painter && capture::ContainsChinese(text)) {
        return false;
    }

    std::string textStr(text);

    bool isFileOrPath = IsFilePathOrIdentifier(textStr);
    if (isFileOrPath && source != TranslationSource::StaticUI) {
        return false;
    }

    if (source != TranslationSource::StaticUI && IsProtectedFgdIoName(textStr)) {
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

            static const std::string kCnEllipsis = "\xe2\x80\xa6";
            static const std::string kEnEllipsis = "...";
            static const std::string kCnColon = "\xef\xbc\x9a";
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

}
