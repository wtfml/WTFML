#include "controllers/JavaSettingsController.hpp"
#include <QCoreApplication>

#include "controllers/JavaRuntimeController.hpp"
#include "InlineMessage.hpp"
#include "JavaManageDialog.hpp"
#include "MainWindowUI.hpp"
#include "MemorySlider.hpp"
#include "OverlayHost.hpp"
#include "settings/GlobalJavaSettings.hpp"
#include "util/SystemMemory.hpp"
#include "util/AppPaths.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

#include <utility>

namespace {
QString memoryText(qint64 mb) {
    if (mb <= 0) return JavaSettingsController::tr("未知");
    if (mb >= 1024) return QStringLiteral("%1 GB").arg(QString::number(double(mb) / 1024.0, 'f', 1));
    return QStringLiteral("%1 MB").arg(mb);
}
}

JavaSettingsController::JavaSettingsController(MainWindowUI *ui, GlobalJavaSettings *settings,
                                               JavaRuntimeController *runtime,
                                               QWidget *dialogParent, Host host, QObject *parent)
    : QObject(parent), m_ui(ui), m_settings(settings), m_runtime(runtime),
      m_dialogParent(dialogParent), m_host(std::move(host)) {
    connect(runtime, &JavaRuntimeController::busyChanged, this, [this](bool busy) {
        if (m_page != nullptr) m_page->setBusy(busy);
    });
    connect(runtime, &JavaRuntimeController::statusChanged, this,
            [this](const QString &message) { log(message); });
    connect(runtime, &JavaRuntimeController::detectionFinished, this,
            &JavaSettingsController::applyDetected);
    connect(runtime, &JavaRuntimeController::runtimeFinished, this,
            [this](int major, bool ok, const QString &path, bool batch) {
        if (ok) {
            log(tr("Java %1 运行时已装好：%2").arg(major).arg(QDir::toNativeSeparators(path)));
        } else {
            log(tr("Java %1 运行时下载失败").arg(major));
            if (!batch && m_page != nullptr && m_page->isVisible())
                InlineMessage::warning(m_ui->overlay(), tr("Java 运行时下载失败"),
                    tr("Java %1 未从 Adoptium 下载成功。\n请重试，或在安装 Java 后选择「检测已有 Java」。也可在实例设置中指定 Java。")
                        .arg(major));
        }
    });
    connect(runtime, &JavaRuntimeController::downloadSequenceFinished, this,
            [this](bool batch, int okCount, int failCount, bool cancelled) {
        if (cancelled) log(tr("Java 下载队列已停止；当前下载完成后没有再开始下一份。"));
        if (batch && failCount > 0 && m_page != nullptr && m_page->isVisible())
            InlineMessage::warning(m_ui->overlay(), tr("部分 Java 下载失败"),
                tr("已下载 %1 个 Java 运行时，%2 个下载失败。\n请重试，或手动安装后选择「检测已有 Java」。")
                    .arg(okCount).arg(failCount));
    });

    // 自动分配要"随系统可用内存更新"：设置页露着的时候每 2 秒重读一次并刷新展示。
    // 只在自动档 + 设置页可见时跑（见 refreshMemory() 末尾）—— 手动档下读系统内存
    // 没有任何意义，页不在眼前时更新也没人看。
    m_memoryRefreshTimer = new QTimer(this);
    m_memoryRefreshTimer->setInterval(2000);
    connect(m_memoryRefreshTimer, &QTimer::timeout, this, [this]() {
        if (!m_settings->autoMemory() || !memoryPageVisible()) {
            m_memoryRefreshTimer->stop();
            return;
        }
        refreshMemory();
    });
}

bool JavaSettingsController::memoryPageVisible() const {
    QTabWidget *tabs = m_ui->getTabWidget();
    QComboBox *mode = m_ui->getMemoryModeCombo();
    return tabs != nullptr && mode != nullptr &&
           tabs->currentIndex() == MainWindowUI::settingsTabIndex() && mode->isVisible();
}

void JavaSettingsController::log(const QString &message) const {
    if (m_host.log) m_host.log(message);
}

