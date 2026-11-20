#pragma once

#include <QWidget>
#include <QString>
#include <vector>
#include <memory>

#include "service/dictionary_editor_service.h"

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;
class QLabel;
class QCheckBox;
class QTimer;

class DictionaryEditorWindow : public QWidget {
    Q_OBJECT

public:
    explicit DictionaryEditorWindow(
        DictionaryEditorService* service,
        std::function<bool()> getMachineTransEnabled,
        QWidget* parent = nullptr
    );
    ~DictionaryEditorWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onDictTypeChanged(int index);
    void onSearchClicked();
    void onSearchReturnPressed();
    void onMasterTableSelectionChanged();
    void onCaptureTableSelectionChanged();
    void onRefreshCaptureClicked();
    void onClearCaptureClicked();
    void onSaveClicked();
    void onAddClicked();
    void onDeleteClicked();

private:
    void setupUi();
    void populateModuleCombos();
    void applyDarkTheme();
    void setStatus(const QString& msg, const QString& colorHex = "#8b949e");
    bool checkUnsavedDraft();

    DictionaryEditorService* m_service{nullptr};
    std::function<bool()> m_getMachineTransEnabled;

    QComboBox* m_moduleFilterCombo{nullptr};
    QLineEdit* m_searchEdit{nullptr};
    QPushButton* m_searchBtn{nullptr};

    QTabWidget* m_tabWidget{nullptr};
    QTableWidget* m_masterTable{nullptr};
    QTableWidget* m_captureTable{nullptr};

    QLineEdit* m_captureFilterEdit{nullptr};
    QPushButton* m_refreshCaptureBtn{nullptr};
    QPushButton* m_clearCaptureBtn{nullptr};

    QComboBox* m_dictTypeCombo{nullptr};
    QComboBox* m_targetModuleCombo{nullptr};
    QPlainTextEdit* m_englishKeyEdit{nullptr};
    QPlainTextEdit* m_chineseValueEdit{nullptr};
    QPushButton* m_addBtn{nullptr};
    QPushButton* m_saveBtn{nullptr};
    QPushButton* m_deleteBtn{nullptr};

    QLabel* m_statusLabel{nullptr};

    std::vector<DictEntryRecord> m_currentSearchResults;
    std::vector<capture::UntranslatedCaptureReader::Item> m_currentCaptureResults;
    int m_selectedSearchRow{-1};
    int m_selectedCaptureRow{-1};
    bool m_isDirty{false};
};
