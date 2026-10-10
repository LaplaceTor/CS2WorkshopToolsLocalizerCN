#include "ui/dictionary_editor_window.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
#include <QHeaderView>
#include <QLabel>
#include <QCheckBox>
#include <QTimer>
#include <QMessageBox>
#include <QDateTime>
#include <QCloseEvent>

DictionaryEditorWindow::DictionaryEditorWindow(
    DictionaryEditorService* service,
    std::function<bool()> getMachineTransEnabled,
    QWidget* parent
) : QWidget(parent, Qt::Window),
    m_service(service),
    m_getMachineTransEnabled(std::move(getMachineTransEnabled))
{
    setupUi();
    applyDarkTheme();
    populateModuleCombos();
}

DictionaryEditorWindow::~DictionaryEditorWindow() = default;

void DictionaryEditorWindow::setupUi() {
    setWindowTitle("词典运行时编辑器与实时未翻译捕获 - CS2 Workshop Tools Localizer CN");
    resize(1020, 680);
    setMinimumSize(850, 520);

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(8);

    // =========================================================================
    // 主体左右分栏 (QSplitter)
    // =========================================================================
    QSplitter* splitter = new QSplitter(Qt::Horizontal, this);

    // -------------------------------------------------------------------------
    // 左侧：双 Tab 容器
    // -------------------------------------------------------------------------
    m_tabWidget = new QTabWidget(splitter);

    // Tab 1: 📚 已收录词典库
    QWidget* tab1Widget = new QWidget(m_tabWidget);
    QVBoxLayout* tab1Layout = new QVBoxLayout(tab1Widget);
    tab1Layout->setContentsMargins(6, 6, 6, 6);
    tab1Layout->setSpacing(6);

    // Tab 1 专属检索工具栏（与捕获池筛选栏保持对齐一致）
    QHBoxLayout* searchCtrlLayout = new QHBoxLayout();
    searchCtrlLayout->setSpacing(6);

    QLabel* moduleFilterLabel = new QLabel("目标DLL:", tab1Widget);
    m_moduleFilterCombo = new QComboBox(tab1Widget);
    m_moduleFilterCombo->addItem("全部");
    m_moduleFilterCombo->setMinimumWidth(110);

    m_searchEdit = new QLineEdit(tab1Widget);
    m_searchEdit->setPlaceholderText("搜索已收录词条 (需 >=4 个英文字符或 >=2 个汉字)...");
    m_searchEdit->setClearButtonEnabled(true);

    m_searchBtn = new QPushButton("🔍 搜索", tab1Widget);
    m_searchBtn->setStyleSheet("background-color: #238636; color: white; font-weight: bold; height: 26px; border-radius: 4px; padding: 2px 14px;");

    searchCtrlLayout->addWidget(moduleFilterLabel);
    searchCtrlLayout->addWidget(m_moduleFilterCombo);
    searchCtrlLayout->addWidget(m_searchEdit, 1);
    searchCtrlLayout->addWidget(m_searchBtn);
    tab1Layout->addLayout(searchCtrlLayout);

    m_masterTable = new QTableWidget(0, 4, tab1Widget);
    m_masterTable->setHorizontalHeaderLabels({"来源", "作用域/DLL", "英文原文", "中文译文"});
    m_masterTable->setTextElideMode(Qt::ElideRight);
    m_masterTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_masterTable->setColumnWidth(0, 70);
    m_masterTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_masterTable->setColumnWidth(1, 90);
    m_masterTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_masterTable->setColumnWidth(2, 220);
    m_masterTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_masterTable->setColumnWidth(3, 220);
    m_masterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_masterTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_masterTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_masterTable->setAlternatingRowColors(true);
    tab1Layout->addWidget(m_masterTable);

    m_tabWidget->addTab(tab1Widget, "📚 已收录词典库 (Search)");

    // Tab 2: 🎯 运行时捕获池
    QWidget* tab2Widget = new QWidget(m_tabWidget);
    QVBoxLayout* tab2Layout = new QVBoxLayout(tab2Widget);
    tab2Layout->setContentsMargins(6, 6, 6, 6);
    tab2Layout->setSpacing(6);

    QHBoxLayout* captureCtrlLayout = new QHBoxLayout();
    captureCtrlLayout->setSpacing(6);

    m_captureFilterEdit = new QLineEdit(tab2Widget);
    m_captureFilterEdit->setPlaceholderText("筛选捕获的文本...");
    m_captureFilterEdit->setClearButtonEnabled(true);

    m_refreshCaptureBtn = new QPushButton("🔄 立即刷新", tab2Widget);
    m_refreshCaptureBtn->setStyleSheet("background-color: #1f6feb; color: white; border-radius: 4px; padding: 4px 10px;");

    m_clearCaptureBtn = new QPushButton("🧹 清空捕获", tab2Widget);
    m_clearCaptureBtn->setStyleSheet("background-color: #484f58; color: white; border-radius: 4px; padding: 4px 10px;");

    captureCtrlLayout->addWidget(m_captureFilterEdit, 1);
    captureCtrlLayout->addWidget(m_refreshCaptureBtn);
    captureCtrlLayout->addWidget(m_clearCaptureBtn);
    tab2Layout->addLayout(captureCtrlLayout);

    m_captureTable = new QTableWidget(0, 4, tab2Widget);
    m_captureTable->setHorizontalHeaderLabels({"来源 DLL", "未翻译原生英文", "频次", "最后触发时间"});
    m_captureTable->setTextElideMode(Qt::ElideRight);
    m_captureTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_captureTable->setColumnWidth(0, 95);
    m_captureTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_captureTable->setColumnWidth(1, 335);
    m_captureTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_captureTable->setColumnWidth(2, 65);
    m_captureTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_captureTable->setColumnWidth(3, 105);
    m_captureTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_captureTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_captureTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_captureTable->setAlternatingRowColors(true);
    tab2Layout->addWidget(m_captureTable);

    m_tabWidget->addTab(tab2Widget, "🎯 运行时捕获池 (Live Capture)");

    splitter->addWidget(m_tabWidget);

    // -------------------------------------------------------------------------
    // 右侧：条目详情与编辑操作卡片
    // -------------------------------------------------------------------------
    QGroupBox* rightGroup = new QGroupBox("【条目详情与编辑】", splitter);
    QVBoxLayout* rightLayout = new QVBoxLayout(rightGroup);
    rightLayout->setContentsMargins(12, 12, 12, 12);
    rightLayout->setSpacing(10);

    QGridLayout* formLayout = new QGridLayout();
    formLayout->setSpacing(8);

    formLayout->addWidget(new QLabel("目标词典:", rightGroup), 0, 0);
    m_dictTypeCombo = new QComboBox(rightGroup);
    m_dictTypeCombo->addItem("界面词典 (qt_translations.jsonc)", static_cast<int>(DictType::QtUI));
    m_dictTypeCombo->addItem("FGD 实体词典 (fgd_translations.jsonc)", static_cast<int>(DictType::FGD));
    formLayout->addWidget(m_dictTypeCombo, 0, 1);

    formLayout->addWidget(new QLabel("目标 DLL:", rightGroup), 1, 0);
    m_targetModuleCombo = new QComboBox(rightGroup);
    m_targetModuleCombo->setEditable(true);
    m_targetModuleCombo->addItem("common");
    m_targetModuleCombo->addItem("hammer");
    formLayout->addWidget(m_targetModuleCombo, 1, 1);

    formLayout->addWidget(new QLabel("英文原文:", rightGroup), 2, 0, Qt::AlignTop);
    m_englishKeyEdit = new QPlainTextEdit(rightGroup);
    m_englishKeyEdit->setPlaceholderText("例如: Undo, Clipping Tool...");
    m_englishKeyEdit->setMinimumHeight(90);
    formLayout->addWidget(m_englishKeyEdit, 2, 1);

    formLayout->addWidget(new QLabel("中文译文:", rightGroup), 3, 0, Qt::AlignTop);
    m_chineseValueEdit = new QPlainTextEdit(rightGroup);
    m_chineseValueEdit->setPlaceholderText("请输入对应中文译文...");
    m_chineseValueEdit->setMinimumHeight(90);
    formLayout->addWidget(m_chineseValueEdit, 3, 1);

    rightLayout->addLayout(formLayout);

    // 操作按钮
    QHBoxLayout* btnRow = new QHBoxLayout();
    btnRow->setSpacing(8);

    m_addBtn = new QPushButton("➕ 加入/新增词条", rightGroup);
    m_addBtn->setStyleSheet("background-color: #238636; color: white; font-weight: bold; height: 32px; border-radius: 4px;");

    m_saveBtn = new QPushButton("💾 保存修改", rightGroup);
    m_saveBtn->setStyleSheet("background-color: #1f6feb; color: white; font-weight: bold; height: 32px; border-radius: 4px;");

    m_deleteBtn = new QPushButton("🗑️ 删除此条目", rightGroup);
    m_deleteBtn->setStyleSheet("background-color: #da3633; color: white; font-weight: bold; height: 32px; border-radius: 4px;");

    btnRow->addWidget(m_addBtn);
    btnRow->addWidget(m_saveBtn);
    btnRow->addWidget(m_deleteBtn);
    rightLayout->addLayout(btnRow);

    rightLayout->addStretch(1);

    splitter->addWidget(rightGroup);
    splitter->setStretchFactor(0, 62);
    splitter->setStretchFactor(1, 38);

    rootLayout->addWidget(splitter, 1);

    // =========================================================================
    // 3. 底部状态反馈栏
    // =========================================================================
    m_statusLabel = new QLabel("就绪 - 输入关键字以检索词典，或切换到捕获池查看 Hammer 实时未收录文本", this);
    m_statusLabel->setStyleSheet("color: #8b949e; padding: 4px 6px; background-color: #161b22; border-radius: 4px;");
    rootLayout->addWidget(m_statusLabel);

    // 信号槽连接
    connect(m_dictTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &DictionaryEditorWindow::onDictTypeChanged);
    connect(m_searchBtn, &QPushButton::clicked, this, &DictionaryEditorWindow::onSearchClicked);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &DictionaryEditorWindow::onSearchReturnPressed);
    connect(m_masterTable, &QTableWidget::itemSelectionChanged, this, &DictionaryEditorWindow::onMasterTableSelectionChanged);
    connect(m_captureTable, &QTableWidget::itemSelectionChanged, this, &DictionaryEditorWindow::onCaptureTableSelectionChanged);
    connect(m_refreshCaptureBtn, &QPushButton::clicked, this, &DictionaryEditorWindow::onRefreshCaptureClicked);
    connect(m_clearCaptureBtn, &QPushButton::clicked, this, &DictionaryEditorWindow::onClearCaptureClicked);
    connect(m_captureFilterEdit, &QLineEdit::textChanged, this, &DictionaryEditorWindow::onRefreshCaptureClicked);
    connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](int idx) { if (idx == 1) onRefreshCaptureClicked(); });
    connect(m_addBtn, &QPushButton::clicked, this, &DictionaryEditorWindow::onAddClicked);
    connect(m_saveBtn, &QPushButton::clicked, this, &DictionaryEditorWindow::onSaveClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &DictionaryEditorWindow::onDeleteClicked);
}

