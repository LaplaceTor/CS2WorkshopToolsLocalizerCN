#include "core/fgd_translator.h"
#include "core/dictionary_compiler.h"
#include "core/backup_manager.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <regex>
#include <filesystem>
#include <optional>
#include <iostream>
#include <memory>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QFile>
#include <QSaveFile>
#include <QString>

namespace fs = std::filesystem;

// ==============================================================================
// 字典加载与解析（使用 Qt 原生 QJsonDocument 解析）
// ==============================================================================

bool FgdTranslator::LoadDictionary(const std::wstring& jsonPath, std::unordered_map<std::string, std::string>& outDict, const std::wstring& fallbackJsonPath) {
    outDict.clear();

    auto loadSingleJson = [&](const std::wstring& path) -> bool {
        if (path.empty()) return false;
        QFile file(QString::fromStdWString(path));
        if (!file.open(QIODevice::ReadOnly)) return false;
        QByteArray rawData = file.readAll();
        file.close();

        std::string clean = DictionaryCompiler::StripJsonComments(rawData.constData(), rawData.size());
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(clean.c_str(), clean.size()), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            return false;
        }

        QJsonObject obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.value().isString()) {
                QString val = it.value().toString().trimmed();
                if (!val.isEmpty()) {
                    outDict[it.key().toStdString()] = val.toStdString();
                }
            }
        }
        return true;
    };

    // 1. 若提供了 fallback 字典，先载入作为兜底
    if (!fallbackJsonPath.empty() && fs::exists(fallbackJsonPath)) {
        loadSingleJson(fallbackJsonPath);
    }

    // 2. 载入主字典，覆盖/补充 fallback
    loadSingleJson(jsonPath);

    return !outDict.empty();
}

bool FgdTranslator::LoadOverrideDictionary(const std::wstring& jsonPath, FgdOverrideData& outOverride) {
    outOverride = FgdOverrideData();
    QFile file(QString::fromStdWString(jsonPath));
    if (!file.open(QIODevice::ReadOnly)) return false;
    QByteArray rawData = file.readAll();
    file.close();

    std::string clean = DictionaryCompiler::StripJsonComments(rawData.constData(), rawData.size());
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromRawData(clean.c_str(), clean.size()), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    QJsonObject root = doc.object();

    auto parsePropOverride = [](const QJsonValue& val, FgdPropertyOverride& propOut) {
        if (val.isString()) {
            propOut.description = val.toString().toStdString();
        } else if (val.isObject()) {
            QJsonObject o = val.toObject();
            if (o.contains("description") && o["description"].isString()) {
                propOut.description = o["description"].toString().toStdString();
            }
            if (o.contains("displayName") && o["displayName"].isString()) {
                propOut.displayName = o["displayName"].toString().toStdString();
            }
        }
    };

    for (auto it = root.begin(); it != root.end(); ++it) {
        QString key = it.key();
        const QJsonValue& val = it.value();

        if (key == "properties" && val.isObject()) {
            QJsonObject pObj = val.toObject();
            for (auto pit = pObj.begin(); pit != pObj.end(); ++pit) {
                FgdPropertyOverride prop;
                parsePropOverride(pit.value(), prop);
                if (!prop.description.empty() || !prop.displayName.empty()) {
                    outOverride.globalProperties[pit.key().toStdString()] = std::move(prop);
                }
            }
        } else if (key == "io" && val.isObject()) {
            QJsonObject ioObj = val.toObject();
            for (auto ioit = ioObj.begin(); ioit != ioObj.end(); ++ioit) {
                if (ioit.value().isString()) {
                    outOverride.ioOverrides[ioit.key().toStdString()] = ioit.value().toString().toStdString();
                } else if (ioit.value().isObject()) {
                    QJsonObject o = ioit.value().toObject();
                    if (o.contains("description") && o["description"].isString()) {
                        outOverride.ioOverrides[ioit.key().toStdString()] = o["description"].toString().toStdString();
                    }
                }
            }
        } else if (key == "classes" && val.isObject()) {
            QJsonObject cObj = val.toObject();
            for (auto cit = cObj.begin(); cit != cObj.end(); ++cit) {
                std::string clsName = cit.key().toStdString();
                if (cit.value().isString()) {
                    outOverride.classDescriptions[clsName] = cit.value().toString().toStdString();
                } else if (cit.value().isObject()) {
                    QJsonObject o = cit.value().toObject();
                    if (o.contains("description") && o["description"].isString()) {
                        outOverride.classDescriptions[clsName] = o["description"].toString().toStdString();
                    }
                    if (o.contains("properties") && o["properties"].isObject()) {
                        QJsonObject cpObj = o["properties"].toObject();
                        for (auto cpit = cpObj.begin(); cpit != cpObj.end(); ++cpit) {
                            FgdPropertyOverride prop;
                            parsePropOverride(cpit.value(), prop);
                            if (!prop.description.empty() || !prop.displayName.empty()) {
                                outOverride.classProperties[clsName][cpit.key().toStdString()] = std::move(prop);
                            }
                        }
                    }
                }
            }
        } else if (!key.startsWith("_")) {
            if (val.isString()) {
                FgdPropertyOverride prop;
                prop.description = val.toString().toStdString();
                outOverride.globalProperties[key.toStdString()] = std::move(prop);
            } else if (val.isObject()) {
                QJsonObject o = val.toObject();
                if (o.contains("properties") && o["properties"].isObject()) {
                    if (o.contains("description") && o["description"].isString()) {
                        outOverride.classDescriptions[key.toStdString()] = o["description"].toString().toStdString();
                    }
                    QJsonObject cpObj = o["properties"].toObject();
                    for (auto cpit = cpObj.begin(); cpit != cpObj.end(); ++cpit) {
                        FgdPropertyOverride prop;
                        parsePropOverride(cpit.value(), prop);
                        if (!prop.description.empty() || !prop.displayName.empty()) {
                            outOverride.classProperties[key.toStdString()][cpit.key().toStdString()] = std::move(prop);
                        }
                    }
                } else {
                    FgdPropertyOverride prop;
                    parsePropOverride(val, prop);
                    if (!prop.description.empty() || !prop.displayName.empty()) {
                        outOverride.globalProperties[key.toStdString()] = std::move(prop);
                    }
                }
            }
        }
    }

    return !outOverride.empty();
}