void JavaSettingsController::openManagePage() {
    OverlayHost *overlay = m_ui->overlay();
    if (overlay == nullptr) return;
    if (m_page == nullptr) {
        m_page = new JavaManageDialog(m_dialogParent);
        connect(m_page, &JavaManageDialog::downloadRequested, this,
                &JavaSettingsController::download);
        connect(m_page, &JavaManageDialog::downloadAllMissingRequested, this,
                &JavaSettingsController::downloadAllMissing);
        connect(m_page, &JavaManageDialog::detectRequested, this, &JavaSettingsController::detect);
        connect(m_page, &JavaManageDialog::useAutoRequested, this, &JavaSettingsController::useAuto);
        connect(m_page, &JavaManageDialog::useRuntimeRequested, this,
                &JavaSettingsController::useRuntime);
        connect(m_page, &JavaManageDialog::manualPathRequested, this,
                &JavaSettingsController::browseManualJava);
        connect(m_page, &QDialog::rejected, m_runtime,
                &JavaRuntimeController::cancelAfterCurrent);
    }
    pushRows();
    m_page->setBusy(m_runtime->isBusy());
    if (!m_runtime->isBusy()) m_runtime->detectInstalled();
    const int settingsTab = MainWindowUI::settingsTabIndex();
    if (QTabWidget *tabs = m_ui->getTabWidget(); settingsTab >= 0 && settingsTab < tabs->count())
        tabs->setCurrentIndex(settingsTab);
    // 非阻塞开页（见 `OverlayHost::showPage()`）：这一页后面没有一行代码要等它收掉；
    // 页上那一发 `rejected` 照旧接在 `cancelAfterCurrent` 上（返回箭头也是 reject）。
    if (!overlay->showPage(m_page, m_page->titleText(), settingsTab)) {
        log(tr("无法打开 Java 管理页：界面已被另一页占用。"));
    }
}

void JavaSettingsController::resetDiscovery() {
    ++m_discoveryGeneration;
    m_discoveryPending = false;
    m_scanned = false;
    m_versions.clear();
}

void JavaSettingsController::refreshList() {
    if (m_scanned) return;
    m_scanned = true;
    const QString root = AppPaths::javaDir();
    const auto provider = m_host.scanJava;
    queueDiscovery([root, provider] {
        return provider ? provider(root) : GlobalJavaSettings::scanLocalJavaVersions(root);
    });
}

void JavaSettingsController::applyDetected(const QMap<int, QString> &versions) {
    // 显式检测/下载结果优先于先前自动扫描；自定义 Java 探测也不放回 GUI 线程。
    m_scanned = true;
    const QString pinned = m_settings->javaPath();
    if (pinned.isEmpty() || versions.values().contains(pinned)) {
        ++m_discoveryGeneration;
        m_discoveryPending = false;
        m_versions = versions;
        pushRows();
        emit discoveryFinished();
        return;
    }
    queueDiscovery([versions] { return versions; });
}

void JavaSettingsController::queueDiscovery(std::function<QMap<int, QString>()> scan) {
    const quint64 generation = ++m_discoveryGeneration;
    const QString root = AppPaths::javaDir();
    const QString pinned = m_settings->javaPath();
    m_discoveryPending = true;
    auto *watcher = new QFutureWatcher<QMap<int, QString>>(this);
    connect(watcher, &QFutureWatcher<QMap<int, QString>>::finished, this,
            [this, watcher, generation, root, pinned] {
        const auto versions = watcher->result();
        watcher->deleteLater();
        if (generation != m_discoveryGeneration) return;
        m_discoveryPending = false;
        if (root != AppPaths::javaDir() || pinned != m_settings->javaPath()) {
            m_scanned = false;
            refreshList();
            return;
        }
        m_versions = versions;
        pushRows();
        emit discoveryFinished();
    });
    // 手动档下钉的可能是一条扫描压根不认识的路径（另一个盘上的 JDK）。它必须占住自己
    // 主版本那一行 —— 否则同一主版本里"扫描认得的那一条"会显示成正在用的，而实际上
    // 盘上钉的是另一条。自动档照旧：宁可显示扫描结果，也不拿"上次用过"那条冒充。
    const bool manualPinned = !m_settings->autoSelectJava();
    watcher->setFuture(QtConcurrent::run([scan = std::move(scan), pinned, manualPinned] {
        auto versions = scan();
        if (!pinned.isEmpty() && !versions.values().contains(pinned)) {
            const int major = GlobalJavaSettings::probeJavaMajorVersion(pinned);
            if (major > 0 && (manualPinned || !versions.contains(major)))
                versions.insert(major, pinned);
        }
        return versions;
    }));
}