void DictionaryEditorWindow::populateModuleCombos() {
    auto dictType = static_cast<DictType>(m_dictTypeCombo->currentData().toInt());
    auto modules = m_service->getAvailableModules(dictType);

    m_moduleFilterCombo->clear();
    m_moduleFilterCombo->addItem("全部");
    m_targetModuleCombo->clear();

    for (const auto& mod : modules) {
        QString qm = QString::fromStdString(mod);
        m_moduleFilterCombo->addItem(qm);
        m_targetModuleCombo->addItem(qm);
    }
}

void DictionaryEditorWindow::onDictTypeChanged(int index) {
    auto dictType = static_cast<DictType>(m_dictTypeCombo->itemData(index).toInt());
    if (dictType == DictType::FGD) {
        // FGD 模式：根据设计共识，隔离并禁用 Tab 2 捕获池，锁定目标 DLL 为全局
        m_tabWidget->setTabEnabled(1, false);
        m_tabWidget->setCurrentIndex(0);
        m_moduleFilterCombo->setEnabled(false);
        m_targetModuleCombo->setEnabled(false);
        m_targetModuleCombo->setCurrentText("[全局 (无DLL分组)]");
    } else {
        // Qt 界面词典模式：开放 Tab 2 与模块作用域
        m_tabWidget->setTabEnabled(1, true);
        m_moduleFilterCombo->setEnabled(true);
        m_targetModuleCombo->setEnabled(true);
        populateModuleCombos();
    }
    m_masterTable->setRowCount(0);
    m_currentSearchResults.clear();
    setStatus("已切换词典类型，请输入关键字进行检索。");
}