std::string FgdTranslator::TranslateLine(const std::string& line, const std::unordered_map<std::string, std::string>& dict) {
    return FgdCore::TranslateLine(line, dict);
}

std::string FgdTranslator::TranslateLine(
    const std::string& line,
    const std::unordered_map<std::string, std::string>& dict,
    const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass
) {
    return FgdCore::TranslateLine(line, dict, overrideData, inOutCurrentClass);
}

std::string FgdTranslator::TranslateLine(
    const std::string& line,
    const std::unordered_map<std::string, std::string>& dict,
    const FgdOverrideData& overrideData,
    std::string& inOutCurrentClass,
    std::string& inOutPendingClassDesc
) {
    return FgdCore::TranslateLine(line, dict, overrideData, inOutCurrentClass, inOutPendingClassDesc);
}

// ==============================================================================
// FGD 文件批量与单文件翻译
// ==============================================================================

bool FgdTranslator::TranslateFile(
    const std::wstring& srcPath,
    const std::wstring& dstPath,
    const std::unordered_map<std::string, std::string>& dict,
    const FgdOverrideData& overrideData
) {
    std::ifstream inFile(srcPath, std::ios::binary);
    if (!inFile.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
    inFile.close();

    std::string translated;
    if (!FgdCore::TranslateContent(content, translated, dict, overrideData)) {
        return false;
    }

    fs::path outDir = fs::path(dstPath).parent_path();
    if (!outDir.empty()) {
        fs::create_directories(outDir);
    }

    // QSaveFile 原子写入：先写临时文件，commit 时整体替换，避免中断产生半写文件
    QSaveFile outFile(QString::fromStdWString(dstPath));
    if (!outFile.open(QIODevice::WriteOnly)) {
        return false;
    }

    if (outFile.write(translated.data(), static_cast<qint64>(translated.size())) < 0) {
        outFile.cancelWriting();
        return false;
    }

    if (!outFile.commit()) {
        return false;
    }
    return true;
}

bool FgdTranslator::TranslateAndDeployAll(
    const std::wstring& cs2Root,
    const std::wstring& backupDir,
    const std::wstring& translationsDir,
    const std::wstring& jsonDictPath,
    std::vector<std::wstring>& outProcessedFiles,
    std::wstring& outError
) {
    return TranslateAndDeployAll(cs2Root, backupDir, translationsDir, jsonDictPath, L"", outProcessedFiles, outError);
}

bool FgdTranslator::TranslateAndDeployAll(
    const std::wstring& cs2Root,
    const std::wstring& backupDir,
    const std::wstring& translationsDir,
    const std::wstring& jsonDictPath,
    const std::wstring& jsonOverridePath,
    std::vector<std::wstring>& outProcessedFiles,
    std::wstring& outError,
    const std::wstring& jsonFallbackPath
) {
    std::unordered_map<std::string, std::string> dict;
    if (!LoadDictionary(jsonDictPath, dict, jsonFallbackPath)) {
        outError = L"无法加载 FGD 翻译字典: " + jsonDictPath;
        return false;
    }

    FgdOverrideData overrideData;
    if (!jsonOverridePath.empty() && fs::exists(jsonOverridePath)) {
        LoadOverrideDictionary(jsonOverridePath, overrideData);
    }

    fs::path backupRoot(backupDir);
    if (!fs::exists(backupRoot)) {
        outError = L"找不到 backup 目录: " + backupDir;
        return false;
    }

    std::vector<std::wstring> failedFiles;
    try {
        for (const auto& entry : fs::recursive_directory_iterator(backupRoot)) {
            if (entry.is_regular_file() && entry.path().extension() == L".fgd") {
                fs::path relPath = fs::relative(entry.path(), backupRoot);
                fs::path transDst = fs::path(translationsDir) / relPath;
                fs::path cs2Dst = fs::path(cs2Root) / relPath;

                // 翻译至 translationsDir；单个文件失败记录后继续，最后统一汇总
                if (!TranslateFile(entry.path().wstring(), transDst.wstring(), dict, overrideData)) {
                    failedFiles.push_back(relPath.wstring());
                    continue;
                }

                // 覆盖复制到 CS2 对应目录（先拷贝到临时名再整体替换，避免半写；被占用时自动重试）
                fs::create_directories(cs2Dst.parent_path());
                fs::path cs2Tmp = cs2Dst;
                cs2Tmp += L".tmp";
                if (!BackupManager::SafeCopyFileWithRetry(transDst, cs2Tmp)) {
                    failedFiles.push_back(relPath.wstring());
                    continue;
                }
                std::error_code replaceEc;
                fs::rename(cs2Tmp, cs2Dst, replaceEc);
                if (replaceEc) {
                    fs::remove(cs2Tmp, replaceEc);
                    failedFiles.push_back(relPath.wstring());
                    continue;
                }

                outProcessedFiles.push_back(relPath.wstring());
            }
        }

        if (!failedFiles.empty()) {
            std::wstring failedList;
            for (const auto& f : failedFiles) {
                failedList += L"\n  - " + f;
            }
            outError = L"有 " + std::to_wstring(failedFiles.size()) + L" 个 FGD 文件处理失败:" + failedList;
            return false;
        }
    } catch (const std::exception& e) {
        outError = L"处理 FGD 异常: " + QString::fromUtf8(e.what()).toStdWString();
        return false;
    }

    if (outProcessedFiles.empty()) {
        outError = L"backup 目录中未找到任何 FGD 文件: " + backupDir;
        return false;
    }
    return true;
}

// ==============================================================================
// 确保字典文件存在 (模板自动生成)
// ==============================================================================

bool FgdTranslator::EnsureFgdDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice) {
    fs::path p(jsonPath);
    if (fs::exists(p)) {
        return false;
    }

    std::unordered_map<std::string, std::string> loaded;
    if (!fallbackCachePath.empty() && fs::exists(fallbackCachePath)) {
        LoadDictionary(fallbackCachePath, loaded);
    }

    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    // 数据体由 QJsonDocument 组装输出，转义交给 Qt 处理
    QJsonObject entries;
    if (loaded.empty()) {
        entries["Omnidirectional point light"] = "全向点光源";
        entries["Light Source"] = "光源";
        entries["Name"] = "名称";
        entries["The name that other entities use to refer to this entity."] = "其他实体用于引用此实体的名称。";
        entries["Removes this entity from the world."] = "从世界中移除此实体。";
        entries["Enabled"] = "已启用";
        entries["Disabled"] = "已禁用";
    } else {
        for (const auto& kv : loaded) {
            if (kv.first.rfind("_说明", 0) == 0) continue;
            entries[QString::fromStdString(kv.first)] = QString::fromStdString(kv.second);
        }
    }

    QSaveFile out(QString::fromStdWString(jsonPath));
    if (!out.open(QIODevice::WriteOnly)) {
        outNotice = L"无法创建 FGD 翻译字典文件: " + jsonPath;
        return false;
    }

    // 注释版使用指南作为字面量直接写出（QJsonDocument 仅负责数据体）
    static const char kGuideHeader[] = R"(// ==============================================================================
// CS2 Hammer FGD 实体定义翻译字典 (JSONC 格式)
// ==============================================================================
//
// 【使用指南】
// - 格式为标准的键值对："英文原词": "中文翻译"
// - 支持 // 单行注释 与 /* 块注释 */
//
// 【可翻译内容】
// 1. 实体类说明 (@PointClass ... = name : "Description")
// 2. 属性显示名称 (targetname : "Name" : : "...")
// 3. 属性悬停描述 (... : "Name" : default : "Description")
// 4. 选项与标记 ("0" : "Enabled" : "Option Desc")
// 5. 输入输出 (input Kill : "Description")
// 6. 绑定按钮说明 (desc = "Description")
//
// 【格式与安全】
// - 所有底层 RAW 标识符（如 targetname、angles、thinkalways、io 类型与默认值）引擎会自动保护，请仅翻译双引号内的文本。
// - 修改保存后重新在启动器点击启动即可自动重新编译部署。
// ==============================================================================
)";
    out.write(kGuideHeader);
    out.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));

    if (!out.commit()) {
        outNotice = L"写入 FGD 翻译字典文件失败: " + jsonPath;
        return false;
    }

    outNotice = L"已自动生成 fgd_translations.jsonc 模板字典（包含详细使用说明与格式示例）。";
    return true;
}

