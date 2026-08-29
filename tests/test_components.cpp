#include <iostream>
#include <cassert>
#include <filesystem>
#include <fstream>
#include "../src/cs2_detector.h"
#include "../src/pe_patcher.h"
#include "../src/fgd_translator.h"
#include "../src/backup_manager.h"

namespace fs = std::filesystem;

int main() {
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

    // 2. Test FGD Translation Logic
    std::unordered_map<std::string, std::string> dict;
    dict["Omnidirectional point light"] = "全向点光源";
    dict["Light Source"] = "光源";
    dict["Name"] = "名称";
    dict["The name that other entities use to refer to this entity."] = "其他实体用于引用此实体的名称。";
    dict["Removes this entity from the world."] = "从世界中移除此实体。";
    dict["Enabled"] = "已启用";

    std::string testClass = "@PointClass = light_omni : \"Omnidirectional point light\" []";
    std::string transClass = FgdTranslator::TranslateLine(testClass, dict);
    std::cout << "[Test 2.1] Class trans: " << transClass << "\n";
    assert(transClass.find("全向点光源") != std::string::npos);

    std::string testProp = "    targetname(target_source) : \"Name\" : : \"The name that other entities use to refer to this entity.\"";
    std::string transProp = FgdTranslator::TranslateLine(testProp, dict);
    std::cout << "[Test 2.2] Prop trans: " << transProp << "\n";
    assert(transProp.find("名称") != std::string::npos);
    assert(transProp.find("其他实体用于引用此实体的名称。") != std::string::npos);

    std::string testIO = "    input Kill(void) : \"Removes this entity from the world.\"";
    std::string transIO = FgdTranslator::TranslateLine(testIO, dict);
    std::cout << "[Test 2.3] I/O trans: " << transIO << "\n";
    assert(transIO.find("从世界中移除此实体。") != std::string::npos);

    // 3. Test PE Patcher against CS2 Qt5Core.dll
    fs::path qt5CoreSrc = fs::path(cs2Root) / L"game" / L"bin" / L"win64" / L"Qt5Core.dll";
    fs::path tempPatched = fs::current_path() / L"test_patched_Qt5Core.dll";
    std::wstring patchErr;
    bool patchOk = PePatcher::PatchQtCore(qt5CoreSrc.wstring(), tempPatched.wstring(), patchErr);
    std::cout << "[Test 3] PePatcher::PatchQtCore: " << (patchOk ? "PASSED" : "FAILED") << "\n";
    if (!patchOk) {
        std::wcout << L"         Error: " << patchErr << L"\n";
    }
    assert(patchOk && "PE Patch must succeed on real Qt5Core.dll");
    if (fs::exists(tempPatched)) {
        fs::remove(tempPatched);
    }

    // 4. Test Mock Backup and Restore Pipeline
    std::cout << "[Test 4] Testing Mock Backup & Restore Pipeline...\n";
    fs::path mockRoot = fs::current_path() / L"mock_cs2";
    fs::path mockBackup = fs::current_path() / L"mock_backup";
    fs::path mockTrans = fs::current_path() / L"mock_trans";

    if (fs::exists(mockRoot)) fs::remove_all(mockRoot);
    if (fs::exists(mockBackup)) fs::remove_all(mockBackup);
    if (fs::exists(mockTrans)) fs::remove_all(mockTrans);

    fs::create_directories(mockRoot / L"game" / L"core");
    fs::create_directories(mockRoot / L"game" / L"bin" / L"win64");

    // 创建测试 FGD 文件
    {
        std::ofstream fgdFile(mockRoot / L"game" / L"core" / L"test.fgd");
        fgdFile << "@PointClass = light_omni : \"Omnidirectional point light\" []\n";
    }
    // 创建测试 Qt5Core.dll
    {
        std::ofstream dllFile(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Core.dll");
        dllFile << "ORIGINAL_QT5CORE_DATA";
    }

    // 4.1 备份
    std::vector<std::wstring> backedFgd;
    std::wstring err;
    bool b1 = BackupManager::BackupFgdFiles(mockRoot.wstring(), mockBackup.wstring(), backedFgd, err);
    bool b2 = BackupManager::BackupQtCore(mockRoot.wstring(), mockBackup.wstring(), err);
    assert(b1 && b2 && "Mock backup must succeed");
    assert(fs::exists(mockBackup / L"game" / L"core" / L"test.fgd"));
    assert(fs::exists(mockBackup / L"game" / L"bin" / L"win64" / L"Qt5Core.dll"));

    // 4.2 汉化并覆盖
    fs::path fgdDictPath = fs::current_path() / L"fgd_translations.json";
    std::vector<std::wstring> processedFgd;
    bool t1 = FgdTranslator::TranslateAndDeployAll(mockRoot.wstring(), mockBackup.wstring(), mockTrans.wstring(), fgdDictPath.wstring(), processedFgd, err);
    assert(t1 && "Mock translation must succeed");
    assert(fs::exists(mockTrans / L"game" / L"core" / L"test.fgd"));

    // 模拟部署临时文件
    {
        std::ofstream qm(mockRoot / L"game" / L"bin" / L"win64" / L"qtcore_qm.dll");
        qm << "QM_DLL";
        std::ofstream json(mockRoot / L"game" / L"bin" / L"win64" / L"qt_translations.json");
        json << "JSON";
        // 修改 Qt5Core
        std::ofstream patched(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Core.dll");
        patched << "PATCHED_DATA";
    }

    // 4.3 还原
    bool r1 = BackupManager::RestoreAll(mockRoot.wstring(), mockBackup.wstring(), err);
    assert(r1 && "Mock restore must succeed");

    // 验证还原结果
    {
        std::ifstream restoredDll(mockRoot / L"game" / L"bin" / L"win64" / L"Qt5Core.dll");
        std::string s;
        restoredDll >> s;
        assert(s == "ORIGINAL_QT5CORE_DATA" && "Qt5Core.dll must be restored to original!");
    }
    assert(!fs::exists(mockRoot / L"game" / L"bin" / L"win64" / L"qtcore_qm.dll") && "qtcore_qm.dll must be cleaned");
    assert(!fs::exists(mockRoot / L"game" / L"bin" / L"win64" / L"qt_translations.json") && "qt_translations.json must be cleaned");

    // 清理测试目录
    fs::remove_all(mockRoot);
    fs::remove_all(mockBackup);
    fs::remove_all(mockTrans);
    std::cout << "[Test 4] Mock Backup & Restore Pipeline: PASSED\n";

    // 5. Test Dictionary Generation when Missing
    std::cout << "[Test 5] Testing Template Dictionary Generation...\n";
    fs::path tempFgdJson = fs::current_path() / L"test_missing_fgd.json";
    fs::path tempQtJson = fs::current_path() / L"test_missing_qt.json";
    if (fs::exists(tempFgdJson)) fs::remove(tempFgdJson);
    if (fs::exists(tempQtJson)) fs::remove(tempQtJson);

    std::wstring genNotice;
    bool g1 = FgdTranslator::EnsureFgdDictionaryExists(tempFgdJson.wstring(), L"", genNotice);
    bool g2 = FgdTranslator::EnsureQtDictionaryExists(tempQtJson.wstring(), L"", genNotice);
    assert(g1 && g2 && "Template generator must return true when creating new files");
    assert(fs::exists(tempFgdJson) && fs::exists(tempQtJson));

    // 验证生成的字典内容包含说明
    std::unordered_map<std::string, std::string> parsedFgd;
    bool p1 = FgdTranslator::LoadDictionary(tempFgdJson.wstring(), parsedFgd);
    assert(p1 && "Generated FGD template JSON must be valid");
    assert(parsedFgd.find("_说明_1_使用指南") != parsedFgd.end());
    assert(parsedFgd.find("Omnidirectional point light") != parsedFgd.end());

    std::unordered_map<std::string, std::string> parsedQt;
    bool p2 = FgdTranslator::LoadDictionary(tempQtJson.wstring(), parsedQt);
    assert(p2 && "Generated Qt template JSON must be valid");
    assert(parsedQt.find("_说明_1_使用指南") != parsedQt.end());
    assert(parsedQt.find("Clipping Tool") != parsedQt.end());

    fs::remove(tempFgdJson);
    fs::remove(tempQtJson);
    std::cout << "[Test 5] Template Dictionary Generation: PASSED\n";

    std::cout << "\n[ALL TESTS PASSED SUCCESSFULLY!]\n";
    return 0;
}

