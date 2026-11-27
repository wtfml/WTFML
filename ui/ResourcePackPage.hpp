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
class QTimer;

class ResourcePackPage final : public QWidget {
    Q_OBJECT
public:
    struct Pack { QString id; QString title; QString detail; };
    explicit ResourcePackPage(QWidget *parent = nullptr);
    void setPacks(const QList<Pack> &packs, const QStringList &selected,
                  const QByteArray &options, bool exists, const QString &problem = {});
    QStringList selectedIds() const;
    /// 只装在「已启用」列表的视口上：拖到外层视口上下边缘时滚外层。
    bool eventFilter(QObject *watched, QEvent *event) override;
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
    QTimer *m_dragScroll = nullptr;
    int m_dragStep = 0;  // 每一拍滚多少像素，负数向上
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
