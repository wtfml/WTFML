#pragma once
#include <QHash>
#include <QObject>
#include <QPixmap>
#include "download/ResourceCatalog.hpp"

class MainWindow;
class QTreeWidget;
class QTreeWidgetItem;

// Owns this workflow and its transient state; shared session caches stay with MainWindow.
class ResourceSearchController final : public QObject {
public:
    explicit ResourceSearchController(MainWindow *owner);
    void startResourceSearch(ResourceCatalog::Kind kind);
    void fillResourceHitRows(QTreeWidget *tree, ResourceCatalog::Kind kind,
                                     const ResourceCatalog::SearchResult &result);
    void onResourceSearchFinished(const QString &requestKey,
                                         const ResourceCatalog::SearchResult &result);
    void onResourceIconReady(const QString &iconId, const QPixmap &icon);
    void onResourceItemActivated(QTreeWidgetItem *item);
    /// 展开游戏版本时按当前下载来路刷新列表，返回是否已发起请求。
    bool requestResourceVersions(QTreeWidgetItem *gameItem);
    void fillResourceVersionRows(QTreeWidgetItem *gameItem,
                                        const QList<ResourceCatalog::VersionEntry> &versions,
                                        const QString &problem);
    void onResourceVersionsFinished(const QString &requestKey,
                                           const QList<ResourceCatalog::VersionEntry> &versions,
                                           const QString &problem);
    void downloadResourceVersion(QTreeWidgetItem *item);
private:
    MainWindow &m_owner;
    QHash<QString, QString> m_resourceQueries;
};
