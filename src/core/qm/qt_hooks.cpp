#include "core/qm/qt_hooks.h"
#include "core/qm/translation_engine.h"
#include "core/hook_manager.h"
#include "core/pe_patcher.h"
#include "core/shared_log.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace qm {

extern "C" __declspec(dllexport) bool InitializeTranslator();

namespace {

// ==============================================================================
// 1. Qt 核心函数指针定义 (MSVC x64 C++ ABI)
// ==============================================================================
typedef void* (__fastcall *fnQMetaObject_tr)(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n);
typedef void* (__fastcall *fnQString_fromUtf8)(void* pOutQString, const char* utf8, int size);
typedef const wchar_t* (__fastcall *fnQString_utf16)(const void* pQString);
typedef void (__fastcall *fnQString_dtor)(void* pQString);

typedef void (__fastcall *fnQAction_activate)(void* pAction, int event);
typedef void (__fastcall *fnQWidget_insertAction)(void* pWidget, void* pBefore, void* pAction);
typedef void (__fastcall *fnQWidget_addAction)(void* pWidget, void* pAction);
typedef void* (__fastcall *fnQWidget_find)(uint64_t wid);

typedef void* (__fastcall *fnQAction_ctor)(void* pAction, const void* pQString, void* pParent);
typedef void* (__fastcall *fnQAction_ctor_icon)(void* pAction, const void* pIcon, const void* pQString, void* pParent);
typedef void (__fastcall *fnQAction_setText)(void* pAction, const void* pQString);
typedef void (__fastcall *fnQAction_setToolTip)(void* pAction, const void* pQString);
typedef void (__fastcall *fnQAction_setStatusTip)(void* pAction, const void* pQString);
typedef void (__fastcall *fnQAction_setWhatsThis)(void* pAction, const void* pQString);

typedef void (__fastcall *fnQAbstractButton_setText)(void* pButton, const void* pQString);
typedef void (__fastcall *fnQAbstractButton_setIcon)(void* pButton, const void* pIcon);
typedef void (__fastcall *fnQToolButton_setDefaultAction)(void* pButton, void* pAction);
typedef void* (__fastcall *fnQToolButton_defaultAction)(const void* pButton);
typedef void (__fastcall *fnQAction_setIcon)(void* pAction, const void* pIcon);
typedef bool (__fastcall *fnQObject_inherits)(const void* pObject, const char* classname);
typedef void (__fastcall *fnQLabel_setText)(void* pLabel, const void* pQString);
typedef void (__fastcall *fnQWidget_setWindowTitle)(void* pWidget, const void* pQString);
typedef void (__fastcall *fnQGroupBox_setTitle)(void* pBox, const void* pQString);

typedef void (__fastcall *fnQTreeWidgetItem_setText)(void* pItem, int column, const void* pQString);
typedef void (__fastcall *fnQTreeWidget_setHeaderLabel)(void* pTree, const void* pQString);
typedef void* (__fastcall *fnQTableWidgetItem_ctor)(void* pItem, const void* pQString, int type);
typedef void (__fastcall *fnQTableWidgetItem_setText)(void* pItem, const void* pQString);
typedef void* (__fastcall *fnQListWidgetItem_ctor)(void* pItem, const void* pQString, void* pListWidget, int type);
typedef void (__fastcall *fnQListWidgetItem_setText)(void* pItem, const void* pQString);

typedef void (__fastcall *fnQComboBox_addItem)(void* pBox, const void* pQString, const void* pUserData);
typedef void (__fastcall *fnQComboBox_addItem_icon)(void* pBox, const void* pIcon, const void* pQString, const void* pUserData);
typedef void (__fastcall *fnQComboBox_setItemText)(void* pBox, int index, const void* pQString);
typedef void (__fastcall *fnQComboBox_insertItem)(void* pBox, int index, const void* pQString, const void* pUserData);
typedef void (__fastcall *fnQComboBox_insertItem_icon)(void* pBox, int index, const void* pIcon, const void* pQString, const void* pUserData);

typedef void (__fastcall *fnQPainter_drawText_Rect)(void* pPainter, const void* pRect, int flags, const void* pQString, void* pBoundingRect);
typedef void (__fastcall *fnQPainter_drawText_RectF)(void* pPainter, const void* pRectF, int flags, const void* pQString, void* pBoundingRect);
typedef void (__fastcall *fnQPainter_drawText_PointF)(void* pPainter, const void* pPointF, const void* pQString);
typedef void (__fastcall *fnQPainter_drawText_RectF_Option)(void* pPainter, const void* pRectF, const void* pQString, const void* pOption);
typedef void (__fastcall *fnQPainter_drawText_Point)(void* pPainter, const void* pPoint, const void* pQString);
typedef void (__fastcall *fnQPainter_drawText_xy)(void* pPainter, int x, int y, const void* pQString);

typedef void (__fastcall *fnQTextDocument_setPlainText)(void* pDoc, const void* pQString);
typedef void (__fastcall *fnQTextDocument_setHtml)(void* pDoc, const void* pQString);

// 原函数指针存储
fnQMetaObject_tr g_o_QMetaObject_tr = nullptr;
fnQAction_ctor g_o_QAction_ctor = nullptr;
fnQAction_ctor_icon g_o_QAction_ctor_icon = nullptr;
fnQAction_setText g_o_QAction_setText = nullptr;
fnQAction_setToolTip g_o_QAction_setToolTip = nullptr;
fnQAction_setStatusTip g_o_QAction_setStatusTip = nullptr;
fnQAction_setWhatsThis g_o_QAction_setWhatsThis = nullptr;
fnQWidget_insertAction g_o_QWidget_insertAction = nullptr;
fnQWidget_addAction g_o_QWidget_addAction = nullptr;
fnQAction_activate g_o_QAction_activate = nullptr;
fnQWidget_find g_pfn_QWidget_find = nullptr;

fnQAbstractButton_setIcon g_o_QAbstractButton_setIcon = nullptr;
fnQToolButton_setDefaultAction g_o_QToolButton_setDefaultAction = nullptr;
fnQToolButton_defaultAction g_pfn_QToolButton_defaultAction = nullptr;
fnQAction_setIcon g_pfn_QAction_setIcon = nullptr;
fnQObject_inherits g_pfn_QObject_inherits = nullptr;

fnQAbstractButton_setText g_o_QAbstractButton_setText = nullptr;
fnQLabel_setText g_o_QLabel_setText = nullptr;
fnQWidget_setWindowTitle g_o_QWidget_setWindowTitle = nullptr;
fnQGroupBox_setTitle g_o_QGroupBox_setTitle = nullptr;

fnQTreeWidgetItem_setText g_o_QTreeWidgetItem_setText = nullptr;
fnQTreeWidget_setHeaderLabel g_o_QTreeWidget_setHeaderLabel = nullptr;
fnQTableWidgetItem_ctor g_o_QTableWidgetItem_ctor = nullptr;
fnQTableWidgetItem_setText g_o_QTableWidgetItem_setText = nullptr;
fnQListWidgetItem_ctor g_o_QListWidgetItem_ctor = nullptr;
fnQListWidgetItem_setText g_o_QListWidgetItem_setText = nullptr;

fnQComboBox_addItem g_o_QComboBox_addItem = nullptr;
fnQComboBox_addItem_icon g_o_QComboBox_addItem_icon = nullptr;
fnQComboBox_setItemText g_o_QComboBox_setItemText = nullptr;
fnQComboBox_insertItem g_o_QComboBox_insertItem = nullptr;
fnQComboBox_insertItem_icon g_o_QComboBox_insertItem_icon = nullptr;

fnQPainter_drawText_Rect g_o_QPainter_drawText_Rect = nullptr;
fnQPainter_drawText_RectF g_o_QPainter_drawText_RectF = nullptr;
fnQPainter_drawText_PointF g_o_QPainter_drawText_PointF = nullptr;
fnQPainter_drawText_RectF_Option g_o_QPainter_drawText_RectF_Option = nullptr;
fnQPainter_drawText_Point g_o_QPainter_drawText_Point = nullptr;
fnQPainter_drawText_xy g_o_QPainter_drawText_xy = nullptr;

fnQTextDocument_setPlainText g_o_QTextDocument_setPlainText = nullptr;
fnQTextDocument_setHtml g_o_QTextDocument_setHtml = nullptr;

fnQString_fromUtf8 g_pfn_fromUtf8 = nullptr;
fnQString_utf16 g_pfn_utf16 = nullptr;
fnQString_dtor g_pfn_QString_dtor = nullptr;

void* g_pReloadFgdAction = nullptr;
std::mutex g_ReloadActionMutex;
fnQAction_activate g_pfn_QAction_activate = nullptr;

std::atomic<bool> g_bWidgetsHooked{false};
std::atomic<bool> g_bGuiHooked{false};
std::atomic<bool> g_bInSafeProbe{false};

struct SafeProbeScope {
    SafeProbeScope() { g_bInSafeProbe.store(true, std::memory_order_relaxed); }
    ~SafeProbeScope() { g_bInSafeProbe.store(false, std::memory_order_relaxed); }
};

struct HookRequest {
    void* pTarget;
    void* pDetour;
    void** ppOriginal;
    const char* name;
};

bool InstallHookBatch(const std::vector<HookRequest>& requests, const char* batchName) {
    std::vector<size_t> installedIndices;
    bool allOk = true;

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& req = requests[i];
        if (!req.pTarget) {
            shared_log::LogHook("[HOOK] [%s] %s symbol not found!", batchName, req.name);
            allOk = false;
            break;
        }
        if (req.ppOriginal && !*req.ppOriginal) {
            shared_log::LogHook("[HOOK] [%s] Calling InstallHook for %s (target=%p, detour=%p)...", batchName, req.name, req.pTarget, req.pDetour);
            bool ok = HookManager::Instance().InstallHook(req.pTarget, req.pDetour, req.ppOriginal, req.name);
            if (!ok) {
                shared_log::LogHook("[HOOK] [%s] %s InstallHook failed!", batchName, req.name);
                allOk = false;
                break;
            }
            installedIndices.push_back(i);
            shared_log::LogHook("[HOOK] [%s] %s hooked successfully (orig=%p)", batchName, req.name, *req.ppOriginal);
        }
    }

