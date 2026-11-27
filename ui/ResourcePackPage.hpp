#pragma once
#include <QByteArray>
#include <QList>
#include <QStringList>
#include <QWidget>

class QListWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QScrollArea;

class ResourcePackPage final : public QWidget {
    Q_OBJECT
public:
    struct Pack { QString id; QString title; QString detail; };
    explicit ResourcePackPage(QWidget *parent = nullptr);
    void setPacks(const QList<Pack> &packs, const QStringList &selected,
                  const QByteArray &options, bool exists, const QString &problem = {});
    QStringList selectedIds() const;
signals:
    void refreshRequested();
    void selectionRequested(const QStringList &ids, const QByteArray &expected, bool expectedExists);
private:
    void submit();
    void updateButtons();
    void filter();
    void moveCurrent(int delta);
    void fitLists();
    QScrollArea *m_scroll = nullptr;
    QListWidget *m_available = nullptr;
    QListWidget *m_selected = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_note = nullptr;
    QPushButton *m_enable = nullptr;
    QPushButton *m_disable = nullptr;
    QPushButton *m_up = nullptr;
    QPushButton *m_down = nullptr;
    QByteArray m_expected;
    bool m_expectedExists = false;
    bool m_loading = false;
    bool m_editable = false;
    bool m_pending = false;
};
