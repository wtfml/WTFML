#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include "WorldSettingsPage.hpp"

class MainWindow;
class InstanceSettingsDialog;
namespace WorldFiles { struct LevelData; }

class InstanceEditorController final : public QObject {
    Q_OBJECT
public:
    void refreshResourcePacks(InstanceSettingsDialog *dialog, const QString &instanceId);
    void saveResourcePacks(InstanceSettingsDialog *dialog, const QString &instanceId,
                           const QStringList &ids, const QByteArray &expected, bool expectedExists);
    explicit InstanceEditorController(MainWindow *owner);
    QString latestConfigBackup(const QString &instanceId, const QString &path) const;
    void pruneConfigBackups(const QString &instanceId, int keep);
    void openConfigFile(InstanceSettingsDialog *dialog, const QString &path);
    void saveConfigFile(InstanceSettingsDialog *dialog, const QString &path, const QByteArray &bytes);
    void restoreConfigFile(InstanceSettingsDialog *dialog, const QString &path);
    void showConfigHistory(InstanceSettingsDialog *dialog, const QString &path);
    bool backupWorldLevelData(const QString &worldDir, QString *problemOut);
    QString latestWorldBackup(const QString &worldDir) const;
    void showWorldSettingsPage(InstanceSettingsDialog *dialog, const QString &worldDir);
    void leaveWorldSettings(InstanceSettingsDialog *dialog);
    void saveWorldSettings(InstanceSettingsDialog *dialog);
    void leaveConfigEditor(InstanceSettingsDialog *dialog);
    bool confirmDiscardConfigEdits();
    bool confirmDiscardWorldEdits();
    static WorldSettingsPage::WorldView worldViewOf(const QString &worldDir, const WorldFiles::LevelData &data);
    static bool applyWorldEdits(WorldFiles::LevelData &data, const WorldSettingsPage::Delta &delta, QString *problem);
private:
    void loadConfigBackup(InstanceSettingsDialog *dialog, const QString &path,
                          const QString &backupPath);
    MainWindow *m_owner = nullptr;
};

