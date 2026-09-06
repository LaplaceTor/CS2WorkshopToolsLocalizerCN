修复 src/ 代码健壮性 + 用 Qt 替换现有手写实现（范围不含 scripts/、CI、.gitignore；不新造工具函数，全部复用 Qt 库 / Win32 / 项目现有函数）

## 约束说明
`dictionary_compiler.cpp`、`pe_patcher.cpp` 同时编入主程序与**不链接 Qt 的注入 DLL**（CMakeLists 中 qtcore_qm 目标仅链 user32/psapi），这两处及 qtcore_qm.cpp/hook_manager.cpp 保持无 Qt；Qt 替换集中在主程序独占的 backup_manager.cpp、fgd_translator.cpp、cs2_detector.cpp、mainwindow.cpp（项目已有 QSaveFile/QJsonDocument/QSettings/QCryptographicHash 使用先例）。词典文件为只读消费（注释经 StripJsonComments 剥离后丢弃，无保留需求），不构成 Qt 替换障碍。

## 阶段一：崩溃与数据损坏防护
1. **M5 启动错误守卫** `mainwindow.cpp:1717-1720`：onHammerError 对 `QProcess::FailedToStart` 无条件处理——重置 `m_isHammerRunning=false`、停 `m_monitorTimer`、弹窗 + `doRestore(true)` + `ClearSessionState` + `setUiBusy(false)`；其他错误码维持现有路径避免双重恢复。（现状：start() 前已置 true :1436，FailedToStart 只发 errorOccurred，错误分支被 `if (!m_isHammerRunning)` 整体跳过）
2. **原子写统一**（复用项目已有 QSaveFile 先例）：
   - `SaveSessionState`（backup_manager.cpp:553-569）手写 ofs<< 拼JSON → **QSaveFile + QJsonDocument**（与 WriteBackupManifest :186-201 同款写法）；
   - `FgdTranslator::TranslateFile`（fgd_translator.cpp:778-788）→ **QSaveFile**；
   - `MergeJsonFiles`（dictionary_compiler.cpp:413-446）与补丁 Qt5Core.dll 写盘（pe_patcher.cpp:666-676）：非 Qt TU → 写 `路径+".tmp"` 后 `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`，失败删除临时文件。
3. **部署拷贝统一重试**：`SafeCopyFileWithRetry`/`SafeRemoveFileWithRetry`（backup_manager.cpp:59-84，实现不变）改为 BackupManager 公共静态成员，替换 4 处裸 `fs::copy_file`：BackupFgdFiles:418、BackupQtCore:465、mainwindow.cpp:1154（qtcore_qm.dll 拷贝）、fgd_translator.cpp:843（FGD 部署）；FGD 部署改"临时名拷贝 → fs::rename 替换"。
4. **fread 返回值检查** dictionary_compiler.cpp:331,368，短读按解析失败处理。
5. **堆越界读** pe_patcher.cpp:211-219：k 上界收紧至 k+5 ≤ 64。
6. **判空** qtcore_qm.cpp:1540：先判 `req.ppOriginal` 再解引用 `*req.ppOriginal`。

## 阶段二：手写代码 Qt 替换 + 错误处理统一
7. **HasUnrestoredSession**（backup_manager.cpp:536-551）：ifstream+stringstream+`content.find("\"is_patched\": true")` → **QFile + QJsonDocument::fromJson** 取 `is_patched` 布尔值（与 ReadBackupManifest :134-148 写法统一），顺带修复空格/格式差异导致判定失效。
8. **异常消息乱码**：backup_manager.cpp :121/:163/:205/:426/:468/:520 与 fgd_translator.cpp:851 的 `std::wstring(e.what(), e.what()+strlen(...))` → **QString::fromUtf8(e.what()).toStdWString()**；backup_manager.cpp:236-239 两处 `std::wstring(str.begin(), str.end())` → **QString::fromStdString().toStdWString()**。
9. **静默异常落日志**：SaveSessionState/ClearSessionState（:553-577）`catch (...) {}` → **qWarning()** 记录 + 返回 bool，调用点 appendLog。
10. **cs2_detector.cpp Qt 简化**：
    - ParseSteamLibraryFolders（:101-139）：`std::regex` + 手写 `\\` 反转义循环 + MultiByteToWideChar → **QRegularExpression** 全局匹配 + `QString::replace("\\\\","\\")` + fromStdString().toStdWString()（约 25 行 → 8 行，顺带修正 UTF-8 转换）；
    - CheckCommonDrivePaths（:183-203）：硬编码 C:~H: 盘符 → **QDir::drives()** 枚举全部盘符。