    if (!allOk) {
        shared_log::LogHook("[HOOK] [%s] Batch hook installation failed! Rolling back %zu installed hooks...", batchName, installedIndices.size());
        for (auto idx : installedIndices) {
            const auto& req = requests[idx];
            HookManager::Instance().UninstallHook(req.pTarget);
            if (req.ppOriginal) {
                *req.ppOriginal = nullptr;
            }
        }
        return false;
    }

    return true;
}

inline bool SafeGetUtf16(fnQString_utf16 pfn, const void* pQString, const wchar_t*& outWstr) {
    if (!pfn || !pQString || (uintptr_t)pQString < 0x10000) return false;
    __try {
        const void* d = *(const void* const*)pQString;
        if (!d || (uintptr_t)d < 0x10000) return false;
        outWstr = pfn(pQString);
        return (outWstr != nullptr && outWstr[0] != L'\0');
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outWstr = nullptr;
        return false;
    }
}

inline bool SafeCreateQString(fnQString_fromUtf8 pfn, void* pOutQString, const char* utf8, int size) {
    if (!pfn || !pOutQString || !utf8 || size <= 0) return false;
    __try {
        pfn(pOutQString, utf8, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void SafeDestroyQString(fnQString_dtor pfn, void* pQString) {
    if (!pfn || !pQString) return;
    __try {
        pfn(pQString);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

template <typename Forward>
__forceinline void ForwardTranslatedQString(void* caller, const void* pQString,
                                           TranslationSource source, Forward&& forward) {
    const wchar_t* wstr = nullptr;
    if (SafeGetUtf16(g_pfn_utf16, pQString, wstr)) {
        std::string trans;
        bool found = false;
        if (IsTranslationEnabled()) {
            found = FindTranslationScopedW(caller, wstr, trans, source);
        } else {
            found = FindReverseTranslationScopedW(caller, wstr, trans);
        }
        if (found) {
            void* qstr[1] = {0};
            if (SafeCreateQString(g_pfn_fromUtf8, qstr, trans.c_str(), static_cast<int>(trans.length()))) {
                forward(qstr);
                SafeDestroyQString(g_pfn_QString_dtor, qstr);
                return;
            }
        }
    }
    forward(pQString);
}

// -----------------------------------------------------------------------------
// 钩子转发函数
// -----------------------------------------------------------------------------
void __fastcall hk_QPainter_drawText_Rect(void* pPainter, const void* pRect, int flags, const void* pQString, void* pBoundingRect) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::Painter, [&](const void* text) {
        if (g_o_QPainter_drawText_Rect) g_o_QPainter_drawText_Rect(pPainter, pRect, flags, text, pBoundingRect);
    });
}

void __fastcall hk_QPainter_drawText_RectF(void* pPainter, const void* pRectF, int flags, const void* pQString, void* pBoundingRect) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::Painter, [&](const void* text) {
        if (g_o_QPainter_drawText_RectF) g_o_QPainter_drawText_RectF(pPainter, pRectF, flags, text, pBoundingRect);
    });
}

void __fastcall hk_QPainter_drawText_PointF(void* pPainter, const void* pPointF, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::Painter, [&](const void* text) {
        if (g_o_QPainter_drawText_PointF) g_o_QPainter_drawText_PointF(pPainter, pPointF, text);
    });
}