QString JavaSettingsController::rowPathFor(int major) const {
    // 手动挑到的那一份要盖在扫描结果上面 —— 否则同一主版本里扫描认得的那条会把
    // 用户刚钉的那条挤掉，「（当前）」记号没处挂，看起来像没生效。
    // 只认"它还是盘上钉的那条路径"：外面（启动/设置）换了路径，这一格就不作数。
    const bool manualPinned = !m_settings->autoSelectJava() && m_manualMajor > 0 &&
                              !m_manualPath.isEmpty() &&
                              m_manualPath == m_settings->javaPath();
    if (manualPinned && major == m_manualMajor) return m_manualPath;
    return m_versions.value(major);
}

void JavaSettingsController::pushRows() {
    if (m_page == nullptr) return;
    for (int major : JavaManageDialog::managedMajors())
        m_page->setRuntime(major, rowPathFor(major));
    m_page->setSelection(m_settings->autoSelectJava(), m_settings->javaPath());
}

void JavaSettingsController::updatePathLabel() {
    QLabel *label = m_ui->getJavaPathLabel();
    const QString path = m_settings->javaPath();
    if (!m_settings->autoSelectJava())
        label->setText(path.isEmpty() ? tr("手动指定：未选择路径，启动将失败。")
                                      : tr("手动指定：%1").arg(path));
    else
        label->setText(path.isEmpty() ? tr("自动选择　按游戏版本")
                                      : tr("自动选择　上次使用 %1").arg(path));
}

void JavaSettingsController::useAuto() {
    if (m_page == nullptr) return;
    // 换档之后"手动那份"不再代表盘上的状态 —— 再留着它就是拿旧值盖新值。
    // 还在后台认主版本的那一发也作废：用户后来这一下说了算。
    ++m_manualGeneration;
    m_manualPath.clear();
    m_manualMajor = 0;
    m_settings->setAutoSelectJava(true);
    m_settings->save();
    updatePathLabel();
    pushRows();
}

void JavaSettingsController::useRuntime(int major) {
    // 钉的必须是**这一行画着的那条**。手动那份盖在扫描结果上面时，m_versions 里那一格
    // 还是扫描认得的老路径 —— 按它钉，用户看到的是 A、盘上写的是 B。
    if (m_page == nullptr) return;
    const QString path = rowPathFor(major);
    if (path.isEmpty()) return;
    // 还在后台认主版本的那一发作废（用户直接点了「选用」，那一下更新）。
    ++m_manualGeneration;
    // 记成"手动那份"：这一行接下来仍要画它（也许它压根不在扫描结果里）。
    m_manualPath = path;
    m_manualMajor = major;
    m_settings->setAutoSelectJava(false);
    m_settings->setJavaPath(path);
    m_settings->save();
    updatePathLabel();
    pushRows();
}

void JavaSettingsController::browseManualJava() {
    if (m_page == nullptr) return;
    const QString current = m_settings->javaPath();
    // 从正在用的那一份旁边开始找：用户多半就是把启动器带的运行时换成隔壁装的那份。
    const QString startDir = current.isEmpty() ? QString() : QFileInfo(current).absolutePath();
    const QString path = QFileDialog::getOpenFileName(
        m_dialogParent, tr("选择 java.exe"), startDir,
        tr("Java 可执行文件 (java.exe);;所有文件 (*)"));
    if (!path.isEmpty()) setManualJava(path);
}

void JavaSettingsController::setManualJava(const QString &path) {
    const QFileInfo info(path.trimmed());
    if (info.fileName().isEmpty()) return;
    if (!info.isFile()) {
        log(tr("找不到手动指定的 Java：%1").arg(QDir::toNativeSeparators(info.filePath())));
        InlineMessage::warning(
            m_ui->overlay(), tr("没选到 java.exe"),
            tr("这条路线上没有那个文件：\n%1\n\n要选的是 Java 安装目录下 bin 里的 java.exe。")
                .arg(QDir::toNativeSeparators(info.filePath())));
        return;
    }
    // 绝对路径：盘上记的、行里画的、以后启动比对的都是它（用户从框里选回来的通常
    // 已经是绝对路径，这一句是给测试和粘进来的相对路径兜底）。
    const QString absolute = info.absoluteFilePath();
    // 认主版本可能要真跑一次 `java -version`（最多 5 秒），所以先进后台；日志里留一句，
    // 免得那几秒里用户以为这一下没反应。
    log(tr("正在识别 Java 主版本：%1").arg(QDir::toNativeSeparators(absolute)));
    const quint64 generation = ++m_manualGeneration;
    auto *watcher = new QFutureWatcher<int>(this);
    connect(watcher, &QFutureWatcher<int>::finished, this,
            [this, watcher, generation, absolute] {
                watcher->deleteLater();
                if (generation != m_manualGeneration) return;
                const int major = watcher->result();
                if (major <= 0) {
                    log(tr("无法识别 Java 主版本：%1")
                            .arg(QDir::toNativeSeparators(absolute)));
                    InlineMessage::warning(
                        m_ui->overlay(), tr("这不是一份能用的 Java"),
                        tr("无法识别 %1 的 Java 主版本。\n请从 Java 安装目录的 bin 文件夹中选择可运行的 java.exe。")
                            .arg(QDir::toNativeSeparators(absolute)));
                    return;
                }
                m_manualPath = absolute;
                m_manualMajor = major;
                m_settings->setAutoSelectJava(false);
                m_settings->setJavaPath(absolute);
                m_settings->save();
                updatePathLabel();
                pushRows();
                log(tr("全局 Java 已手动指定为 Java %1：%2")
                        .arg(major)
                        .arg(QDir::toNativeSeparators(absolute)));
            });
    watcher->setFuture(QtConcurrent::run([absolute] {
        return GlobalJavaSettings::probeJavaMajorVersion(absolute);
    }));
}

