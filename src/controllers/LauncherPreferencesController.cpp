#include "controllers/LauncherPreferencesController.hpp"

#include "MainWindowUI.hpp"
#include "download/ResourceStore.hpp"
#include "manage/VersionManager.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "InlineMessage.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QSpinBox>

#include <utility>

LauncherPreferencesController::LauncherPreferencesController(
    MainWindowUI *ui, LauncherSettings *settings, GlobalJavaSettings *javaSettings,
    VersionManager *versions, ResourceStore *resources, Host host, QObject *parent)
    : QObject(parent), m_ui(ui), m_settings(settings), m_javaSettings(javaSettings),
      m_versions(versions), m_resources(resources), m_host(std::move(host)) {}

void LauncherPreferencesController::log(const QString &message) const {
    if (m_host.log) m_host.log(message);
}

void LauncherPreferencesController::launcherBehaviourChanged() {
    m_settings->setLauncherBehaviour(
        LauncherBehaviour(m_ui->getLauncherBehaviourCombo()->currentData().toInt()));
}

void LauncherPreferencesController::downloadSourceChanged() {
    m_settings->setDownloadSource(
        DownloadSource(m_ui->getDownloadSourceCombo()->currentData().toInt()));
    syncDownloadSource();
}

void LauncherPreferencesController::downloadConcurrencyChanged() {
    m_settings->setDownloadConcurrency(
        m_ui->getDownloadConcurrencyCombo()->currentData().toInt());
    syncDownloadConcurrency();
    log(tr("下载：同时最多 %1 个文件在下载（对正在下的这一批也生效，已经在下的那几个不会被叫停）。")
            .arg(m_settings->downloadConcurrency()));
}

void LauncherPreferencesController::downloadSpeedLimitChanged() {
    // itemData 存的是 int（见 MainWindow::applySettingsToUi 那段），读回来再转 qint64。
    m_settings->setDownloadSpeedLimit(
        qint64(m_ui->getDownloadSpeedLimitCombo()->currentData().toInt()));
    syncDownloadSpeedLimit();
    const qint64 limit = m_settings->downloadSpeedLimit();
    log(limit > 0
            ? tr("下载：速度上限 %1 KB/s（对正在下的这一批也生效）。")
                  .arg(limit / 1024)
            : tr("下载：不再限制速度。"));
}

void LauncherPreferencesController::downloadProxyModeChanged() {
    const DownloadProxy::Mode mode = static_cast<DownloadProxy::Mode>(
        m_ui->getDownloadProxyCombo()->currentData().toInt());
    m_settings->setProxyMode(mode);
    // 手动地址那一格只在「手动」档露出来（收起来时连位置都不占）。
    m_ui->getDownloadProxyManualEdit()->setVisible(
        m_settings->proxyMode() == DownloadProxy::Mode::Manual);
    syncDownloadProxy();
    switch (m_settings->proxyMode()) {
    case DownloadProxy::Mode::Direct:
        log(tr("网络：不走代理（直连）。"));
        break;
    case DownloadProxy::Mode::Manual:
        log(tr("网络：使用手动指定的代理（%1）。")
                .arg(m_settings->proxyManualText().isEmpty()
                         ? tr("未填写代理地址，当前使用系统代理。")
                         : m_settings->proxyManualText()));
        break;
    case DownloadProxy::Mode::System:
        log(tr("网络：跟随系统代理设置。"));
        break;
    }
}

void LauncherPreferencesController::downloadProxyManualChanged() {
    m_settings->setProxyManualText(m_ui->getDownloadProxyManualEdit()->text());
    const QString text = m_settings->proxyManualText();
    DownloadProxy::Endpoint endpoint;
    QString error;
    // 只在「手动」档校验（另外两档这一格收着，改了也不生效）；空着不弹 —— 用户可能
    // 正要把地址打进去。认不出来时**明确弹一句**（错误要弹窗），并退回系统代理，
    // 而不是把整条网络掐掉。
    if (m_settings->proxyMode() == DownloadProxy::Mode::Manual && !text.isEmpty() &&
        !DownloadProxy::parseManual(text, &endpoint, &error)) {
        InlineMessage::warning(m_ui->overlay(), tr("代理地址写得不对"), error);
        syncDownloadProxy();
        return;
    }
    syncDownloadProxy();
    if (m_settings->proxyMode() != DownloadProxy::Mode::Manual) {
        return;
    }
    if (text.isEmpty()) {
        log(tr("手动代理地址未填写，暂时使用系统代理。"));
        return;
    }
    if (DownloadProxy::parseManual(text, &endpoint)) {
        log(tr("网络：手动代理改为 %1:%2（%3）。")
                .arg(endpoint.host)
                .arg(endpoint.port)
                .arg(endpoint.socks5 ? QStringLiteral("SOCKS5") : QStringLiteral("HTTP")));
    }
}

void LauncherPreferencesController::downloadAutoIntoInstanceChanged() {
    const bool on = m_ui->getDownloadAutoIntoInstanceCheck()->isChecked();
    m_settings->setDownloadAutoIntoInstance(on);
    log(on ? tr("从实例设置下载的模组将直接保存到该实例。")
           : tr("下载：不管从哪儿点进来的，每次都会问存哪儿。"));
}