void DictionaryEditorWindow::onSearchReturnPressed() {
    onSearchClicked();
}

void DictionaryEditorWindow::onSearchClicked() {
    QString query = m_searchEdit->text().trimmed();
    QString err;
    if (!DictionaryEditorService::ValidateSearchQuery(query, err)) {
        setStatus("⚠ " + err, "#d29922");
        return;
    }

    auto dictType = static_cast<DictType>(m_dictTypeCombo->currentData().toInt());
    QString modFilter = m_moduleFilterCombo->currentText();

    m_currentSearchResults = m_service->searchEntries(dictType, modFilter, query, err);
    m_masterTable->setRowCount(0);
    m_selectedSearchRow = -1;

    if (!err.isEmpty()) {
        setStatus("❌ " + err, "#f85149");
        return;
    }

    m_masterTable->setUpdatesEnabled(false);
    m_masterTable->blockSignals(true);
    m_masterTable->setRowCount(static_cast<int>(m_currentSearchResults.size()));
    for (int i = 0; i < static_cast<int>(m_currentSearchResults.size()); ++i) {
        const auto& rec = m_currentSearchResults[i];

        QString srcStr = (rec.source == DictSource::Master) ? "主词典" : "Fallback";
        auto* srcItem = new QTableWidgetItem(srcStr);
        if (rec.source == DictSource::Master) {
            srcItem->setForeground(QColor("#7ee787"));
        } else {
            srcItem->setForeground(QColor("#e3b341"));
        }

        QString modStr = rec.module.empty() ? "[全局]" : QString::fromStdString(rec.module);
        auto* modItem = new QTableWidgetItem(modStr);
        QString keyStr = QString::fromUtf8(rec.key.c_str());
        QString valStr = QString::fromUtf8(rec.value.c_str());
        auto* keyItem = new QTableWidgetItem(keyStr);
        keyItem->setToolTip(keyStr);
        auto* valItem = new QTableWidgetItem(valStr);
        valItem->setToolTip(valStr);

        m_masterTable->setItem(i, 0, srcItem);
        m_masterTable->setItem(i, 1, modItem);
        m_masterTable->setItem(i, 2, keyItem);
        m_masterTable->setItem(i, 3, valItem);
    }
    m_masterTable->blockSignals(false);
    m_masterTable->setUpdatesEnabled(true);

    setStatus(QString("检索完成，共找到 %1 条匹配词条。").arg(m_currentSearchResults.size()), "#7ee787");
}