void JavaSettingsController::detect() { if (m_page != nullptr) m_runtime->detectInstalled(); }
void JavaSettingsController::download(int major) { if (m_page != nullptr && major > 0) m_runtime->downloadRuntime(major); }

void JavaSettingsController::downloadAllMissing() {
    if (m_page == nullptr) return;
    QList<int> missing;
    for (int major : JavaManageDialog::managedMajors())
        if (m_versions.value(major).isEmpty()) missing.append(major);
    m_runtime->downloadMissing(missing);
}

void JavaSettingsController::applyMemoryLimits(qint64 totalMb) {
    const int cap = MainWindowUI::memoryCapForTotalMb(totalMb);
    const int min = MainWindowUI::memoryMaxMinMb();
    const QSignalBlocker blockSpin(m_ui->getMemoryMaxSpin());
    const QSignalBlocker blockSlider(m_ui->getMemoryMaxSlider());
    m_ui->getMemoryMaxSpin()->setRange(min, cap);
    m_ui->getMemoryMaxSlider()->setRangeMb(min, cap);
}

bool JavaSettingsController::clampXmsToXmx(int xmx) {
    if (m_settings->xms() <= xmx) return false;
    m_settings->setXms(xmx);
    return true;
}

void JavaSettingsController::setMemoryModeFromUi() {
    QComboBox *mode = m_ui->getMemoryModeCombo();
    if (mode == nullptr) return;
    const bool wantAuto = mode->currentData().toBool();
    if (wantAuto == m_settings->autoMemory()) {
        refreshMemory();
        return;
    }
    if (wantAuto) {
        // 这一句里面把当前 Xmx/Xms 记成"上次手动档的现场"并落盘。之后 refreshMemory()
        // 才按系统内存重算 —— 顺序不能反，反了记下的就是自动值。
        m_settings->setAutoMemory(true);
    } else {
        m_settings->restoreManualMemory();
    }
    refreshMemory();
}