void __fastcall hk_QPainter_drawText_RectF_Option(void* pPainter, const void* pRectF, const void* pQString, const void* pOption) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::Painter, [&](const void* text) {
        if (g_o_QPainter_drawText_RectF_Option) g_o_QPainter_drawText_RectF_Option(pPainter, pRectF, text, pOption);
    });
}

void __fastcall hk_QPainter_drawText_Point(void* pPainter, const void* pPoint, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::Painter, [&](const void* text) {
        if (g_o_QPainter_drawText_Point) g_o_QPainter_drawText_Point(pPainter, pPoint, text);
    });
}

void __fastcall hk_QPainter_drawText_xy(void* pPainter, int x, int y, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::Painter, [&](const void* text) {
        if (g_o_QPainter_drawText_xy) g_o_QPainter_drawText_xy(pPainter, x, y, text);
    });
}

void __fastcall hk_QTextDocument_setPlainText(void* pDoc, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QTextDocument_setPlainText) g_o_QTextDocument_setPlainText(pDoc, text);
    });
}

void __fastcall hk_QTextDocument_setHtml(void* pDoc, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QTextDocument_setHtml) g_o_QTextDocument_setHtml(pDoc, text);
    });
}

inline bool SafeProbeActionCmdId(void* pAction, uint32_t& outCmdId) {
    if (!pAction || (uintptr_t)pAction < 0x10000) return false;
    __try {
        outCmdId = *(uint32_t*)((char*)pAction + 0x20);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CheckAndCacheReloadActionPtr(void* pAction) {
    if (!pAction || (uintptr_t)pAction < 0x10000) return;
    if (g_pReloadFgdAction) return;

    SafeProbeScope probeScope;
    uint32_t cmdId = 0;
    if (!SafeProbeActionCmdId(pAction, cmdId)) return;

    if (cmdId == 0x49285562) {
        std::lock_guard<std::mutex> lock(g_ReloadActionMutex);
        if (g_pReloadFgdAction != pAction) {
            g_pReloadFgdAction = pAction;
            shared_log::LogHook("[FGD] Cached 'Reload .FGD Files' QAction pointer via CommandID 0x49285562: %p", pAction);
        }
    }
}

inline void CheckAndCacheReloadAction(void* pAction, const void* pQString) {
    if (!pAction) return;
    CheckAndCacheReloadActionPtr(pAction);
    if (!g_pReloadFgdAction && pQString && g_pfn_utf16) {
        const wchar_t* wstr = nullptr;
        if (SafeGetUtf16(g_pfn_utf16, pQString, wstr) && wstr) {
            if ((wcsstr(wstr, L"FGD") != nullptr || wcsstr(wstr, L"fgd") != nullptr) &&
                (wcsstr(wstr, L"Reload") != nullptr || wcsstr(wstr, L"重载") != nullptr || wcsstr(wstr, L"重新加载") != nullptr)) {
                std::lock_guard<std::mutex> lock(g_ReloadActionMutex);
                if (g_pReloadFgdAction != pAction) {
                    g_pReloadFgdAction = pAction;
                    shared_log::LogHook("[FGD] Cached 'Reload .FGD Files' QAction pointer via explicit text ('%ls'): %p", wstr, pAction);
                }
            }
        }
    }
}

void __fastcall hk_QAction_setText(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    CheckAndCacheReloadAction(pAction, pQString);
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QAction_setText) g_o_QAction_setText(pAction, text);
    });
}