void DictionaryEditorWindow::onMasterTableSelectionChanged() {
    int row = m_masterTable->currentRow();
    if (row < 0 || row >= static_cast<int>(m_currentSearchResults.size())) return;

    m_selectedSearchRow = row;
    const auto& rec = m_currentSearchResults[row];

    auto dictType = static_cast<DictType>(m_dictTypeCombo->currentData().toInt());
    if (dictType == DictType::QtUI) {
        m_targetModuleCombo->setCurrentText(rec.module.empty() ? "common" : QString::fromStdString(rec.module));
    }
    m_englishKeyEdit->setPlainText(QString::fromUtf8(rec.key.c_str()));
    m_chineseValueEdit->setPlainText(QString::fromUtf8(rec.value.c_str()));
}

void DictionaryEditorWindow::onCaptureTableSelectionChanged() {
    int row = m_captureTable->currentRow();
    if (row < 0 || row >= static_cast<int>(m_currentCaptureResults.size())) return;

    m_selectedCaptureRow = row;
    const auto& item = m_currentCaptureResults[row];

    QString modStr = QString::fromStdWString(item.module);
    if (modStr.isEmpty() || modStr == "unknown") modStr = "common";

    m_targetModuleCombo->setCurrentText(modStr);
    m_englishKeyEdit->setPlainText(QString::fromUtf8(item.text.c_str()));
    m_chineseValueEdit->clear();
    m_chineseValueEdit->setFocus();

    setStatus(QString("已带入捕获条目: \"%1\" (来自 %2)，可直接在译文框输入中文并点击【➕ 加入/新增】")
        .arg(QString::fromUtf8(item.text.c_str()), modStr), "#58a6ff");
}