void JavaSettingsController::refreshMemory() {
    const bool autoMode = m_settings->autoMemory();
    const int beforeXmx = m_settings->xmx();
    const int beforeXms = m_settings->xms();

    // 自动档：**现算**（`refreshSuggestedMemory()` 会读系统内存并改写 Xmx/Xms）；
    // 手动档：只读一次系统内存写说明，**一个数字都不许动**。
    const SystemMemory::Usage usage =
        autoMode ? m_settings->refreshSuggestedMemory() : SystemMemory::read();
    applyMemoryLimits(usage.totalMb);

    const int cap = m_ui->getMemoryMaxSpin()->maximum();
    int xmx = m_settings->xmx();
    if (xmx > cap) {
        log(tr("内存：%1 %2 MB 超过这台机器的物理内存上限，已降到 %3 MB。")
                .arg(autoMode ? tr("自动给出的最大内存") : tr("已保存的最大内存"))
                .arg(xmx)
                .arg(cap));
        m_settings->setXmx(cap);
        xmx = cap;
    }
    if (clampXmsToXmx(xmx)) {
        log(tr("内存：最小内存不能超过最大内存，已跟着降到 %1 MB。").arg(xmx));
    }
    if (m_settings->xmx() != beforeXmx || m_settings->xms() != beforeXms) {
        // 自动值变了才落盘（定时器每 2 秒跑一次，不能每趟都写注册表）。
        m_settings->save();
    }

    const QSignalBlocker blockSpin(m_ui->getMemoryMaxSpin());
    const QSignalBlocker blockSlider(m_ui->getMemoryMaxSlider());
    const QSignalBlocker blockMode(m_ui->getMemoryModeCombo());
    if (QComboBox *mode = m_ui->getMemoryModeCombo()) {
        const int index = mode->findData(autoMode);
        if (index >= 0) mode->setCurrentIndex(index);
    }
    m_ui->getMemoryMaxSpin()->setValue(xmx);
    m_ui->getMemoryMaxSpin()->setEnabled(!autoMode);
    m_ui->getMemoryMaxSlider()->setEnabled(!autoMode);
    if (autoMode) {
        // 把手落在**自动值**（= 虚条的头）上。停在"上次手动档"那个位置的话，自动值比它小
        // 时圆圈就浮在一段没填的轨道上，看着像圆圈跟条脱开了（用户报的就是这个）。上次手动
        // 的值写在下面那句说明里（"切回手动分配会恢复 N MB"）。
        m_ui->getMemoryMaxSlider()->setValueMb(xmx);
        m_ui->getMemoryMaxSlider()->setAutoValueMb(xmx);
    } else {
        m_ui->getMemoryMaxSlider()->clearAutoValue();
        m_ui->getMemoryMaxSlider()->setValueMb(xmx);
    }

    QString hint;
    if (autoMode) {
        if (usage.totalMb <= 0) {
            hint = tr("无法读取系统内存，自动分配暂用 %1 MB。切回「手动分配」可恢复之前设置的 %2 MB。")
                       .arg(xmx)
                       .arg(m_settings->manualXmx());
        } else {
            hint = tr("系统总内存为 %1，当前可用 %2。自动分配最大 %3 MB、最小 %4 MB，随可用内存变化。切回「手动分配」可恢复之前设置的 %5 MB。")
                       .arg(memoryText(usage.totalMb), memoryText(usage.availableMb))
                       .arg(xmx)
                       .arg(m_settings->xms())
                       .arg(m_settings->manualXmx());
        }
    } else if (usage.totalMb <= 0) {
        hint = tr("无法读取系统内存，请在右侧直接填写数值。");
    } else {
        hint = tr("系统总内存 %1，当前可用 %2；最小内存由启动器设为 %3 MB。")
                   .arg(memoryText(usage.totalMb), memoryText(usage.availableMb))
                   .arg(m_settings->xms());
    }
    m_ui->getMemoryHint()->setText(hint);

    // 自动档在设置页露着的时候才需要每 2 秒刷新一次；其他情况立刻停掉。
    if (autoMode && memoryPageVisible()) {
        m_memoryRefreshTimer->start();
    } else {
        m_memoryRefreshTimer->stop();
    }
}

void JavaSettingsController::setWarnMismatchFromUi() {
    const bool warn = m_ui->getJavaWarnMismatchCheck()->isChecked();
    m_settings->setWarnVersionMismatch(warn);
    log(warn ? tr("实例指定的 Java 版本不符时：会提醒。")
             : tr("实例指定的 Java 版本不符时：不再提醒，直接按它指定的 Java 启动。"));
}

void JavaSettingsController::commitMemoryFromSpin() {
    // 自动档下这两个控件是禁用的，值由 refreshMemory() 写；万一有谁绕过程序改了信号，
    // 也不能把自动算出来的值当成用户手填的写回盘。
    if (m_settings->autoMemory()) return;
    const int value = m_ui->getMemoryMaxSpin()->value();
    if (m_ui->getMemoryMaxSlider()->valueMb() != value) {
        const QSignalBlocker block(m_ui->getMemoryMaxSlider());
        m_ui->getMemoryMaxSlider()->setValueMb(value);
    }
    m_settings->setXmx(value);
    clampXmsToXmx(value);
    m_settings->save();
    refreshMemory();
}

void JavaSettingsController::syncMemorySlider() {
    if (m_settings->autoMemory()) return;
    const int value = m_ui->getMemoryMaxSlider()->valueMb();
    if (m_ui->getMemoryMaxSpin()->value() != value) {
        const QSignalBlocker block(m_ui->getMemoryMaxSpin());
        m_ui->getMemoryMaxSpin()->setValue(value);
    }
    if (!m_ui->getMemoryMaxSlider()->isSliderDown()) commitMemoryFromSpin();
}
