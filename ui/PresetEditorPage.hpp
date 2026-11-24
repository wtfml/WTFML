#pragma once
#include <QHash>
#include <QJsonObject>
#include <QDialog>
#include <functional>
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QCheckBox;
class QLabel;
class QTableWidget;
class PresetOptionsEditor;
class PresetModEditor;

// One in-memory document; no sub-page writes to disk.
//
// 已有预设可从详情左栏直接编辑。新建等独立入口由 `OverlayHost::showPage()` 打开整页。
// `reject()` 把离开请求交给控制器，统一检查未保存的修改。
class PresetEditorPage final : public QDialog {
  public:
    enum class Presentation { FullPage, Embedded };
    explicit PresetEditorPage(const QJsonObject &document, QWidget *parent = nullptr,
                              Presentation presentation = Presentation::FullPage);
    static QStringList sectionTitles();
    void selectSection(int section);
    /// 返回箭头 / Esc：由控制器统一检查未保存的修改。
    void reject() override;
    QJsonObject document() const;
    void setDocument(const QJsonObject &document);
    bool isDirty() const;
    void markSaved(const QJsonObject &document);
    void showProblem(const QString &text);
    std::function<void(bool)> saveRequested;
    std::function<bool(const QString &)> confirmReset;
    std::function<void()> backRequested;

  private:
    void rebuildPages(const QJsonObject &document);
    void addPack(const QString &part);
    QJsonObject m_original, m_base;
    QLineEdit *m_name = nullptr;
    QLineEdit *m_icon = nullptr;
    QListWidget *m_navigation = nullptr;
    QStackedWidget *m_pages = nullptr;
    QLabel *m_problem = nullptr;
    PresetOptionsEditor *m_options = nullptr;
    PresetOptionsEditor *m_keys = nullptr;
    PresetModEditor *m_mods = nullptr;
    QHash<QString, QCheckBox *> m_enabled;
    QHash<QString, QTableWidget *> m_packs;
};