bool FgdTranslator::EnsureFgdOverrideDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice) {
    fs::path p(jsonPath);
    if (fs::exists(p)) {
        return false;
    }

    FgdOverrideData loaded;
    if (!fallbackCachePath.empty() && fs::exists(fallbackCachePath)) {
        LoadOverrideDictionary(fallbackCachePath, loaded);
    }

    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    // 示例数据由 QJsonDocument 组装输出
    QJsonObject properties;
    properties["bodygroups"] = "设置模型的子部件与可选身体部件网格组合。";
    properties["vscripts"] = "实体生成后自动加载并执行的 VScript 脚本文件列表。";
    properties["clientSideEntity"] = "是否仅在客户端创建并运行此实体（不向服务器同步）。";
    properties["TeamNum"] = "所属队伍编号（0: 任意/无队伍, 2: T 阵营, 3: CT 阵营）。";
    properties["box_mins"] = "包围盒/光照探针体积的最小边界坐标 (X Y Z)。";
    properties["box_maxs"] = "包围盒/光照探针体积的最大边界坐标 (X Y Z)。";
    properties["flood_fill"] = "忽略玩家不可达的空间，加快光照烘焙速度并节省显存。";
    properties["voxelize"] = "忽略已体素化的实体空间，优化光照探针计算。";
    properties["light_probe_volume_from_cubemap"] = "是否使用立方体贴图 (Cubemap) 计算漫反射光照探针。";
    properties["moveable"] = "是否允许在游戏运行时移动、绑定父级、启用或禁用此对象。";
    properties["edge_fade_dist"] = "反射或光照边界平滑淡出过渡距离。";
    properties["max_lightmap_resolution"] = "限制此对象在烘焙时的最大光照贴图分辨率（0 为默认）。";

    QJsonObject io;
    io["SetParent"] = "设置该实体的父级对象。";
    io["ClearParent"] = "解除与父级实体的挂载绑定关系，使其独立运动。";
    io["FollowEntity"] = "骨骼合并 (Bone Merge) 附加到目标实体。";
    io["Kill"] = "从世界中移除此实体并释放资源。";
    io["SetHealth"] = "设置该实体的当前生命值。";

    QJsonObject envCubemapProps;
    envCubemapProps["influenceradius"] = "当前立方体贴图的生效影响半径（单位：英寸）。";

    QJsonObject envCubemap;
    envCubemap["description"] = "用于采样环境间接镜面反射的高动态范围立方体贴图实体。";
    envCubemap["properties"] = envCubemapProps;

    QJsonObject classes;
    classes["info_node"] = "AI 地面导航节点，供 NPC 寻路与路径规划计算使用。";
    classes["csm_fov_override"] = "级联阴影贴图 (CSM) 视场角覆盖控制器。";
    classes["env_cubemap"] = envCubemap;

    QJsonObject root;
    root["properties"] = properties;
    root["io"] = io;
    root["classes"] = classes;

    QSaveFile out(QString::fromStdWString(jsonPath));
    if (!out.open(QIODevice::WriteOnly)) {
        outNotice = L"无法创建 FGD 覆盖字典文件: " + jsonPath;
        return false;
    }

    // 注释版使用指南作为字面量直接写出（QJsonDocument 仅负责数据体）
    static const char kGuideHeader[] = R"(// ==============================================================================
// CS2 FGD 实体键值描述补充与覆盖字典 (JSONC 格式)
// ==============================================================================
//
// 【作用说明】
// - 本文件用于针对 FGD 中特定的【属性名 (Key)】、【实体类名 (Class)】或【输入输出 (I/O)】
//   补充缺失的说明描述，或覆盖原版已有描述。
// - 与 fgd_translations.jsonc 互为补充：
//   * fgd_translations.jsonc: 负责已有英文字符串 -> 中文翻译。
//   * fgd_override.jsonc: 负责针对特定键名无描述时【自动新增描述】或【强制覆盖描述】。
//
// 【支持格式】
// 1. 全局属性描述补充 (properties): "属性键名": "描述文本" 或 "属性键名": { "description": "...", "displayName": "..." }
// 2. 输入输出说明补充 (io): "IOName": "说明文本"
// 3. 实体类说明补充 (classes): "classname": "说明文本" 或 "classname": { "description": "...", "properties": { ... } }
// 4. 顶层快速简写: "键名": "描述文本"
// ==============================================================================

)";
    out.write(kGuideHeader);
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    if (!out.commit()) {
        outNotice = L"写入 FGD 覆盖字典文件失败: " + jsonPath;
        return false;
    }

    outNotice = L"已自动生成 fgd_override.jsonc 模板字典（包含详细使用说明与格式示例）。";
    return true;
}