11. **FGD 单文件失败语义**（fgd_translator.cpp:837-840）：失败收集清单并继续处理其余文件，结束汇总（成功数/失败列表），消除"部分已部署但整体报失败"的误导。
12. **JSON 写出全面 Qt 化，删除手写转义函数**：
    - 三个模板生成函数（fgd_translator.cpp:876-1111，EnsureFgdDictionaryExists / EnsureFgdOverrideDictionaryExists / EnsureQtDictionaryExists）：注释版使用指南作为**字符串字面量**先写出（QJsonDocument 无需处理注释），其后的数据体改由 **QJsonObject 组装 + QJsonDocument::toJson** 输出（转义由 Qt 处理）；删除 EscapeJsonString（fgd_translator.cpp:856-870）；
    - EscapeJsonStr（dictionary_compiler.cpp:383-397）随 MergeJsonFiles 留在共享 TU，成为项目内唯一一份（如日后希望彻底 Qt 化 MergeJsonFiles，需将该函数迁出共享编译单元，本次不做）；
    - 数据读取路径维持现有 ParseJsoncFileToMaps（同一实现同时服务 DLL，注释剥离后丢弃，无 Qt 化收益）。
13. **\uXXXX 代理对**（dictionary_compiler.cpp:220-248，现有函数内修改）：补 D800-DBFF + DC00-DFFF 代理对解码为 4 字节 UTF-8，非法代理替换 U+FFFD（该 TU 与 DLL 共享，保持无 Qt）。

## 阶段三：注入 DLL（qtcore_qm.cpp，无 Qt）
14. **VEH 跳指令：默认保持，只记录，短时 K 级爆发才熔断**（:1866-1873）：VEH 内保持 0 锁 0 IO，仅原子计数——10 秒滑动窗口内跳过 ≥1000 次置熔断原子标志，熔断后不再跳过、异常正常传播；记录由现有 ToolsHookThread 后台循环（:1763-1800，500ms 周期）读计数增量经 LogHook 汇总输出。不加环境变量开关。
15. **hook_runtime.log 非 DEBUG 不生成**（:183-214）：LogHook/LogVerboseTr 统一挂现有 `IsVerboseLogEnabled()`（-debug/-verbosehook 启动参数）之下——非 DEBUG 完全不创建/写文件；DEBUG 下保留写入并 8MB 轮转为 .old；两函数重复实现合并为一处。
16. **初始化失败不重试**（:1914-1936）：InitializeTranslator 失败置原子标志，避免每次 tr() 都重试完整初始化并刷日志。

## 阶段四：UI 线程重 IO 迁移（改动最大，最后做）
17. injectLocalization（mainwindow.cpp:907-1271）、doRestore（:1754-1803）、checkAndRecoverAbnormalExit（:2666-2759）中的重 IO（全量 SHA256、备份拷贝、FGD 部署、PE 补丁写盘）移入 worker 线程：**QtConcurrent::run + QFutureWatcher**（CMakeLists 主程序目标补链 Qt6::Concurrent，唯一 src 外改动）；`appendLog` 内部用 **QMetaObject::invokeMethod(..., Qt::QueuedConnection)** 按线程自 marshal（改造现有函数，不新增机制）；worker 运行期间互斥标志禁止并发注入/还原/启动；`isPatchDeployedAndValid`（:788-835）内 BackupMatchesCurrentGame 的 4 次全文件 SHA256 同样移出 UI 线程（缓存校验结果）。
18. **Steam 更新竞态缓解**（:985-1007）：GameUpdated 判定前二次哈希确认——首次 SHA256 不匹配后 `QThread::msleep(2000)`（已在 worker 线程，不卡 UI）重哈希，两次一致才判定游戏更新并重建备份；否则提示"游戏可能正在更新"并中止本次注入，防止把半更新文件定格为"纯净备份"。

## 验收
- CMake 三个目标（qtcore_qm / 主程序 / test_components）构建通过，本机有 CS2 则跑 test_components；
- M5：错误参数模拟 FailedToStart，确认立即弹窗并自动还原；
- 原子写：注入中途强杀启动器，目标目录无半写文件，下次启动 abnormal-exit 恢复正常；
- 阶段二：CS2 路径检测三途径（注册表/VDF/盘符）回归通过；三个模板字典文件自动生成后可被启动器与注入 DLL 正常加载（验证 Qt 写出与现有解析器兼容）；
- 阶段三：非 DEBUG 启动确认 CS2 bin 目录不产生 hook_runtime.log；-verbosehook 下 QComboBox 等 30+ 钩子行为不变、日志轮转正常；构造高频空写场景验证 ≥1000 次/10 秒熔断后异常正常传播；
- 阶段四：注入/还原全程 UI 可响应、日志顺序正确、按钮状态无错乱；
- dev 分支每阶段独立 commit（共 4 个），不 push。