void __fastcall hk_QAction_setToolTip(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QAction_setToolTip) g_o_QAction_setToolTip(pAction, text);
    });
}

void __fastcall hk_QAction_setStatusTip(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QAction_setStatusTip) g_o_QAction_setStatusTip(pAction, text);
    });
}

void __fastcall hk_QAction_setWhatsThis(void* pAction, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QAction_setWhatsThis) g_o_QAction_setWhatsThis(pAction, text);
    });
}

void __fastcall hk_QAbstractButton_setText(void* pButton, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QAbstractButton_setText) g_o_QAbstractButton_setText(pButton, text);
    });
}

thread_local bool s_bInSyncIcon = false;

inline void SafeSyncButtonActionIcon(void* pButton, const void* pIcon) {
    if (s_bInSyncIcon || !pButton || (uintptr_t)pButton < 0x10000 || !pIcon) {
        return;
    }
    s_bInSyncIcon = true;
    __try {
        bool isToolButton = true;
        if (g_pfn_QObject_inherits) {
            isToolButton = g_pfn_QObject_inherits(pButton, "QToolButton");
        }
        if (isToolButton && g_pfn_QToolButton_defaultAction && g_pfn_QAction_setIcon) {
            void* pAction = g_pfn_QToolButton_defaultAction(pButton);
            if (pAction && (uintptr_t)pAction > 0x10000) {
                g_pfn_QAction_setIcon(pAction, pIcon);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    s_bInSyncIcon = false;
}

void __fastcall hk_QAbstractButton_setIcon(void* pButton, const void* pIcon) {
    if (g_o_QAbstractButton_setIcon) {
        g_o_QAbstractButton_setIcon(pButton, pIcon);
    }
    if (pButton && pIcon && !s_bInSyncIcon && g_pfn_QToolButton_defaultAction && g_pfn_QAction_setIcon) {
        SafeProbeScope probeScope;
        SafeSyncButtonActionIcon(pButton, pIcon);
    }
}

void __fastcall hk_QToolButton_setDefaultAction(void* pButton, void* pAction) {
    if (g_o_QToolButton_setDefaultAction) {
        g_o_QToolButton_setDefaultAction(pButton, pAction);
    }
    if (pAction && !g_pReloadFgdAction) {
        CheckAndCacheReloadActionPtr(pAction);
    }
}

void __fastcall hk_QLabel_setText(void* pLabel, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QLabel_setText) g_o_QLabel_setText(pLabel, text);
    });
}

void __fastcall hk_QWidget_setWindowTitle(void* pWidget, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QWidget_setWindowTitle) g_o_QWidget_setWindowTitle(pWidget, text);
    });
}

void __fastcall hk_QGroupBox_setTitle(void* pBox, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        if (g_o_QGroupBox_setTitle) g_o_QGroupBox_setTitle(pBox, text);
    });
}

void* __fastcall hk_QAction_ctor(void* pAction, const void* pQString, void* pParent) {
    void* caller = _ReturnAddress();
    CheckAndCacheReloadAction(pAction, pQString);
    void* result = nullptr;
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        result = g_o_QAction_ctor(pAction, text, pParent);
    });
    return result;
}

void* __fastcall hk_QAction_ctor_icon(void* pAction, const void* pIcon, const void* pQString, void* pParent) {
    void* caller = _ReturnAddress();
    CheckAndCacheReloadAction(pAction, pQString);
    void* result = nullptr;
    ForwardTranslatedQString(caller, pQString, TranslationSource::StaticUI, [&](const void* text) {
        result = g_o_QAction_ctor_icon(pAction, pIcon, text, pParent);
    });
    return result;
}

void __fastcall hk_QWidget_insertAction(void* pWidget, void* pBefore, void* pAction) {
    if (g_o_QWidget_insertAction) {
        g_o_QWidget_insertAction(pWidget, pBefore, pAction);
    }
    if (pAction && !g_pReloadFgdAction) {
        CheckAndCacheReloadActionPtr(pAction);
    }
}

void __fastcall hk_QWidget_addAction(void* pWidget, void* pAction) {
    if (g_o_QWidget_addAction) {
        g_o_QWidget_addAction(pWidget, pAction);
    }
    if (pAction && !g_pReloadFgdAction) {
        CheckAndCacheReloadActionPtr(pAction);
    }
}

void __fastcall hk_QAction_activate(void* pAction, int event) {
    if (pAction && !g_pReloadFgdAction) {
        CheckAndCacheReloadActionPtr(pAction);
    }
    if (g_o_QAction_activate) {
        g_o_QAction_activate(pAction, event);
    }
}

void __fastcall hk_QTreeWidgetItem_setText(void* pItem, int column, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QTreeWidgetItem_setText) g_o_QTreeWidgetItem_setText(pItem, column, text);
    });
}

void __fastcall hk_QTreeWidget_setHeaderLabel(void* pTree, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QTreeWidget_setHeaderLabel) g_o_QTreeWidget_setHeaderLabel(pTree, text);
    });
}

void __fastcall hk_QTableWidgetItem_setText(void* pItem, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QTableWidgetItem_setText) g_o_QTableWidgetItem_setText(pItem, text);
    });
}

void __fastcall hk_QListWidgetItem_setText(void* pItem, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QListWidgetItem_setText) g_o_QListWidgetItem_setText(pItem, text);
    });
}

void __fastcall hk_QComboBox_addItem(void* pBox, const void* pQString, const void* pUserData) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QComboBox_addItem) g_o_QComboBox_addItem(pBox, text, pUserData);
    });
}