bool FgdTranslator::EnsureQtDictionaryExists(const std::wstring& jsonPath, const std::wstring& fallbackCachePath, std::wstring& outNotice) {
    fs::path p(jsonPath);
    if (fs::exists(p)) {
        return false;
    }

    std::unordered_map<std::string, std::string> loaded;
    if (!fallbackCachePath.empty() && fs::exists(fallbackCachePath)) {
        LoadDictionary(fallbackCachePath, loaded);
    }

    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    // 数据体由 QJsonDocument 组装输出，转义交给 Qt 处理
    QJsonObject entries;
    if (loaded.empty()) {
        entries["File"] = "文件";
        entries["Edit"] = "编辑";
        entries["View"] = "视图";
        entries["Tools"] = "工具";
        entries["New"] = "新建";
        entries["Open"] = "打开";
        entries["Save"] = "保存";
        entries["Save As..."] = "另存为...";
        entries["Close"] = "关闭";
        entries["Exit"] = "退出";
        entries["Undo"] = "撤销";
        entries["Redo"] = "重做";
        entries["Clipping Tool"] = "剪切工具";
        entries["Transform Locked"] = "变换锁定";
        entries["Pinned To"] = "固定至";
        entries["Force Hidden"] = "强制隐藏";
    } else {
        for (const auto& kv : loaded) {
            if (kv.first.rfind("_说明", 0) == 0) continue;
            entries[QString::fromStdString(kv.first)] = QString::fromStdString(kv.second);
        }
    }

    QSaveFile out(QString::fromStdWString(jsonPath));
    if (!out.open(QIODevice::WriteOnly)) {
        outNotice = L"无法创建 Qt 界面翻译字典文件: " + jsonPath;
        return false;
    }

    // 注释版使用指南作为字面量直接写出（QJsonDocument 仅负责数据体）
    static const char kGuideHeader[] = R"(// ==============================================================================
// CS2 Hammer 界面与菜单核心翻译字典 (JSONC 格式)
// ==============================================================================
//
// 【使用指南】
// - 格式为标准的键值对："英文原词": "中文翻译"
// - 支持 // 单行注释 与 /* 块注释 */
//
// 【可翻译内容】
// 1. 主菜单与二级菜单项
// 2. 工具栏按钮与悬停提示
// 3. 属性面板属性名
// 4. 树形视图、列表与下拉框文本
// 5. 弹窗对话框与按钮文本
//
// 【快捷键自动适配】
// - 核心注入模块已内置动态快捷键识别与拆分引擎。
// - 遇到如 'Clipping Tool [Shift+X]'、'Undo (Ctrl+Z)'、'Save\tCtrl+S'、'Save As...'、'Name:' 等文本，
//   只需翻译基础英文（如 "Clipping Tool": "剪切工具"），快捷键后缀会被自动保留与拼接，无需手动输入快捷键！
// ==============================================================================
)";
    out.write(kGuideHeader);
    out.write(QJsonDocument(entries).toJson(QJsonDocument::Indented));

    if (!out.commit()) {
        outNotice = L"写入 Qt 界面翻译字典文件失败: " + jsonPath;
        return false;
    }

    outNotice = L"已自动生成 qt_translations.jsonc 模板字典（包含详细使用说明与格式示例）。";
    return true;
}
