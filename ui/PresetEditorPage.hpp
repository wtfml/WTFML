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
// ★ 它是一个**整页**（`QDialog`，由 `OverlayHost::showPage()` 铺到内容区上），打开它的方式
// 和「实例设置」同一套外壳：页眉（返回箭头 + 标题）由覆盖层自己搭，左上角不再有自己那颗
// 「返回」按钮。`reject()` 被接管 —— 它**不关页**，只把"用户要走了"交给控制器，
// 由控制器问"保存为草稿 / 保存为预设 / 不保存"，然后才 `accept()` 收页。
class PresetEditorPage final : public QDialog {
  public:
    explicit PresetEditorPage(const QJsonObject &document, QWidget *parent = nullptr);
    /// 返回箭头 / Esc：交给控制器（它决定保存不保存，再由它 accept()）。
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