void __fastcall hk_QComboBox_addItem_icon(void* pBox, const void* pIcon, const void* pQString, const void* pUserData) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QComboBox_addItem_icon) g_o_QComboBox_addItem_icon(pBox, pIcon, text, pUserData);
    });
}

void __fastcall hk_QComboBox_setItemText(void* pBox, int index, const void* pQString) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QComboBox_setItemText) g_o_QComboBox_setItemText(pBox, index, text);
    });
}

void __fastcall hk_QComboBox_insertItem(void* pBox, int index, const void* pQString, const void* pUserData) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QComboBox_insertItem) g_o_QComboBox_insertItem(pBox, index, text, pUserData);
    });
}

void __fastcall hk_QComboBox_insertItem_icon(void* pBox, int index, const void* pIcon, const void* pQString, const void* pUserData) {
    void* caller = _ReturnAddress();
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        if (g_o_QComboBox_insertItem_icon) g_o_QComboBox_insertItem_icon(pBox, index, pIcon, text, pUserData);
    });
}

void* __fastcall hk_QTableWidgetItem_ctor(void* pItem, const void* pQString, int type) {
    void* caller = _ReturnAddress();
    void* result = nullptr;
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        result = g_o_QTableWidgetItem_ctor(pItem, text, type);
    });
    return result;
}

void* __fastcall hk_QListWidgetItem_ctor(void* pItem, const void* pQString, void* pListWidget, int type) {
    void* caller = _ReturnAddress();
    void* result = nullptr;
    ForwardTranslatedQString(caller, pQString, TranslationSource::ItemWidget, [&](const void* text) {
        result = g_o_QListWidgetItem_ctor(pItem, text, pListWidget, type);
    });
    return result;
}