void LauncherPreferencesController::warnMissingDependenciesChanged() {
    const bool on = m_ui->getWarnMissingDependenciesCheck()->isChecked();
    m_settings->setWarnMissingDependencies(on);
    log(on ? tr("下载：点下载模组 / 光影包时会先检查那个实例缺不缺前置。")
           : tr("仅下载所选版本，跳过依赖检查。"));
}

void LauncherPreferencesController::showGameLogChanged() {
    const bool show = m_ui->getShowGameLogCheck()->isChecked();
    m_settings->setShowGameLog(show);
    log(show ? tr("游戏日志：启动游戏时会开一个日志窗。")
             : tr("游戏日志：不再自动开日志窗。"));
}

void LauncherPreferencesController::warnDuplicateLaunchChanged() {
    m_settings->setWarnDuplicateLaunch(m_ui->getWarnDuplicateLaunchCheck()->isChecked());
}

void LauncherPreferencesController::warnWorldLossChanged() {
    m_settings->setWarnWorldLoss(m_ui->getWarnWorldLossCheck()->isChecked());
}

void LauncherPreferencesController::processPriorityChanged() {
    m_settings->setProcessPriority(
        ProcessPriority(m_ui->getProcessPriorityCombo()->currentData().toInt()));
}

void LauncherPreferencesController::gameWindowSizeChanged() {
    m_settings->setGameWindowSize(m_ui->getGameWindowWidthSpin()->value(),
                                  m_ui->getGameWindowHeightSpin()->value());
}

void LauncherPreferencesController::gameWindowModeChanged() {
    m_settings->setGameWindowMode(
        GameWindowMode(m_ui->getGameWindowModeCombo()->currentData().toInt()));
    updateGameWindowSizeEnabled();
}

void LauncherPreferencesController::updateGameWindowSizeEnabled() {
    const bool enabled = m_settings->gameWindowMode() != GameWindowMode::Maximized;
    m_ui->getGameWindowWidthSpin()->setEnabled(enabled);
    m_ui->getGameWindowHeightSpin()->setEnabled(enabled);
    m_ui->getGameWindowSizeTimesLabel()->setEnabled(enabled);
    // 「恢复默认」跟着这一行的可用性走：整行灰着（最大化）时它也灰，值已是默认时也灰。
    m_ui->syncGameWindowSizeResetButton();
}

void LauncherPreferencesController::syncDownloadSource() {
    m_versions->setUseMirror(m_settings->downloadSource() == DownloadSource::Mirror);
}

void LauncherPreferencesController::syncDownloadConcurrency() {
    m_versions->setDownloadConcurrency(m_settings->downloadConcurrency());
}

void LauncherPreferencesController::syncDownloadSpeedLimit() {
    m_versions->setDownloadSpeedLimit(m_settings->downloadSpeedLimit());
}

void LauncherPreferencesController::syncDownloadProxy() {
    // 这一项和上面几条不一样：它是**进程级**的（QNetworkProxy::setApplicationProxy），
    // 管的不只是下载器 —— 版本清单、皮肤、资源搜索、工具箱全都走它。所以「改完立刻生效」
    // 在这一项上的意思是"下一次请求就走新的"，已经在飞的请求不会被掐断。
    QNetworkProxy proxy;
    const DownloadProxy::Mode mode = m_settings->proxyMode();
    if (mode == DownloadProxy::Mode::Direct) {
        proxy.setType(QNetworkProxy::NoProxy);
    } else if (mode == DownloadProxy::Mode::Manual) {
        DownloadProxy::Endpoint endpoint;
        if (DownloadProxy::parseManual(m_settings->proxyManualText(), &endpoint)) {
            proxy.setType(endpoint.socks5 ? QNetworkProxy::Socks5Proxy
                                          : QNetworkProxy::HttpProxy);
            proxy.setHostName(endpoint.host);
            proxy.setPort(quint16(endpoint.port));
        } else {
            // 地址认不出来（多半还没填）：退回系统代理 —— 宁可走系统那条能通的路，
            // 也不要把网络掐死。界面上那句警告已经把原因说清楚了。
            proxy.setType(QNetworkProxy::DefaultProxy);
        }
    } else {
        // 跟随系统：DefaultProxy 就是 Qt 的默认值。
        proxy.setType(QNetworkProxy::DefaultProxy);
    }
    QNetworkProxy::setApplicationProxy(proxy);
}

void LauncherPreferencesController::syncCatalogCredentials() {
    m_versions->setCurseForgeApiKey(m_settings->curseForgeApiKey());
    m_versions->setCurseForgeApiBaseOverride(m_settings->curseForgeApiBaseOverride());
    m_resources->setCurseForgeApiKey(m_settings->curseForgeApiKey());
    m_resources->setCurseForgeApiBaseOverride(m_settings->curseForgeApiBaseOverride());
    m_versions->setModrinthApiBaseOverride(m_settings->modrinthApiBaseOverride());
    m_resources->setModrinthApiBaseOverride(m_settings->modrinthApiBaseOverride());
}

void LauncherPreferencesController::curseForgeApiKeyEdited() {
    m_settings->setCurseForgeApiKey(m_ui->getCurseForgeApiKeyEdit()->text());
    syncCatalogCredentials();
}

void LauncherPreferencesController::applyExtraJvmArgs() {
    const QString args = m_ui->getExtraJvmArgsEdit()->toPlainText().trimmed();
    m_javaSettings->setExtraJvmArgs(args);
    m_javaSettings->save();
    log(tr("已保存全局 JVM 参数：%1").arg(args.isEmpty() ? tr("（空）") : args));
}
