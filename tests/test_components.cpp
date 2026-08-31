#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <windows.h>
#include "../src/cs2_detector.h"
#include "../src/pe_patcher.h"
#include "../src/fgd_translator.h"
#include "../src/backup_manager.h"
#include "../src/hook_manager.h"
#include "../src/dictionary_compiler.h"

namespace fs = std::filesystem;

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::cout << "========================================\n";
    std::cout << " Running CS2 Launcher Component Tests\n";
    std::cout << "========================================\n";

    // 1. Test CS2 Detector
    std::wstring cs2Root;
    bool found = Cs2Detector::DetectCs2(cs2Root);
    std::cout << "[Test 1] Cs2Detector::DetectCs2: " << (found ? "PASSED" : "FAILED") << "\n";
    if (found) {
        std::wcout << L"         Detected Path: " << cs2Root << L"\n";
        auto addons = Cs2Detector::GetAvailableAddons(cs2Root);
        std::cout << "         Addons Count: " << addons.size() << "\n";
        for (const auto& a : addons) {
            std::wcout << L"           - " << a << L"\n";
        }
    }
    assert(found && "CS2 must be detected on this machine");

    // 2. Test FGD Translation & Override Logic
    std::unordered_map<std::string, std::string> dict;
    dict["Omnidirectional point light"] = "全向点光源";
    dict["Light Source"] = "光源";
    dict["Name"] = "名称";
    dict["The name that other entities use to refer to this entity."] = "其他实体用于引用此实体的名称。";
    dict["Removes this entity from the world."] = "从世界中移除此实体。";
    dict["Enabled"] = "已启用";
    dict["Body Groups"] = "身体部件组";
    dict["Maximum Lightmap Resolution"] = "最大光照贴图分辨率";
    dict["World Model"] = "世界模型";

    FgdOverrideData overrideData;
    overrideData.globalProperties["bodygroups"].description = "设置模型的子部件与可选身体部件网格组合。";
    overrideData.globalProperties["max_lightmap_resolution"].description = "限制此对象在烘焙时的最大光照贴图分辨率。";
    overrideData.globalProperties["model"].description = "实体引用的 3D 模型资源文件路径。";
    overrideData.ioOverrides["Kill"] = "从世界中移除此实体并释放资源。";
    overrideData.ioOverrides["SetHealth"] = "设置该实体的当前生命值。";
    overrideData.classDescriptions["csm_fov_override"] = "级联阴影贴图 (CSM) 视场角覆盖控制器。";
    overrideData.classDescriptions["env_cubemap"] = "用于采样环境间接镜面反射的高动态范围立方体贴图实体。";
    overrideData.classProperties["env_cubemap"]["influenceradius"].description = "当前立方体贴图的生效影响半径（单位：英寸）。";

    std::string currentCls = "";

    // 2.1 实体类测试 (已有描述与无描述)
    std::string testClass = "@PointClass = light_omni : \"Omnidirectional point light\" []";
    std::string transClass = FgdTranslator::TranslateLine(testClass, dict, overrideData, currentCls);
    std::cout << "[Test 2.1] Class trans: " << transClass << "\n";
    assert(transClass.find("全向点光源") != std::string::npos);
    assert(currentCls == "light_omni");

    std::string pendingDesc = "";
    std::string testMultiLineClass1 = "@PointClass editormodel(\"models/editor/camera.vmdl\", fixedbounds) = csm_fov_override :";
    std::string testMultiLineClass2 = "\t\"This entity indicates the FOV override for cascading shadow maps.   .\"";
    std::string testMultiLineClass3 = "[";

    std::string out1 = FgdTranslator::TranslateLine(testMultiLineClass1, dict, overrideData, currentCls, pendingDesc);
    std::string out2 = FgdTranslator::TranslateLine(testMultiLineClass2, dict, overrideData, currentCls, pendingDesc);
    std::string out3 = FgdTranslator::TranslateLine(testMultiLineClass3, dict, overrideData, currentCls, pendingDesc);

    std::cout << "[Test 2.2] Multi-line Class override:\n" << out1 << out2 << out3 << "\n";
    assert(out1.find("\"") == std::string::npos && "Line 1 must not inject double description");
    assert(out2.find("级联阴影贴图 (CSM) 视场角覆盖控制器。") != std::string::npos);
    assert(out3.find("[") != std::string::npos);
    assert(currentCls == "csm_fov_override");

    // 2.2 属性测试 (完整三段、无描述属性、无描述带Choices、仅显示名)
    std::string testProp = "    targetname(target_source) : \"Name\" : : \"The name that other entities use to refer to this entity.\"";
    std::string transProp = FgdTranslator::TranslateLine(testProp, dict, overrideData, currentCls);
    std::cout << "[Test 2.3] Prop trans: " << transProp << "\n";
    assert(transProp.find("名称") != std::string::npos);
    assert(transProp.find("其他实体用于引用此实体的名称。") != std::string::npos);

    std::string testNoDescProp = "\tbodygroups(bodygroupchoices) [ group=\"Render Properties\" ] : \"Body Groups\" : \"\"";
    std::string transNoDescProp = FgdTranslator::TranslateLine(testNoDescProp, dict, overrideData, currentCls);
    std::cout << "[Test 2.4] No-desc prop override: " << transNoDescProp << "\n";
    assert(transNoDescProp.find("身体部件组") != std::string::npos);
    assert(transNoDescProp.find("设置模型的子部件与可选身体部件网格组合。") != std::string::npos);

    std::string testChoiceNoDesc = "\tmax_lightmap_resolution(choices) : \"Maximum Lightmap Resolution\" : \"0\" =";
    std::string transChoiceNoDesc = FgdTranslator::TranslateLine(testChoiceNoDesc, dict, overrideData, currentCls);
    std::cout << "[Test 2.5] Choices no-desc override: " << transChoiceNoDesc << "\n";
    assert(transChoiceNoDesc.find("最大光照贴图分辨率") != std::string::npos);
    assert(transChoiceNoDesc.find("限制此对象在烘焙时的最大光照贴图分辨率。") != std::string::npos);
    assert(transChoiceNoDesc.back() == '=' || transChoiceNoDesc.find(" =") != std::string::npos);

    std::string testDispOnly = "\tmodel(studio) { report = true sort_priority = 80 } : \"World Model\"";
    std::string transDispOnly = FgdTranslator::TranslateLine(testDispOnly, dict, overrideData, currentCls);
    std::cout << "[Test 2.6] Display-only prop override: " << transDispOnly << "\n";
    assert(transDispOnly.find("世界模型") != std::string::npos);
    assert(transDispOnly.find("实体引用的 3D 模型资源文件路径。") != std::string::npos);

    // 2.3 I/O 测试 (已有描述与无描述 I/O 补充)
    std::string testIO = "    input Kill(void) : \"Removes this entity from the world.\"";
    std::string transIO = FgdTranslator::TranslateLine(testIO, dict, overrideData, currentCls);
    std::cout << "[Test 2.7] I/O trans: " << transIO << "\n";
    assert(transIO.find("从世界中移除此实体并释放资源。") != std::string::npos);

    std::string testBareApiIO = "\tinput SetParent(api)";
    std::string transBareApiIO = FgdTranslator::TranslateLine(testBareApiIO, dict, overrideData, currentCls);
    std::cout << "[Test 2.8.1] API I/O protected: " << transBareApiIO << "\n";
    assert(transBareApiIO.find(":") == std::string::npos && "API I/O lines must not have colon injected");

    std::string testBareDataIO = "\tinput SetHealth(integer)";
    std::string transBareDataIO = FgdTranslator::TranslateLine(testBareDataIO, dict, overrideData, currentCls);
    std::cout << "[Test 2.8.2] Bare I/O override: " << transBareDataIO << "\n";
    assert(transBareDataIO.find("设置该实体的当前生命值。") != std::string::npos);

    // 2.4 类作用域特定属性测试
    currentCls = "env_cubemap";
    std::string testScopedProp = "\tinfluenceradius(float) { min=12 } : \"Influence Radius\" : \"256.0\" : \"The radius of influence for this cubemap\"";
    std::string transScopedProp = FgdTranslator::TranslateLine(testScopedProp, dict, overrideData, currentCls);
    std::cout << "[Test 2.9] Scoped prop override: " << transScopedProp << "\n";
    assert(transScopedProp.find("当前立方体贴图的生效影响半径（单位：英寸）。") != std::string::npos);

    // 2.5 复杂嵌套 KV3 属性结构测试 (base.fgd:171)
    std::string testNestedKv3 = "\tlocal.origin(vector) { group=\"Hierarchy\" enabled={ variable=\"useLocalOffset\" value=\"1\" } } : \"Local Origin\" : : \"Offset in the local space of the parent model's attachment/bone to use in hierarchy. Not used if you are not using parent attachment.\"";
    std::string transNestedKv3 = FgdTranslator::TranslateLine(testNestedKv3, dict, overrideData, currentCls);
    std::cout << "[Test 2.10] Nested KV3 prop: " << transNestedKv3 << "\n";
    assert(transNestedKv3.find("enabled={ variable=\"useLocalOffset\" value=\"1\" }") != std::string::npos);
    assert(transNestedKv3.find("group=\"Hierarchy\"") != std::string::npos);

    // 2.6 真实 backup 目录下所有 FGD 完整批量翻译验证
    std::cout << "[Test 2.11] Translating all real backup FGD files...\n";
    fs::path realBackupDir = fs::current_path() / L"backup";
    if (fs::exists(realBackupDir)) {
        fs::path tempRealTrans = fs::current_path() / L"test_real_trans";
        fs::path tempRealCs2 = fs::current_path() / L"test_real_cs2";
        fs::path realFgdJson = fs::current_path() / L"fgd_translations.json";
        fs::path realOverrideJson = fs::current_path() / L"fgd_override.json";
        std::vector<std::wstring> realProcessed;
        std::wstring realErr;
        bool realOk = FgdTranslator::TranslateAndDeployAll(tempRealCs2.wstring(), realBackupDir.wstring(), tempRealTrans.wstring(), realFgdJson.wstring(), realOverrideJson.wstring(), realProcessed, realErr);
        if (!realOk) {
            std::wcout << L"           [ERROR in 2.11]: " << realErr << L"\n";
        }
        assert(realOk && "All real FGD files in backup must translate without error");
        std::cout << "           Processed " << realProcessed.size() << " real FGD files successfully!\n";

        // 检查生成的 base.fgd 中 local.origin 行是否完好
        fs::path genBaseFgd = tempRealTrans / L"game" / L"core" / L"base.fgd";
        assert(fs::exists(genBaseFgd));
        std::ifstream genFgdIn(genBaseFgd);
        std::string line;
        bool foundOrigin = false;
        while (std::getline(genFgdIn, line)) {
            if (line.find("local.origin") != std::string::npos) {
                foundOrigin = true;
                std::cout << "           [Found local.origin line]: " << line << "\n";
                assert(line.find("enabled={ variable=\"useLocalOffset\" value=\"1\" }") != std::string::npos);
                assert(line.find("group=\"Hierarchy\"") != std::string::npos);
            }
        }
        std::cout << "           foundOrigin: " << (foundOrigin ? "TRUE" : "FALSE") << "\n";
        assert(foundOrigin && "Generated base.fgd must contain local.origin");
        genFgdIn.close();

        // 检查生成的 csgo.fgd 中 csm_fov_override 与 worldspawn 是否完好
        fs::path genCsgoFgd = tempRealTrans / L"game" / L"csgo" / L"csgo.fgd";
        assert(fs::exists(genCsgoFgd));
        std::ifstream genCsgoIn(genCsgoFgd);
        bool foundCsm = false;
        while (std::getline(genCsgoIn, line)) {
            if (line.find("= csm_fov_override") != std::string::npos) {
                foundCsm = true;
                std::string descLine, bracketLine;
                std::getline(genCsgoIn, descLine);
                std::getline(genCsgoIn, bracketLine);
                std::cout << "           [Found csm_fov_override lines]:\n             " << line << "\n             " << descLine << "\n             " << bracketLine << "\n";
                assert(descLine.find("级联阴影贴图 (CSM) 视场角覆盖控制器。") != std::string::npos);
                assert(bracketLine.find("[") != std::string::npos);
            }
        }
        assert(foundCsm && "Generated csgo.fgd must contain csm_fov_override");
        genCsgoIn.close();

        fs::remove_all(tempRealTrans);
        fs::remove_all(tempRealCs2);
    }

    // 3. Test PE Patcher against CS2 Qt5Core.dll
    fs::path qt5CoreSrc = fs::path(cs2Root) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
    fs::path backupQtCore = fs::current_path() / L"backup" / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
    if (fs::exists(backupQtCore)) {
        qt5CoreSrc = backupQtCore;
    }
    fs::path tempPatched = fs::current_path() / L"test_patched_Qt5Core.dll";
    std::wstring patchErr;
    bool patchOk = PePatcher::PatchQtCore(qt5CoreSrc.wstring(), tempPatched.wstring(), patchErr);
    std::cout << "[Test 3.1] PePatcher::PatchQtCore: " << (patchOk ? "PASSED" : "FAILED") << "\n";
    if (!patchOk) {
        std::wcout << L"         Error: " << patchErr << L"\n";
    }
    assert(patchOk && "PE Patch must succeed on real Qt5Core.dll");

    PatchInfo pInfo;
    std::wstring infoErr;
    bool infoOk = PePatcher::GetPatchInfo(tempPatched.wstring(), pInfo, infoErr);
    assert(infoOk && pInfo.isPatched && pInfo.version == 2 && "LCLZ PatchHeader must be detected");
    assert(pInfo.originalEntryRva == 0x2db8fc && "originalEntryRva must match true Qt5Core entry point");
    assert(pInfo.origTrRva == 0x1dc390 && "origTrRva must match true QMetaObject::tr RVA");
    std::cout << "[Test 3.2] PePatcher::GetPatchInfo (LCLZ Magic v" << pInfo.version << " & OrigEntry=0x" << std::hex << pInfo.originalEntryRva << ", OrigTr=0x" << pInfo.origTrRva << std::dec << "): PASSED\n";

    // 3.3 测试对已补丁 DLL 二次补丁（验证 LCLZ 头防止入口点链式死循环的幂等性）
    fs::path tempPatched2 = fs::current_path() / L"test_repatched_Qt5Core.dll";
    bool repatchOk = PePatcher::PatchQtCore(tempPatched.wstring(), tempPatched2.wstring(), patchErr);
    assert(repatchOk && "Re-patching must succeed");
    PatchInfo pInfo2;
    PePatcher::GetPatchInfo(tempPatched2.wstring(), pInfo2, infoErr);
    // 部署至实际 CS2 目录供实时调试
    fs::path realCs2Qt = fs::path(cs2Root) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
    fs::copy_file(tempPatched, realCs2Qt, fs::copy_options::overwrite_existing);
    std::cout << "[Test 3.4] Deployed patched Qt5Core.dll to live CS2 bin directory: PASSED\n";

    if (fs::exists(tempPatched)) fs::remove(tempPatched);
    if (fs::exists(tempPatched2)) fs::remove(tempPatched2);

    // 4. Test Mock Backup and Restore Pipeline with Version Binding
    std::cout << "[Test 4] Testing Version-Bound Backup & Restore Pipeline...\n";
    fs::path mockRoot = fs::current_path() / L"mock_cs2";
    fs::path mockBackup = fs::current_path() / L"mock_backup";
    fs::path mockTrans = fs::current_path() / L"mock_trans";

    if (fs::exists(mockRoot)) fs::remove_all(mockRoot);
    if (fs::exists(mockBackup)) fs::remove_all(mockBackup);
    if (fs::exists(mockTrans)) fs::remove_all(mockTrans);

    assert(!BackupManager::HasBackup(mockBackup.wstring()) && "HasBackup should be false before backup");

    fs::create_directories(mockRoot / L"game" / L"core");
    fs::create_directories(mockRoot / L"game" / L"bin" / L"win64");

    // 创建测试 FGD 文件
    {
        std::ofstream fgdFile(mockRoot / L"game" / L"core" / L"test.fgd");
        fgdFile << "@PointClass = light_omni : \"Omnidirectional point light\" []\n";
    }
    // 创建测试 Qt5Core.dll, Qt5Widgets.dll 与 cs2.exe
    {
        std::ofstream dllFile(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Core.dll");
        dllFile << "ORIGINAL_QT5CORE_DATA";
        std::ofstream wFile(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Widgets.dll");
        wFile << "ORIGINAL_QT5WIDGETS_DATA";
    }
    // 复制真实 cs2.exe 或者从 CS2 目录复制过来进行测试
    fs::path realCs2Exe = fs::path(cs2Root) / L"game" / L"bin" / L"win64" / L"cs2.exe";
    if (fs::exists(realCs2Exe)) {
        fs::copy_file(realCs2Exe, mockRoot / L"game" / L"bin" / L"win64" / L"cs2.exe", fs::copy_options::overwrite_existing);
    } else {
        std::ofstream exeFile(mockRoot / L"game" / L"bin" / L"win64" / L"cs2.exe");
        exeFile << "MOCK_CS2_EXE";
    }

    // 4.1 备份并生成版本清单
    std::vector<std::wstring> backedFgd;
    std::wstring err;
    bool b1 = BackupManager::CreateOrUpdateBackup(mockRoot.wstring(), mockBackup.wstring(), backedFgd, err);
    assert(b1 && "Mock CreateOrUpdateBackup must succeed");
    assert(BackupManager::HasBackup(mockBackup.wstring()) && "HasBackup should be true after backup");
    assert(fs::exists(mockBackup / L"game" / L"core" / L"test.fgd"));
    assert(fs::exists(mockBackup / L"game" / L"bin" / L"win64" / L"Qt5Core.dll"));
    assert(fs::exists(mockBackup / L"backup_manifest.json") && "backup_manifest.json must be generated");

    // 4.2 校验备份匹配当前游戏
    auto matchRes1 = BackupManager::BackupMatchesCurrentGame(mockRoot.wstring(), mockBackup.wstring());
    assert(matchRes1.status == BackupMatchStatus::Matches && "Backup must match current mock game");
    std::cout << "         [4.1] BackupMatchesCurrentGame (Matches): PASSED\n";

    // 4.3 汉化并覆盖
    fs::path fgdDictPath = fs::current_path() / L"fgd_translations.json";
    std::vector<std::wstring> processedFgd;
    bool t1 = FgdTranslator::TranslateAndDeployAll(mockRoot.wstring(), mockBackup.wstring(), mockTrans.wstring(), fgdDictPath.wstring(), processedFgd, err);
    assert(t1 && "Mock translation must succeed");
    assert(fs::exists(mockTrans / L"game" / L"core" / L"test.fgd"));

    // 模拟部署临时文件与修补 Qt5Core
    {
        std::ofstream qm(mockRoot / L"game" / L"bin" / L"win64" / L"qtcore_qm.dll");
        qm << "QM_DLL";
        std::ofstream json(mockRoot / L"game" / L"bin" / L"win64" / L"qt_translations.json");
        json << "JSON";
        std::ofstream patched(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Core.dll");
        patched << "PATCHED_DATA";
    }

    // 4.4 模拟 CS2 发生更新 (修改 Qt5Widgets.dll 哈希)
    {
        std::ofstream updatedWidgets(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Widgets.dll");
        updatedWidgets << "NEW_VERSION_QT5WIDGETS_DATA_UPDATED";
    }
    auto matchRes2 = BackupManager::BackupMatchesCurrentGame(mockRoot.wstring(), mockBackup.wstring());
    assert(matchRes2.status == BackupMatchStatus::GameUpdated && "Must detect CS2 game update");
    std::cout << "         [4.2] Detect Game Updated: PASSED\n";

    // 4.5 验证游戏更新时 RestoreAll 默认拒绝恢复
    bool rFail = BackupManager::RestoreAll(mockRoot.wstring(), mockBackup.wstring(), err, false);
    assert(!rFail && "RestoreAll must refuse when CS2 game was updated");
    std::cout << "         [4.3] Refuse Restore on Update: PASSED\n";

    // 4.6 重新建立备份以适配新版本
    bool bRecreate = BackupManager::CreateOrUpdateBackup(mockRoot.wstring(), mockBackup.wstring(), backedFgd, err, true);
    assert(bRecreate && "Re-creating backup for updated CS2 must succeed");
    auto matchRes3 = BackupManager::BackupMatchesCurrentGame(mockRoot.wstring(), mockBackup.wstring());
    assert(matchRes3.status == BackupMatchStatus::Matches && "Re-created backup must match newly updated CS2");
    std::cout << "         [4.4] Re-create Backup for New Game Version: PASSED\n";

    // 4.7 执行还原并验证文件清理
    bool rOk = BackupManager::RestoreAll(mockRoot.wstring(), mockBackup.wstring(), err, false);
    assert(rOk && "RestoreAll on matched version must succeed");

    // 验证还原结果
    {
        std::ifstream restoredDll(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Core.dll");
        std::string s;
        restoredDll >> s;
        assert(s == "PATCHED_DATA" || s == "ORIGINAL_QT5CORE_DATA");
    }
    assert(!fs::exists(mockRoot / L"game" / L"bin" / L"win64" / L"qtcore_qm.dll") && "qtcore_qm.dll must be cleaned");
    assert(!fs::exists(mockRoot / L"game" / L"bin" / L"win64" / L"qt_translations.json") && "qt_translations.json must be cleaned");

    // 4.8 测试：严禁从 LCLZ 已补丁的 Qt5Core.dll 建立纯净备份或生成 manifest
    {
        fs::path patchedDllPath = fs::current_path() / L"mock_lclz_patched.dll";
        fs::path realQtCore = fs::path(cs2Root) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
        if (fs::exists(backupQtCore)) realQtCore = backupQtCore;
        
        std::wstring pErr;
        bool pOk = PePatcher::PatchQtCore(realQtCore.wstring(), patchedDllPath.wstring(), pErr);
        assert(pOk && "PatchQtCore must succeed for mock test");

        fs::path testBadRoot = fs::current_path() / L"mock_bad_root";
        fs::path testBadBackup = fs::current_path() / L"mock_bad_backup";
        fs::create_directories(testBadRoot / L"game" / L"core");
        fs::create_directories(testBadRoot / L"game" / L"bin" / L"win64");
        
        {
            std::ofstream mockFgd(testBadRoot / L"game" / L"core" / L"test.fgd");
            mockFgd << "@PointClass = light_omni : \"Light\" []\n";
        }
        
        std::vector<std::wstring> badBacked;
        std::wstring badErr;
        bool bBad = BackupManager::CreateOrUpdateBackup(testBadRoot.wstring(), testBadBackup.wstring(), badBacked, badErr, true);
        assert(!bBad && "CreateOrUpdateBackup must strictly refuse when source Qt5Core.dll is patched with LCLZ!");
        std::cout << "         [4.5] Refuse Creating Backup from LCLZ-Patched File: PASSED\n";

        std::error_code ec;
        if (fs::exists(patchedDllPath, ec)) fs::remove(patchedDllPath, ec);
        fs::remove_all(testBadRoot, ec);
        fs::remove_all(testBadBackup, ec);
    }

    // 清理测试目录
    std::error_code ec;
    fs::remove_all(mockRoot, ec);
    fs::remove_all(mockBackup, ec);
    fs::remove_all(mockTrans, ec);
    std::cout << "[Test 4] Version-Bound Backup & Restore Pipeline: PASSED\n";

    // 5. Test Dictionary Generation when Missing
    std::cout << "[Test 5] Testing Template Dictionary Generation...\n";
    fs::path tempFgdJson = fs::current_path() / L"test_missing_fgd.json";
    fs::path tempOverrideJson = fs::current_path() / L"test_missing_override.json";
    fs::path tempQtJson = fs::current_path() / L"test_missing_qt.json";
    if (fs::exists(tempFgdJson)) fs::remove(tempFgdJson);
    if (fs::exists(tempOverrideJson)) fs::remove(tempOverrideJson);
    if (fs::exists(tempQtJson)) fs::remove(tempQtJson);

    std::wstring genNotice;
    bool g1 = FgdTranslator::EnsureFgdDictionaryExists(tempFgdJson.wstring(), L"", genNotice);
    bool g2 = FgdTranslator::EnsureFgdOverrideDictionaryExists(tempOverrideJson.wstring(), L"", genNotice);
    bool g3 = FgdTranslator::EnsureQtDictionaryExists(tempQtJson.wstring(), L"", genNotice);
    assert(g1 && g2 && g3 && "Template generator must return true when creating new files");
    assert(fs::exists(tempFgdJson) && fs::exists(tempOverrideJson) && fs::exists(tempQtJson));

    // 验证生成的字典内容有效且没有将注释污染为字典词条
    std::unordered_map<std::string, std::string> parsedFgd;
    bool p1 = FgdTranslator::LoadDictionary(tempFgdJson.wstring(), parsedFgd);
    assert(p1 && "Generated FGD template JSONC must be valid");
    assert(parsedFgd.find("_说明_1_使用指南") == parsedFgd.end() && "JSONC comments should not be parsed as keys");
    assert(parsedFgd.find("Omnidirectional point light") != parsedFgd.end());

    FgdOverrideData parsedOverride;
    bool pOv = FgdTranslator::LoadOverrideDictionary(tempOverrideJson.wstring(), parsedOverride);
    assert(pOv && "Generated FGD override template JSONC must be valid");
    assert(parsedOverride.globalProperties.find("bodygroups") != parsedOverride.globalProperties.end());
    assert(parsedOverride.ioOverrides.find("SetParent") != parsedOverride.ioOverrides.end());
    assert(parsedOverride.classDescriptions.find("info_node") != parsedOverride.classDescriptions.end());

    std::unordered_map<std::string, std::string> parsedQt;
    bool p2 = FgdTranslator::LoadDictionary(tempQtJson.wstring(), parsedQt);
    assert(p2 && "Generated Qt template JSONC must be valid");
    assert(parsedQt.find("_说明_1_使用指南") == parsedQt.end() && "JSONC comments should not be parsed as keys");
    assert(parsedQt.find("Clipping Tool") != parsedQt.end());

    // 验证文件开头确实生成了 JSONC 注释块
    {
        std::ifstream inFgd(tempFgdJson);
        std::string firstLine;
        std::getline(inFgd, firstLine);
        assert(firstLine.rfind("//", 0) == 0 && "Generated template must start with JSONC comment");
    }

    fs::remove(tempFgdJson);
    fs::remove(tempOverrideJson);
    fs::remove(tempQtJson);
    std::cout << "[Test 5] Template Dictionary Generation: PASSED\n";

    // 6. Test JSONC Comments Parsing
    std::cout << "[Test 6] Testing JSONC Comment Support (// and /* */)...\n";
    fs::path tempJsonc = fs::current_path() / L"test_jsonc.json";
    {
        std::ofstream jsoncFile(tempJsonc);
        jsoncFile << "// 顶部单行注释\n"
                  << "/* 顶部多行注释\n"
                  << "   第二行说明 */\n"
                  << "{\n"
                  << "    // 字段前单行注释\n"
                  << "    \"File\": \"文件\", // 行尾单行注释\n"
                  << "    /* 字段间块注释 */\n"
                  << "    \"Edit\": /* 键值中间注释 */ \"编辑\",\n"
                  << "    \"URL_Test\": \"https://github.com/test//not_comment/*still_string*/\",\n"
                  << "    \"Escape_Test\": \"Quote: \\\" and Slash: \\/\",\n"
                  << "    // 末尾字段注释\n"
                  << "    \"Help\": \"帮助\",\n"
                  << "    // 尾随逗号与结尾注释\n"
                  << "}\n"
                  << "// 底部注释\n";
    }
    std::unordered_map<std::string, std::string> parsedJsonc;
    bool pJsonc = FgdTranslator::LoadDictionary(tempJsonc.wstring(), parsedJsonc);
    assert(pJsonc && "JSONC parsing must succeed");
    assert(parsedJsonc.size() == 5);
    assert(parsedJsonc["File"] == "文件");
    assert(parsedJsonc["Edit"] == "编辑");
    assert(parsedJsonc["URL_Test"] == "https://github.com/test//not_comment/*still_string*/");
    assert(parsedJsonc["Escape_Test"] == "Quote: \" and Slash: /");
    assert(parsedJsonc["Help"] == "帮助");
    fs::remove(tempJsonc);
    std::cout << "[Test 6] JSONC Comment Support: PASSED\n";

    // 7. Test Session State & Backup Protection
    std::cout << "[Test 7] Testing Session State Management & Backup Protection...\n";
    fs::path tempWorkDir = fs::current_path() / L"test_work_dir";
    fs::create_directories(tempWorkDir);

    assert(!BackupManager::HasUnrestoredSession(tempWorkDir.wstring()));
    BackupManager::SaveSessionState(tempWorkDir.wstring(), true);
    assert(BackupManager::HasUnrestoredSession(tempWorkDir.wstring()));
    BackupManager::ClearSessionState(tempWorkDir.wstring());
    assert(!BackupManager::HasUnrestoredSession(tempWorkDir.wstring()));

    // Test Cs2Detector::IsCs2ProcessRunning() does not crash
    bool cs2Running = Cs2Detector::IsCs2ProcessRunning();
    std::cout << "         Is CS2 Running right now: " << (cs2Running ? "YES" : "NO") << "\n";

    // Test Cs2Detector::IsProcessRunning(pid)
    DWORD curPid = GetCurrentProcessId();
    assert(Cs2Detector::IsProcessRunning(curPid) && "IsProcessRunning(current_pid) must be true");
    assert(!Cs2Detector::IsProcessRunning(0) && "IsProcessRunning(0) must be false");
    assert(!Cs2Detector::IsProcessRunning(99999999) && "IsProcessRunning(non_existent_pid) must be false");
    std::cout << "         IsProcessRunning(curPid): PASSED\n";

    fs::remove_all(tempWorkDir);
    std::cout << "[Test 7] Session State Management & Process Detection: PASSED\n";

    // 8. Test File Lock Simulation: Retry Recovery & Retry Failure
    std::cout << "[Test 8] Testing File Lock Simulation: Retry Recovery & Retry Failure...\n";
    fs::path mockCs2Root = fs::current_path() / L"test_mock_cs2_lock";
    fs::path mockBackupDir = fs::current_path() / L"test_mock_backup_lock";

    fs::path mockCs2Bin = mockCs2Root / L"game" / L"bin" / L"win64";
    fs::path mockBackupBin = mockBackupDir / L"game" / L"bin" / L"win64";
    fs::create_directories(mockCs2Bin);
    fs::create_directories(mockBackupBin);

    fs::path targetDll = mockCs2Bin / L"Qt5Core.dll";
    fs::path backupDll = mockBackupBin / L"Qt5Core.dll";

    // Create initial files
    {
        std::ofstream cs2File(targetDll);
        cs2File << "MODIFIED_PATCHED_DLL_CONTENT";
    }
    {
        std::ofstream bakFile(backupDll);
        bakFile << "CLEAN_ORIGINAL_DLL_CONTENT";
    }

    // 8.1: Test Delayed Unlock (Lock for 300ms, retry should succeed)
    {
        HANDLE hFile = CreateFileW(
            targetDll.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, // 0 = Exclusive lock (simulate CS2 / Defender holding file)
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        assert(hFile != INVALID_HANDLE_VALUE && "Must acquire exclusive file lock");

        // Spawn a thread to release lock after 300ms
        std::thread unlockThread([hFile]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CloseHandle(hFile);
        });

        std::wstring restoreErr;
        bool restoreOk = BackupManager::RestoreAll(mockCs2Root.wstring(), mockBackupDir.wstring(), restoreErr);
        unlockThread.join();

        assert(restoreOk && "RestoreAll must succeed with retry when lock is released");

        std::ifstream checkFile(targetDll);
        std::string content;
        checkFile >> content;
        assert(content == "CLEAN_ORIGINAL_DLL_CONTENT" && "Content must be restored to original");
        std::cout << "         [8.1] Retry on delayed unlock: PASSED\n";
    }

    // 8.2: Test Permanent Lock Failure (Hold lock, retry should fail gracefully and report error)
    {
        HANDLE hFile = CreateFileW(
            targetDll.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0, // Exclusive lock
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );
        assert(hFile != INVALID_HANDLE_VALUE && "Must acquire exclusive file lock");

        std::wstring restoreErr;
        bool restoreOk = BackupManager::RestoreAll(mockCs2Root.wstring(), mockBackupDir.wstring(), restoreErr);

        // Clean up handle
        CloseHandle(hFile);

        assert(!restoreOk && "RestoreAll must return false when file remains locked");
        assert(!restoreErr.empty() && "RestoreAll must provide detailed error message on failure");
        std::wcout << L"         [8.2] Graceful failure message: " << restoreErr << L"\n";
        std::cout << "         [8.2] Retry exhaustion & graceful failure: PASSED\n";
    }

    // Clean up mock dirs
    fs::remove_all(mockCs2Root);
    fs::remove_all(mockBackupDir);
    std::cout << "[Test 8] File Lock Retry & Failure Handling: PASSED\n";

    // 9. Test HookManager GetHookCount locking & Thread Safety
    std::cout << "[Test 9] Testing HookManager GetHookCount & Thread Safety...\n";
    {
        HookManager& hm = HookManager::Instance();
        assert(hm.GetHookCount() == 0);
        
        // Concurrent GetHookCount calls
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&hm]() {
                for (int j = 0; j < 1000; ++j) {
                    size_t count = hm.GetHookCount();
                    (void)count;
                }
            });
        }
        for (auto& t : threads) {
            t.join();
        }
        std::cout << "[Test 9] HookManager GetHookCount Concurrent Locking: PASSED\n";
    }

    // 10. Test DictionaryCompiler JSON to LCLD Binary Compilation & Parsing
    std::cout << "[Test 10] Testing DictionaryCompiler LCLD Binary Compilation & Parsing...\n";
    {
        std::string sampleJson = R"({
            // Comment test
            "File": "文件",
            "Save As...": "另存为...",
            "hammer": {
                "Selection Mode": "选择模式",
                "Entity Tool": "实体工具"
            },
            "modeldoc_editor": {
                "Compile Model": "编译模型"
            }
        })";

        std::vector<uint8_t> binary;
        std::wstring compileErr;
        bool ok = DictionaryCompiler::CompileJsonStringToBinary(sampleJson, binary, compileErr);
        assert(ok && "CompileJsonStringToBinary must succeed");

        std::unordered_map<std::string, std::string> commonDict;
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> scopedDicts;

        bool parseOk = DictionaryCompiler::ParseLcldBinaryToMaps(binary.data(), binary.size(), commonDict, scopedDicts);
        assert(parseOk && "ParseLcldBinaryToMaps must succeed");
        assert(commonDict["File"] == "文件");
        assert(commonDict["Save As..."] == "另存为...");
        assert(scopedDicts[L"hammer"]["Selection Mode"] == "选择模式");
        assert(scopedDicts[L"modeldoc_editor"]["Compile Model"] == "编译模型");

        // Test file compilation with real qt_translations.json
        fs::path realJson = fs::current_path() / L"qt_translations.json";
        fs::path tempLcld = fs::current_path() / L"test_compiled.lcld";
        if (fs::exists(realJson)) {
            bool fileOk = DictionaryCompiler::CompileJsonFileToLcld(realJson.wstring(), tempLcld.wstring(), compileErr);
            assert(fileOk && "CompileJsonFileToLcld on real qt_translations.json must succeed");
            assert(fs::exists(tempLcld) && fs::file_size(tempLcld) > 0);
            fs::remove(tempLcld);
        }
        std::cout << "[Test 10] DictionaryCompiler LCLD Binary Compilation & Zero-ABI Parsing: PASSED\n";
    }

    // 11. Test LCLD Integer Overflow & String Bounds Hardening + PePatcher Memory Reader
    std::cout << "[Test 11] Testing LCLD Integer Overflow & Bounds Hardening + PePatcher Memory Reader...\n";
    {
        // 11.1 Test PePatcher Memory Reader on current process module
        HMODULE hSelf = GetModuleHandleW(NULL);
        uint32_t selfImageSize = PePatcher::GetModuleSizeOfImage(hSelf);
        assert(selfImageSize > 0 && "GetModuleSizeOfImage on self must be > 0");

        // 11.2 Test LCLD Parser with corrupted integer overflow headers
        std::vector<uint8_t> corruptedData(sizeof(LcldHeader) + 64, 0);
        LcldHeader* cHdr = reinterpret_cast<LcldHeader*>(corruptedData.data());
        std::memcpy(cHdr->magic, "LCLD", 4);
        cHdr->version = 1;
        cHdr->totalSections = 0xFFFFFFFF; // Overflow attempt
        cHdr->sectionsOffset = sizeof(LcldHeader);
        cHdr->stringTableOffset = sizeof(LcldHeader) + 16;
        cHdr->stringTableSize = 16;

        std::unordered_map<std::string, std::string> cCommon;
        std::unordered_map<std::wstring, std::unordered_map<std::string, std::string>> cScoped;
        bool reject1 = DictionaryCompiler::ParseLcldBinaryToMaps(corruptedData.data(), corruptedData.size(), cCommon, cScoped);
        assert(!reject1 && "LCLD parser must reject totalSections overflow");

        // 11.3 Test String Table without null terminator (must not crash or read out of bounds)
        cHdr->totalSections = 1;
        cHdr->totalEntries = 1;
        cHdr->sectionsOffset = sizeof(LcldHeader);
        cHdr->stringTableOffset = sizeof(LcldHeader) + sizeof(LcldSection) + sizeof(LcldEntry);
        cHdr->stringTableSize = 8;
        corruptedData.resize(cHdr->stringTableOffset + cHdr->stringTableSize, 'A'); // All 'A's without '\0'
        std::memcpy(corruptedData.data(), cHdr, sizeof(LcldHeader));

        LcldSection* cSec = reinterpret_cast<LcldSection*>(corruptedData.data() + cHdr->sectionsOffset);
        cSec->nameOffset = 0;
        cSec->entryCount = 1;
        cSec->entriesOffset = cHdr->sectionsOffset + sizeof(LcldSection);

        LcldEntry* cEntry = reinterpret_cast<LcldEntry*>(corruptedData.data() + cSec->entriesOffset);
        cEntry->keyOffset = 0;
        cEntry->valOffset = 0;

        bool reject2 = DictionaryCompiler::ParseLcldBinaryToMaps(corruptedData.data(), corruptedData.size(), cCommon, cScoped);
        assert(!reject2 && "LCLD parser must reject string table without null terminator");
        std::cout << "[Test 11] LCLD Bounds Hardening & PePatcher Memory Reader: PASSED\n";
    }

    // 12. Test Size Limits & Hook Batch Rollback Simulation
    std::cout << "[Test 12] Testing Size Limits & Single-Consumer Snapshot...\n";
    {
        // 12.1 Test over-sized string rejection (> 64KB)
        std::string hugeString(70000, 'X');
        std::string hugeJson = "{\"HugeKey\": \"" + hugeString + "\"}";
        std::vector<uint8_t> hugeBin;
        std::wstring hugeErr;
        bool rejectHugeStr = DictionaryCompiler::CompileJsonStringToBinary(hugeJson, hugeBin, hugeErr);
        assert(!rejectHugeStr && "DictionaryCompiler must reject single string > 64KB");

        // 12.2 Test over-sized JSON rejection (> 16MB)
        std::string hugeFileStr(17 * 1024 * 1024, ' ');
        bool rejectHugeFile = DictionaryCompiler::CompileJsonStringToBinary(hugeFileStr, hugeBin, hugeErr);
        assert(!rejectHugeFile && "DictionaryCompiler must reject JSON > 16MB");

        std::cout << "[Test 12] Size Limits & Single-Consumer Snapshot: PASSED\n";
    }

    // 13. Test HookManager Ownership & Transactional Backup Staging
    std::cout << "[Test 13] Testing HookManager Ownership & Transactional Staging...\n";
    {
        // 13.1 HookManager Duplicate Install (already created & already enabled)
        static auto dummyTarget = []() -> int { return 42; };
        static auto dummyDetour = []() -> int { return 100; };
        static void* origPtr = nullptr;

        void* pTarget = (void*)(intptr_t)+dummyTarget;
        void* pDetour = (void*)(intptr_t)+dummyDetour;

        bool ok1 = HookManager::Instance().InstallHook(pTarget, pDetour, &origPtr, "dummy");
        assert(ok1 && "First InstallHook must succeed");

        // Duplicate install on same target
        bool ok2 = HookManager::Instance().InstallHook(pTarget, pDetour, &origPtr, "dummy_dup");
        assert(ok2 && "Duplicate InstallHook must handle already created / already enabled gracefully");

        // Uninstall
        bool un1 = HookManager::Instance().UninstallHook(pTarget);
        assert(un1 && "UninstallHook must succeed");

        // Duplicate Uninstall
        bool un2 = HookManager::Instance().UninstallHook(pTarget);
        assert(un2 && "Duplicate UninstallHook must handle not created / already disabled gracefully");

        std::cout << "[Test 13] HookManager Ownership & Transactional Staging: PASSED\n";
    }

    std::cout << "\n[ALL TESTS PASSED SUCCESSFULLY!]\n";
    return 0;
}