inline bool SafeGetWidgetActions(void* pWidget, void**& outItems, int& outCount) {
    if (!pWidget || (uintptr_t)pWidget < 0x10000) return false;
    __try {
        void* d_ptr = *(void**)((char*)pWidget + 8);
        if (!d_ptr || (uintptr_t)d_ptr < 0x10000) return false;
        void* listData = *(void**)((char*)d_ptr + 0x190);
        if (!listData || (uintptr_t)listData < 0x10000) return false;
        int begin = *(int*)((char*)listData + 8);
        int end = *(int*)((char*)listData + 12);
        if (begin >= 0 && end > begin && (end - begin) < 5000) {
            outItems = (void**)((char*)listData + 16) + begin;
            outCount = end - begin;
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void* SafeFindWidget(fnQWidget_find pfn, uint64_t wid) {
    if (!pfn || !wid) return nullptr;
    __try {
        return pfn(wid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

inline bool SafeActivateAction(fnQAction_activate pfnActivate, void* pAction) {
    if (!pfnActivate || !pAction) return false;
    __try {
        pfnActivate(pAction, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ScanWindowTreeForReloadAction() {
    if (g_pReloadFgdAction) return;
    HMODULE hQtWidgets = GetModuleHandleW(L"Qt5Widgets.dll");
    if (!hQtWidgets) return;
    if (!g_pfn_QWidget_find) {
        g_pfn_QWidget_find = (fnQWidget_find)GetProcAddress(hQtWidgets, "?find@QWidget@@SAPEAV1@_K@Z");
    }
    if (!g_pfn_QWidget_find) return;

    SafeProbeScope probeScope;

    EnumWindows([](HWND hwnd, LPARAM) -> BOOL {
        if (g_pReloadFgdAction) return FALSE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId()) return TRUE;

        auto InspectWidget = [](void* pWidget) {
            if (!pWidget || g_pReloadFgdAction) return;
            void** items = nullptr;
            int count = 0;
            if (SafeGetWidgetActions(pWidget, items, count) && items && count > 0) {
                for (int i = 0; i < count; ++i) {
                    void* pAction = items[i];
                    if (pAction) {
                        CheckAndCacheReloadActionPtr(pAction);
                        if (g_pReloadFgdAction) return;
                    }
                }
            }
        };

        void* pTop = SafeFindWidget(g_pfn_QWidget_find, (uint64_t)hwnd);
        if (pTop) {
            InspectWidget(pTop);
        }

        EnumChildWindows(hwnd, [](HWND childHwnd, LPARAM) -> BOOL {
            if (g_pReloadFgdAction) return FALSE;
            void* pChild = SafeFindWidget(g_pfn_QWidget_find, (uint64_t)childHwnd);
            if (pChild) {
                void** items = nullptr;
                int count = 0;
                if (SafeGetWidgetActions(pChild, items, count) && items && count > 0) {
                    for (int i = 0; i < count; ++i) {
                        void* pAction = items[i];
                        if (pAction) {
                            CheckAndCacheReloadActionPtr(pAction);
                            if (g_pReloadFgdAction) return FALSE;
                        }
                    }
                }
            }
            return TRUE;
        }, 0);

        return TRUE;
    }, 0);
}

} // namespace

bool InitializeQtCoreHooks() {
    HMODULE hQtCore = GetModuleHandleW(L"Qt5Core.dll");
    if (!hQtCore) return false;

    g_pfn_fromUtf8 = (fnQString_fromUtf8)GetProcAddress(hQtCore, "?fromUtf8@QString@@SA?AV1@PEBDH@Z");
    g_pfn_utf16 = (fnQString_utf16)GetProcAddress(hQtCore, "?utf16@QString@@QEBAPEBGXZ");
    g_pfn_QString_dtor = (fnQString_dtor)GetProcAddress(hQtCore, "??1QString@@QEAA@XZ");
    g_pfn_QObject_inherits = (fnQObject_inherits)GetProcAddress(hQtCore, "?inherits@QObject@@QEBA_NPEBD@Z");

    PatchInfo patchInfo;
    if (PePatcher::GetPatchInfoFromMemory(hQtCore, patchInfo) && patchInfo.origTrRva != 0) {
        g_o_QMetaObject_tr = (fnQMetaObject_tr)((uint8_t*)hQtCore + patchInfo.origTrRva);
        shared_log::LogHook("[INIT] EAT Redirect: real QMetaObject::tr resolved from LCLZ header at %p", g_o_QMetaObject_tr);
    } else {
        void* pTr = (void*)GetProcAddress(hQtCore, "?tr@QMetaObject@@QEBA?AVQString@@PEBD0H@Z");
        if (pTr && g_pfn_fromUtf8 && !g_o_QMetaObject_tr) {
            bool ok = HookManager::Instance().InstallHook(pTr, (void*)hk_QMetaObject_tr, (void**)&g_o_QMetaObject_tr, "QMetaObject::tr");
            shared_log::LogHook("[INIT] Hooked QMetaObject::tr via MinHook, result=%d, orig=%p", ok, g_o_QMetaObject_tr);
        }
    }

    return true;
}

bool TryHookQtToolsModules() {
    if (!g_bWidgetsHooked.load()) {
        HMODULE hQtWidgets = GetModuleHandleW(L"Qt5Widgets.dll");
        if (hQtWidgets) {
            shared_log::LogHook("[HOOK] Found Qt5Widgets.dll (%p), installing widget hooks...", hQtWidgets);
            void* pActionCtorText  = (void*)GetProcAddress(hQtWidgets, "??0QAction@@QEAA@AEBVQString@@PEAVQObject@@@Z");
            void* pActionCtorIcon  = (void*)GetProcAddress(hQtWidgets, "??0QAction@@QEAA@AEBVQIcon@@AEBVQString@@PEAVQObject@@@Z");
            void* pActionSetText   = (void*)GetProcAddress(hQtWidgets, "?setText@QAction@@QEAAXAEBVQString@@@Z");
            void* pActionSetTip    = (void*)GetProcAddress(hQtWidgets, "?setToolTip@QAction@@QEAAXAEBVQString@@@Z");
            void* pActionSetStatus = (void*)GetProcAddress(hQtWidgets, "?setStatusTip@QAction@@QEAAXAEBVQString@@@Z");
            void* pActionSetWhats  = (void*)GetProcAddress(hQtWidgets, "?setWhatsThis@QAction@@QEAAXAEBVQString@@@Z");
            void* pButtonSetText   = (void*)GetProcAddress(hQtWidgets, "?setText@QAbstractButton@@QEAAXAEBVQString@@@Z");
            void* pButtonSetIcon   = (void*)GetProcAddress(hQtWidgets, "?setIcon@QAbstractButton@@QEAAXAEBVQIcon@@@Z");
            void* pToolButtonSetDefaultAction = (void*)GetProcAddress(hQtWidgets, "?setDefaultAction@QToolButton@@QEAAXPEAVQAction@@@Z");
            void* pLabelSetText    = (void*)GetProcAddress(hQtWidgets, "?setText@QLabel@@QEAAXAEBVQString@@@Z");
            void* pSetTitle        = (void*)GetProcAddress(hQtWidgets, "?setWindowTitle@QWidget@@QEAAXAEBVQString@@@Z");
            void* pGroupSetTitle   = (void*)GetProcAddress(hQtWidgets, "?setTitle@QGroupBox@@QEAAXAEBVQString@@@Z");

            if (!g_pfn_QToolButton_defaultAction) {
                g_pfn_QToolButton_defaultAction = (fnQToolButton_defaultAction)GetProcAddress(hQtWidgets, "?defaultAction@QToolButton@@QEBAPEAVQAction@@XZ");
            }
            if (!g_pfn_QAction_setIcon) {
                g_pfn_QAction_setIcon = (fnQAction_setIcon)GetProcAddress(hQtWidgets, "?setIcon@QAction@@QEAAXAEBVQIcon@@@Z");
            }

            void* pTreeSetText     = (void*)GetProcAddress(hQtWidgets, "?setText@QTreeWidgetItem@@QEAAXHAEBVQString@@@Z");
            void* pTreeSetHeader   = (void*)GetProcAddress(hQtWidgets, "?setHeaderLabel@QTreeWidget@@QEAAXAEBVQString@@@Z");
            void* pTableCtor       = (void*)GetProcAddress(hQtWidgets, "??0QTableWidgetItem@@QEAA@AEBVQString@@H@Z");
            void* pTableSetText    = (void*)GetProcAddress(hQtWidgets, "?setText@QTableWidgetItem@@QEAAXAEBVQString@@@Z");
            void* pListCtor        = (void*)GetProcAddress(hQtWidgets, "??0QListWidgetItem@@QEAA@AEBVQString@@PEAVQListWidget@@H@Z");
            void* pListSetText     = (void*)GetProcAddress(hQtWidgets, "?setText@QListWidgetItem@@QEAAXAEBVQString@@@Z");

            void* pComboAddItem    = (void*)GetProcAddress(hQtWidgets, "?addItem@QComboBox@@QEAAXAEBVQString@@AEBVQVariant@@@Z");
            void* pComboAddItemIcon= (void*)GetProcAddress(hQtWidgets, "?addItem@QComboBox@@QEAAXAEBVQIcon@@AEBVQString@@AEBVQVariant@@@Z");
            void* pComboSetItemText= (void*)GetProcAddress(hQtWidgets, "?setItemText@QComboBox@@QEAAXHAEBVQString@@@Z");
            void* pComboInsertItem = (void*)GetProcAddress(hQtWidgets, "?insertItem@QComboBox@@QEAAXHAEBVQString@@AEBVQVariant@@@Z");
            void* pComboInsertItemIcon = (void*)GetProcAddress(hQtWidgets, "?insertItem@QComboBox@@QEAAXHAEBVQIcon@@AEBVQString@@AEBVQVariant@@@Z");

            void* pInsertAction    = (void*)GetProcAddress(hQtWidgets, "?insertAction@QWidget@@QEAAXPEAVQAction@@0@Z");
            void* pAddAction       = (void*)GetProcAddress(hQtWidgets, "?addAction@QWidget@@QEAAXPEAVQAction@@@Z");
            void* pActivate        = (void*)GetProcAddress(hQtWidgets, "?activate@QAction@@QEAAXW4ActionEvent@1@@Z");

            std::vector<HookRequest> widgetRequests = {
                { pActionCtorText,      (void*)hk_QAction_ctor,             (void**)&g_o_QAction_ctor,             "QAction::QAction(text)" },
                { pActionCtorIcon,      (void*)hk_QAction_ctor_icon,        (void**)&g_o_QAction_ctor_icon,        "QAction::QAction(icon,text)" },
                { pActionSetText,       (void*)hk_QAction_setText,          (void**)&g_o_QAction_setText,          "QAction::setText" },
                { pActionSetTip,        (void*)hk_QAction_setToolTip,       (void**)&g_o_QAction_setToolTip,       "QAction::setToolTip" },
                { pActionSetStatus,     (void*)hk_QAction_setStatusTip,     (void**)&g_o_QAction_setStatusTip,     "QAction::setStatusTip" },
                { pActionSetWhats,      (void*)hk_QAction_setWhatsThis,     (void**)&g_o_QAction_setWhatsThis,     "QAction::setWhatsThis" },
                { pInsertAction,        (void*)hk_QWidget_insertAction,     (void**)&g_o_QWidget_insertAction,     "QWidget::insertAction" },
                { pAddAction,           (void*)hk_QWidget_addAction,        (void**)&g_o_QWidget_addAction,        "QWidget::addAction" },
                { pActivate,            (void*)hk_QAction_activate,         (void**)&g_o_QAction_activate,         "QAction::activate" },
                { pButtonSetText,       (void*)hk_QAbstractButton_setText,  (void**)&g_o_QAbstractButton_setText,  "QAbstractButton::setText" },
                { pButtonSetIcon,       (void*)hk_QAbstractButton_setIcon,  (void**)&g_o_QAbstractButton_setIcon,  "QAbstractButton::setIcon" },
                { pToolButtonSetDefaultAction, (void*)hk_QToolButton_setDefaultAction, (void**)&g_o_QToolButton_setDefaultAction, "QToolButton::setDefaultAction" },
                { pLabelSetText,        (void*)hk_QLabel_setText,           (void**)&g_o_QLabel_setText,           "QLabel::setText" },
                { pSetTitle,            (void*)hk_QWidget_setWindowTitle,   (void**)&g_o_QWidget_setWindowTitle,   "QWidget::setWindowTitle" },
                { pGroupSetTitle,       (void*)hk_QGroupBox_setTitle,       (void**)&g_o_QGroupBox_setTitle,       "QGroupBox::setTitle" },
                { pTreeSetText,         (void*)hk_QTreeWidgetItem_setText,  (void**)&g_o_QTreeWidgetItem_setText,  "QTreeWidgetItem::setText" },
                { pTreeSetHeader,       (void*)hk_QTreeWidget_setHeaderLabel, (void**)&g_o_QTreeWidget_setHeaderLabel, "QTreeWidget::setHeaderLabel" },
                { pTableCtor,           (void*)hk_QTableWidgetItem_ctor,    (void**)&g_o_QTableWidgetItem_ctor,    "QTableWidgetItem::QTableWidgetItem" },
                { pTableSetText,        (void*)hk_QTableWidgetItem_setText, (void**)&g_o_QTableWidgetItem_setText, "QTableWidgetItem::setText" },
                { pListCtor,            (void*)hk_QListWidgetItem_ctor,     (void**)&g_o_QListWidgetItem_ctor,     "QListWidgetItem::QListWidgetItem" },
                { pListSetText,         (void*)hk_QListWidgetItem_setText,  (void**)&g_o_QListWidgetItem_setText,  "QListWidgetItem::setText" },
                { pComboAddItem,        (void*)hk_QComboBox_addItem,        (void**)&g_o_QComboBox_addItem,        "QComboBox::addItem" },
                { pComboAddItemIcon,    (void*)hk_QComboBox_addItem_icon,   (void**)&g_o_QComboBox_addItem_icon,   "QComboBox::addItem(icon)" },
                { pComboSetItemText,    (void*)hk_QComboBox_setItemText,    (void**)&g_o_QComboBox_setItemText,    "QComboBox::setItemText" },
                { pComboInsertItem,     (void*)hk_QComboBox_insertItem,     (void**)&g_o_QComboBox_insertItem,     "QComboBox::insertItem" },
                { pComboInsertItemIcon, (void*)hk_QComboBox_insertItem_icon, (void**)&g_o_QComboBox_insertItem_icon, "QComboBox::insertItem(icon)" }
            };

            if (InstallHookBatch(widgetRequests, "Qt5Widgets")) {
                g_bWidgetsHooked.store(true);
                shared_log::LogHook("[HOOK] Qt5Widgets hooks installed successfully (all %zu/%zu OK)", widgetRequests.size(), widgetRequests.size());
            }
        }
    }

    if (!g_bGuiHooked.load()) {
        HMODULE hQtGui = GetModuleHandleW(L"Qt5Gui.dll");
        if (hQtGui) {
            shared_log::LogHook("[HOOK] Found Qt5Gui.dll (%p), installing GUI hooks...", hQtGui);
            void* pDrawRect       = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRect@@HAEBVQString@@PEAV2@@Z");
            void* pDrawRectF      = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRectF@@HAEBVQString@@PEAV2@@Z");
            void* pDrawPointF     = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQPointF@@AEBVQString@@@Z");
            void* pDrawRectFOpt   = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQRectF@@AEBVQString@@AEBVQTextOption@@@Z");
            void* pDrawPoint      = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXAEBVQPoint@@AEBVQString@@@Z");
            void* pDrawXY         = (void*)GetProcAddress(hQtGui, "?drawText@QPainter@@QEAAXHHAEBVQString@@@Z");

            void* pDocSetPlainText= (void*)GetProcAddress(hQtGui, "?setPlainText@QTextDocument@@QEAAXAEBVQString@@@Z");
            void* pDocSetHtml     = (void*)GetProcAddress(hQtGui, "?setHtml@QTextDocument@@QEAAXAEBVQString@@@Z");

            std::vector<HookRequest> guiRequests = {
                { pDrawRect,        (void*)hk_QPainter_drawText_Rect,           (void**)&g_o_QPainter_drawText_Rect,           "QPainter::drawText(Rect)" },
                { pDrawRectF,       (void*)hk_QPainter_drawText_RectF,          (void**)&g_o_QPainter_drawText_RectF,          "QPainter::drawText(RectF)" },
                { pDrawPointF,      (void*)hk_QPainter_drawText_PointF,         (void**)&g_o_QPainter_drawText_PointF,         "QPainter::drawText(PointF)" },
                { pDrawRectFOpt,    (void*)hk_QPainter_drawText_RectF_Option,   (void**)&g_o_QPainter_drawText_RectF_Option,   "QPainter::drawText(RectF,Option)" },
                { pDrawPoint,       (void*)hk_QPainter_drawText_Point,          (void**)&g_o_QPainter_drawText_Point,          "QPainter::drawText(Point)" },
                { pDrawXY,          (void*)hk_QPainter_drawText_xy,             (void**)&g_o_QPainter_drawText_xy,             "QPainter::drawText(xy)" },
                { pDocSetPlainText, (void*)hk_QTextDocument_setPlainText,       (void**)&g_o_QTextDocument_setPlainText,       "QTextDocument::setPlainText" },
                { pDocSetHtml,      (void*)hk_QTextDocument_setHtml,            (void**)&g_o_QTextDocument_setHtml,            "QTextDocument::setHtml" }
            };

            if (InstallHookBatch(guiRequests, "Qt5Gui")) {
                g_bGuiHooked.store(true);
                shared_log::LogHook("[HOOK] Qt5Gui hooks installed successfully (all %zu/%zu OK)", guiRequests.size(), guiRequests.size());
            }
        }
    }

    return (g_bWidgetsHooked.load() && g_bGuiHooked.load());
}

bool AreWidgetsHooked() {
    return g_bWidgetsHooked.load(std::memory_order_relaxed);
}

bool AreGuiHooked() {
    return g_bGuiHooked.load(std::memory_order_relaxed);
}

void TriggerReloadFgdAction() {
    void* pAction = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_ReloadActionMutex);
        pAction = g_pReloadFgdAction;
    }
    if (!pAction) {
        ScanWindowTreeForReloadAction();
        std::lock_guard<std::mutex> lock(g_ReloadActionMutex);
        pAction = g_pReloadFgdAction;
    }
    if (pAction) {
        HMODULE hQtWidgets = GetModuleHandleW(L"Qt5Widgets.dll");
        if (hQtWidgets && !g_pfn_QAction_activate) {
            g_pfn_QAction_activate = (fnQAction_activate)GetProcAddress(hQtWidgets, "?activate@QAction@@QEAAXW4ActionEvent@1@@Z");
        }
        fnQAction_activate pfnActivate = g_o_QAction_activate ? g_o_QAction_activate : g_pfn_QAction_activate;
        if (pfnActivate) {
            shared_log::LogHook("[FGD] Automatically triggering QAction 'Reload .FGD Files' at %p", pAction);
            SafeProbeScope probeScope;
            if (SafeActivateAction(pfnActivate, pAction)) {
                shared_log::LogHook("[FGD] Successfully triggered 'Reload .FGD Files' QAction");
            } else {
                shared_log::LogHook("[FGD] Exception caught while activating 'Reload .FGD Files' QAction");
            }
        }
    } else {
        shared_log::LogHook("[FGD] Reload .FGD Files action not cached yet");
    }
}

extern "C" __declspec(dllexport) void* __fastcall hk_QMetaObject_tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n) {
    void* caller = _ReturnAddress();
    InitializeTranslator();
    if (sourceText && g_pfn_fromUtf8 && pOutQString) {
        std::string trans;
        if (FindTranslationScoped(caller, sourceText, trans)) {
            if (SafeCreateQString(g_pfn_fromUtf8, pOutQString, trans.c_str(), (int)trans.length())) {
                return pOutQString;
            }
        }
    }
    if (g_o_QMetaObject_tr) {
        return g_o_QMetaObject_tr(pMetaObject, pOutQString, sourceText, disambiguation, n);
    }
    return nullptr;
}

extern "C" __declspec(dllexport) void* __fastcall tr(const void* pMetaObject, void* pOutQString, const char* sourceText, const char* disambiguation, int n) {
    return hk_QMetaObject_tr(pMetaObject, pOutQString, sourceText, disambiguation, n);
}

} // namespace qm