void DictionaryEditorWindow::onRefreshCaptureClicked() {
    auto entries = m_service->fetchCaptureEntries();
    QString filter = m_captureFilterEdit->text().trimmed().toLower();

    m_currentCaptureResults.clear();
    for (const auto& e : entries) {
        if (filter.isEmpty() ||
            QString::fromUtf8(e.text.c_str()).toLower().contains(filter) ||
            QString::fromStdWString(e.module).toLower().contains(filter)) {
            m_currentCaptureResults.push_back(e);
        }
    }

    // 默认按最后触发时间降序排列（最新在上）
    std::sort(m_currentCaptureResults.begin(), m_currentCaptureResults.end(), [](const auto& a, const auto& b) {
        return a.lastTimestamp > b.lastTimestamp;
    });

    m_captureTable->setUpdatesEnabled(false);
    m_captureTable->blockSignals(true);
    m_captureTable->setRowCount(static_cast<int>(m_currentCaptureResults.size()));
    for (int i = 0; i < static_cast<int>(m_currentCaptureResults.size()); ++i) {
        const auto& item = m_currentCaptureResults[i];

        auto* modItem = new QTableWidgetItem(QString::fromStdWString(item.module));
        QString fullCaptureText = QString::fromUtf8(item.text.c_str());
        auto* textItem = new QTableWidgetItem(fullCaptureText);
        textItem->setToolTip(fullCaptureText);
        auto* hitItem = new QTableWidgetItem(QString::number(item.hitCount));
        hitItem->setTextAlignment(Qt::AlignCenter);

        // 时间戳格式化
        QString timeStr = QString("%1 ms 前").arg(GetTickCount64() - item.lastTimestamp);
        auto* timeItem = new QTableWidgetItem(timeStr);
        timeItem->setTextAlignment(Qt::AlignCenter);

        m_captureTable->setItem(i, 0, modItem);
        m_captureTable->setItem(i, 1, textItem);
        m_captureTable->setItem(i, 2, hitItem);
        m_captureTable->setItem(i, 3, timeItem);
    }
    m_captureTable->blockSignals(false);
    m_captureTable->setUpdatesEnabled(true);
}

void DictionaryEditorWindow::onClearCaptureClicked() {
    if (QMessageBox::question(this, "清空捕获池", "确定要清空当前的实时捕获池吗？") == QMessageBox::Yes) {
        m_service->clearCaptureEntries();
        m_currentCaptureResults.clear();
        m_captureTable->setRowCount(0);
        setStatus("✓ 已清空运行时捕获池。", "#7ee787");
    }
}

void DictionaryEditorWindow::onSaveClicked() {
    QString key = m_englishKeyEdit->toPlainText().trimmed();
    QString val = m_chineseValueEdit->toPlainText().trimmed();

    if (key.isEmpty() || val.isEmpty()) {
        setStatus("⚠ 英文原文与中文译文均不能为空！", "#d29922");
        return;
    }

    auto dictType = static_cast<DictType>(m_dictTypeCombo->currentData().toInt());
    std::string mod = (dictType == DictType::QtUI) ? m_targetModuleCombo->currentText().trimmed().toStdString() : "";

    DictEntryRecord rec;
    rec.key = key.toStdString();
    rec.value = val.toStdString();
    rec.module = mod;

    // 判断来源是否是检索出的 Fallback
    if (m_selectedSearchRow >= 0 && m_selectedSearchRow < static_cast<int>(m_currentSearchResults.size())) {
        rec.source = m_currentSearchResults[m_selectedSearchRow].source;
    } else {
        rec.source = DictSource::Master;
    }

    bool useMt = m_getMachineTransEnabled ? m_getMachineTransEnabled() : true;
    auto res = m_service->saveEntry(dictType, rec, useMt);

    if (res.success) {
        setStatus(res.message, "#7ee787");
        // 刷新检索列表显示最新结果
        if (!m_searchEdit->text().trimmed().isEmpty()) {
            onSearchClicked();
        }
    } else {
        setStatus("❌ " + res.message, "#f85149");
    }
}

void DictionaryEditorWindow::onAddClicked() {
    QString key = m_englishKeyEdit->toPlainText().trimmed();
    QString val = m_chineseValueEdit->toPlainText().trimmed();

    if (key.isEmpty() || val.isEmpty()) {
        setStatus("⚠ 英文原文与中文译文均不能为空！", "#d29922");
        return;
    }

    auto dictType = static_cast<DictType>(m_dictTypeCombo->currentData().toInt());
    std::string mod = (dictType == DictType::QtUI) ? m_targetModuleCombo->currentText().trimmed().toStdString() : "";

    bool useMt = m_getMachineTransEnabled ? m_getMachineTransEnabled() : true;
    auto res = m_service->addEntry(dictType, mod, key.toStdString(), val.toStdString(), useMt);

    if (res.success) {
        setStatus(res.message, "#7ee787");
        if (dictType == DictType::QtUI) {
            populateModuleCombos();
            m_targetModuleCombo->setCurrentText(QString::fromStdString(mod));
        }
        if (!m_searchEdit->text().trimmed().isEmpty()) {
            onSearchClicked();
        }
    } else {
        setStatus("❌ " + res.message, "#f85149");
    }
}

