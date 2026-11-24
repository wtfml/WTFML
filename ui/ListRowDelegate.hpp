/**
 * @file ListRowDelegate.hpp
 * @brief 设计稿「列表行」（components.html .row）：40 封面 + 两行字 + 行尾元信息；
 *        悬停时行尾换成快捷操作（文件夹 / 更多 / 启动胶囊），选中 = 强调色淡底（不画指示条）。
 *
 * 数据：DisplayRole = 标题，DecorationRole = 封面（QIcon / QPixmap），SubtitleRole = 第二行，
 * MetaRole = 行尾那句（如「2 小时前」）。行高 60（设计稿令牌），用 install() 挂到视图上。
 * 可选：CoverRole 给了就用它当封面（DecorationRole 留给别处用）；MetaIconRole 给了，元信息改成
 * 行中部一列「小图标 + 字」（设计稿 manage 的加载器那一列），悬停时也不让位。
 * 服务端行借同一个代理：LaunchTextRole / LaunchKindRole 把第三格换成「启动 / 停止 / 修复 / 停止中…」，
 * ActionTipRole 给三格各一句悬停说明（文件夹、更多、第三格）。
 */
#pragma once

#include <QPersistentModelIndex>
#include <QSet>
#include <QStyledItemDelegate>

class QAbstractItemView;

class ListRowDelegate : public QStyledItemDelegate {
    Q_OBJECT
public:
    enum Role {
        SubtitleRole = Qt::UserRole + 0x4C52,
        MetaRole,
        CoverRole,
        MetaIconRole,
        LaunchTextRole,  ///< 第三格的字；空 = 「启动」
        LaunchKindRole,  ///< LaunchKind（int）；空 = Start
        ActionTipRole,   ///< QStringList{文件夹, 更多, 第三格} 的悬停说明
    };
    enum class Action { Folder, More, Launch };
    enum class LaunchKind { Start = 0, Stop, Repair, Busy };

    using QStyledItemDelegate::QStyledItemDelegate;

    /// 挂到视图上：设本委托、打开悬停（快捷操作只在悬停那一行出现）。
    static ListRowDelegate *install(QAbstractItemView *view);

    static constexpr int rowHeight() { return 60; }
    static constexpr int coverSize() { return 40; }
    /// 悬停那一行里某颗快捷操作占的矩形（@p row 是条目矩形）。画和点都认它。
    static QRect actionRect(const QRect &row, Action action);

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    /// 上一次画的时候这一行的标题收了省略号（悬停提示据此给全名）。
    bool titleElided(const QModelIndex &index) const { return m_elidedTitles.contains(index); }
    bool subtitleElided(const QModelIndex &index) const { return m_elidedSubs.contains(index); }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    /// 三格的说明；标题或第二行被省略时给全文（后面接条目自己的 ToolTipRole）。
    bool helpEvent(QHelpEvent *event, QAbstractItemView *view, const QStyleOptionViewItem &option,
                   const QModelIndex &index) override;

signals:
    void actionTriggered(const QModelIndex &index, ListRowDelegate::Action action);

protected:
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

private:
    mutable QSet<QPersistentModelIndex> m_elidedTitles;
    mutable QSet<QPersistentModelIndex> m_elidedSubs;
};