void DictionaryEditorWindow::onDeleteClicked() {
    QString key = m_englishKeyEdit->toPlainText().trimmed();
    if (key.isEmpty()) {
        setStatus("⚠ 当前未选中任何有效词条！", "#d29922");
        return;
    }

    if (QMessageBox::question(
            this,
            "确认删除",
            QString("确定要物理删除词条 \"%1\" 吗？此操作不可逆。").arg(key),
            QMessageBox::Yes | QMessageBox::No
        ) != QMessageBox::Yes) {
        return;
    }

    auto dictType = static_cast<DictType>(m_dictTypeCombo->currentData().toInt());
    std::string mod = (dictType == DictType::QtUI) ? m_targetModuleCombo->currentText().trimmed().toStdString() : "";

    DictEntryRecord rec;
    rec.key = key.toStdString();
    rec.module = mod;
    if (m_selectedSearchRow >= 0 && m_selectedSearchRow < static_cast<int>(m_currentSearchResults.size())) {
        rec.source = m_currentSearchResults[m_selectedSearchRow].source;
    } else {
        rec.source = DictSource::Master;
    }

    bool useMt = m_getMachineTransEnabled ? m_getMachineTransEnabled() : true;
    auto res = m_service->deleteEntry(dictType, rec, useMt);

    if (res.success) {
        setStatus(res.message, "#7ee787");
        m_englishKeyEdit->clear();
        m_chineseValueEdit->clear();
        if (!m_searchEdit->text().trimmed().isEmpty()) {
            onSearchClicked();
        }
    } else {
        setStatus("❌ " + res.message, "#f85149");
    }
}

void DictionaryEditorWindow::setStatus(const QString& msg, const QString& colorHex) {
    m_statusLabel->setText(msg);
    m_statusLabel->setStyleSheet(QString("color: %1; padding: 4px 6px; background-color: #161b22; border-radius: 4px; font-weight: bold;").arg(colorHex));
}

void DictionaryEditorWindow::applyDarkTheme() {
    setStyleSheet(
        "QWidget {"
        "  background-color: #0d1117;"
        "  color: #c9d1d9;"
        "  font-family: 'Segoe UI', 'Microsoft YaHei', sans-serif;"
        "  font-size: 12px;"
        "}"
        "QGroupBox {"
        "  border: 1px solid #30363d;"
        "  border-radius: 6px;"
        "  margin-top: 10px;"
        "  padding-top: 10px;"
        "  font-weight: bold;"
        "  color: #8b949e;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  subcontrol-position: top left;"
        "  left: 10px;"
        "  padding: 0 4px;"
        "}"
        "QLineEdit, QPlainTextEdit, QComboBox {"
        "  background-color: #161b22;"
        "  border: 1px solid #30363d;"
        "  border-radius: 4px;"
        "  padding: 4px;"
        "  color: #f0f6fc;"
        "}"
        "QLineEdit:focus, QPlainTextEdit:focus, QComboBox:focus {"
        "  border: 1px solid #58a6ff;"
        "}"
        "QTableWidget {"
        "  background-color: #161b22;"
        "  alternate-background-color: #0d1117;"
        "  border: 1px solid #30363d;"
        "  gridline-color: #21262d;"
        "  selection-background-color: #1f6feb;"
        "  selection-color: white;"
        "}"
        "QHeaderView::section {"
        "  background-color: #21262d;"
        "  color: #8b949e;"
        "  padding: 4px;"
        "  border: 1px solid #30363d;"
        "  font-weight: bold;"
        "}"
        "QTabWidget::pane {"
        "  border: 1px solid #30363d;"
        "  background-color: #0d1117;"
        "}"
        "QTabBar::tab {"
        "  background-color: #161b22;"
        "  color: #8b949e;"
        "  border: 1px solid #30363d;"
        "  padding: 6px 14px;"
        "  margin-right: 2px;"
        "  border-top-left-radius: 4px;"
        "  border-top-right-radius: 4px;"
        "}"
        "QTabBar::tab:selected {"
        "  background-color: #0d1117;"
        "  color: #58a6ff;"
        "  border-bottom-color: #0d1117;"
        "  font-weight: bold;"
        "}"
        "QSplitter::handle {"
        "  background-color: #30363d;"
        "  width: 2px;"
        "}"
    );
}

void DictionaryEditorWindow::closeEvent(QCloseEvent* event) {
    event->accept();
}
